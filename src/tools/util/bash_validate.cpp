// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/bash_validate.hpp>
#include <mcp/tools/util/shellx.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::tools::util {

namespace {

std::string_view first_token(std::string_view cmd) noexcept {
    size_t i = 0;
    while (i < cmd.size() && (cmd[i] == ' ' || cmd[i] == '\t')) i++;
    size_t start = i;
    while (i < cmd.size() && cmd[i] != ' ' && cmd[i] != '\t'
           && cmd[i] != '|' && cmd[i] != '&' && cmd[i] != ';') i++;
    auto tok = cmd.substr(start, i - start);
    size_t slash = tok.find_last_of("/\\");
    if (slash != std::string_view::npos) tok.remove_prefix(slash + 1);
    if (tok.size() > 4
        && (tok.ends_with(".exe") || tok.ends_with(".EXE")
            || tok.ends_with(".cmd") || tok.ends_with(".bat")))
        tok.remove_suffix(4);
    return tok;
}

// ── Destructive-rm analysis ───────────────────────────────────────
//
// This gate used to scan for the literal prefixes "rm -rf " / "rm -fr " /
// "rm -r -f " / "rm -f -r ". A probe of 22 filesystem-destroying commands
// found 19 of them walked straight through — `rm -Rf /` bypassed it on a
// single capital letter, as did `rm -rfv /`, `rm --recursive --force /`,
// `rm / -rf` (flags after the path), `rm -rf /tmp/../../` (traversal back to
// root), `rm -rf /tmp /` (one legit path hiding a fatal one), `rm -rf $HOME`
// and `rm -rf .`.
//
// A prefix list cannot be made complete: flag spellings are combinatorial,
// order is free, and the dangerous part is the resolved PATH, not the text.
// So tokenize the command, collect flags and paths separately regardless of
// order, normalize each path (resolving `..` the way the kernel will), and
// judge each resolved target on its own — the approach Zed's agent takes,
// minus its bash AST, which is more machinery than this one check needs.

// Split on whitespace, honouring quotes so a path with a space stays one
// token and a quoted string is never mistaken for syntax.
std::vector<std::string> shell_tokens(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char quote = '\0';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { cur += s[++i]; have = true; continue; }
        if (quote) {
            if (c == quote) quote = '\0';
            else { cur += c; }
            have = true;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; have = true; continue; }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (have) { out.push_back(cur); cur.clear(); have = false; }
            continue;
        }
        cur += c;
        have = true;
    }
    if (have) out.push_back(cur);
    return out;
}

// Resolve `.` and `..` lexically, the way the kernel will when it walks the
// path. `/tmp/../..` is the root; not resolving it is how a traversal slips
// past a gate that only compares text.
std::string normalize_path(std::string_view p) {
    const bool absolute = !p.empty() && (p.front() == '/' || p.front() == '\\');
    std::vector<std::string_view> parts;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find_first_of("/\\", i);
        auto seg = p.substr(i, j == std::string_view::npos ? j : j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            // Popping past the root stays at the root — `/..` is `/`, which
            // is exactly the case that must stay dangerous.
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    std::string out;
    if (absolute) out += '/';
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += '/';
        out += std::string{parts[k]};
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

// Is this resolved target catastrophic to delete recursively?
std::string_view rm_target_danger(std::string_view raw) {
    // Trailing glob: `/`, `/*`, `~/*` all destroy the same tree.
    std::string_view t = raw;
    if (t.size() >= 2 && t.ends_with("*")) t.remove_suffix(1);

    // Environment spellings of the home directory, before normalization —
    // the shell would expand these, and `$HOME/../..` is the root.
    for (std::string_view home : {"$HOME", "${HOME}", "~"}) {
        if (t == home) return "refusing to recursively delete the home directory";
        if (t.size() > home.size() && t.starts_with(home)
            && (t[home.size()] == '/' || t[home.size()] == '\\')) {
            auto rest = normalize_path(t.substr(home.size()));
            if (rest == "/" || rest == ".")
                return "refusing to recursively delete the home directory";
            // `$HOME/../..` climbs above home into the filesystem root.
            size_t up = 0;
            for (size_t k = home.size(); k + 1 < t.size(); ++k)
                if (t[k] == '.' && t[k + 1] == '.') { ++up; ++k; }
            if (up >= 2)
                return "refusing wide rm that could wipe the filesystem root";
            return {};
        }
    }
    if (t.starts_with("$")) return {};   // some other variable — unknowable

    const auto norm = normalize_path(t);
    if (norm == "/")
        return "refusing wide rm that could wipe the filesystem root";
    // `.` and `..` as a whole target wipe the working tree — the repo the
    // agent is editing. Zed blocks these for the same reason.
    if (norm == ".")
        return "refusing `rm -r .` — it would delete the working directory";
    return {};
}

// Does this token enable recursion? Covers -r/-R/--recursive and any short
// cluster containing r (-rf, -Rfv, -fR…), which is where the prefix list
// failed: `rm -Rf /` differs from `rm -rf /` by one capital letter.
bool is_recursive_flag(std::string_view t) {
    if (t == "--recursive") return true;
    if (t.size() < 2 || t[0] != '-' || t[1] == '-') return false;
    for (size_t i = 1; i < t.size(); ++i)
        if (t[i] == 'r' || t[i] == 'R') return true;
    return false;
}

// Scan ONE command (no chaining operators) for a catastrophic rm.
std::string check_rm_command(const std::vector<std::string>& tok) {
    if (tok.empty()) return {};
    // The command word may be a path (`/bin/rm`) or have an .exe suffix.
    std::string_view name = tok.front();
    if (auto slash = name.find_last_of("/\\"); slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    if (name != "rm") return {};

    bool recursive = false, end_of_flags = false;
    std::vector<std::string_view> paths;
    for (size_t i = 1; i < tok.size(); ++i) {
        std::string_view t = tok[i];
        if (!end_of_flags && t == "--") { end_of_flags = true; continue; }
        // Flags are collected wherever they appear: `rm / -rf` puts them
        // AFTER the path, which the old prefix scan could never see.
        if (!end_of_flags && t.size() > 1 && t[0] == '-') {
            if (is_recursive_flag(t)) recursive = true;
            continue;
        }
        paths.push_back(t);
    }
    if (!recursive) return {};
    // Every path is judged on its own, so one fatal target cannot hide
    // behind a legitimate one (`rm -rf /tmp /`).
    for (auto p : paths)
        if (auto why = rm_target_danger(p); !why.empty()) return std::string{why};
    return {};
}

// Split a command line on chaining operators so each simple command is
// checked separately — `make && rm -rf /` must not hide behind the `make`.
std::string check_rm_anywhere(std::string_view cmd) {
    size_t start = 0;
    char quote = '\0';
    auto flush = [&](size_t end) -> std::string {
        return check_rm_command(shell_tokens(cmd.substr(start, end - start)));
    };
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == '\\') { ++i; continue; }
        if (quote) { if (c == quote) quote = '\0'; continue; }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == ';' || c == '|' || c == '&' || c == '\n') {
            if (auto why = flush(i); !why.empty()) return why;
            start = i + 1;
        }
    }
    return flush(cmd.size());
}

}  // namespace

std::string validate_bash_command(std::string_view cmd) {
    auto tok = first_token(cmd);
    static const std::vector<std::string_view> always_interactive = {
        "vim", "vi", "nvim", "nano", "emacs", "pico", "ed", "joe", "mcedit",
        "less", "more", "man", "top", "htop", "btop", "tmux", "screen",
        "mysql", "psql", "sqlite3", "redis-cli", "mongo",
        "ghci", "ocaml", "irb", "pry", "lua", "tclsh", "gdb", "lldb",
        "fzf", "dialog", "whiptail",
    };
    for (auto name : always_interactive) {
        if (tok == name)
            return "refusing to run interactive command '" + std::string{name}
                 + "' — it would block waiting for stdin. Use a non-interactive "
                   "alternative (e.g. for editors: use the write/edit tools).";
    }
    static const std::vector<std::string_view> interactive_if_bare = {
        "python", "python3", "node", "deno", "ruby", "php", "iex", "bash",
        "sh", "zsh", "fish", "pwsh", "powershell", "cmd",
    };
    for (auto name : interactive_if_bare) {
        if (tok != name) continue;
        auto rest = cmd.substr(cmd.find(tok) + tok.size());
        bool has_more = false;
        for (char c : rest) if (c != ' ' && c != '\t' && c != '\n') { has_more = true; break; }
        if (!has_more)
            return "refusing to start interactive " + std::string{name}
                 + " REPL — it would block waiting for stdin. Provide a script "
                   "path or use `-c \"…\"` to run a snippet.";
    }
    auto contains_word = [&](std::string_view needle) {
        size_t p = 0;
        while ((p = cmd.find(needle, p)) != std::string::npos) {
            bool left_ok  = p == 0 || cmd[p - 1] == ' ' || cmd[p - 1] == '\t';
            bool right_ok = p + needle.size() == cmd.size()
                         || cmd[p + needle.size()] == ' '
                         || cmd[p + needle.size()] == '\t';
            if (left_ok && right_ok) return true;
            p += needle.size();
        }
        return false;
    };
    if (tok == "git") {
        if (contains_word("rebase") && contains_word("-i"))
            return "refusing to run interactive rebase (`git rebase -i`) — the editor "
                   "would block the agent. Use non-interactive rebase options instead.";
        if (contains_word("add") && (contains_word("-i") || contains_word("-p")
                                     || contains_word("--interactive")
                                     || contains_word("--patch")))
            return "refusing to run interactive git add — use explicit file paths.";
        if (contains_word("commit")
            && !contains_word("-m") && !contains_word("-F")
            && !contains_word("--message") && !contains_word("--file")
            && !contains_word("--amend") && !contains_word("-C")
            && !contains_word("--no-edit"))
            return "refusing `git commit` without -m/-F — it would open an editor. "
                   "Pass -m \"<message>\" to commit non-interactively, or use the "
                   "git_commit tool.";
    }
    // Root-wipe / home-wipe / working-tree-wipe, judged on the RESOLVED
    // target of every `rm -r` in the line (see check_rm_anywhere above).
    if (auto why = check_rm_anywhere(cmd); !why.empty()) return why;
    static const std::vector<std::pair<std::string_view, std::string_view>> danger = {
        {":(){ :|:& };:",          "fork-bomb pattern refused"},
        {"mkfs",                   "refusing mkfs — would reformat a filesystem"},
        {"dd if=",                 "refusing raw `dd` write — can corrupt disks if misdirected"},
        {"shutdown",               "refusing shutdown"},
        {"reboot",                 "refusing reboot"},
        {"git push --force",       "refusing `git push --force`; use --force-with-lease and ask the user first"},
        {"git push -f",            "refusing `git push -f`; use --force-with-lease and ask the user first"},
    };
    for (const auto& [needle, msg] : danger) {
        if (cmd.find(needle) != std::string::npos) return std::string{msg};
    }
    auto piped_to_shell = [&](std::string_view prog) {
        size_t p = cmd.find(prog);
        while (p != std::string::npos) {
            size_t pipe = cmd.find('|', p);
            if (pipe == std::string::npos) break;
            auto rest = cmd.substr(pipe + 1);
            size_t i = 0;
            while (i < rest.size() && (rest[i] == ' ' || rest[i] == '|')) i++;
            auto next = first_token(rest.substr(i));
            if (next == "sh" || next == "bash" || next == "zsh"
                || next == "dash" || next == "ksh")
                return true;
            p = cmd.find(prog, pipe);
        }
        return false;
    };
    if (piped_to_shell("curl") || piped_to_shell("wget"))
        return "refusing `curl|sh` / `wget|sh` — download the script, inspect it, then run explicitly.";
    return {};
}

// ── Native-tool detour detection ────────────────────────────────────────
//
// Built on the tree-sitter parse (shellx::analyze), not a byte scanner. The
// old scanner had to guess what a quote, a `$`, a `<` or a `>` meant; the
// parse knows. So `grep '$HOME' src` is a literal search, `cat $HOME/f` is
// an expansion, `cat <<EOF > f` is a write, and `echo "grep x"` runs echo.
//
// Advisory only. Nothing here runs anything in place of the shell: the
// verdict feeds a tip, and substitutable() is the ONE read/write gate for
// any caller that ever wants to act on it.

namespace {

namespace sx = shellx;

std::string_view base_name(std::string_view p) noexcept {
    if (auto s = p.find_last_of('/'); s != std::string_view::npos) p.remove_prefix(s + 1);
    return p;
}

// `2>/dev/null` and `2>&1` are noise control, not work. The model adds them
// because a missing path makes the shell shout; a native tool just says
// "no matches". Every other redirect is either a write or an input the
// native tool can't take.
bool noise_redirect(const sx::Redirect& r) noexcept {
    if (r.kind == sx::Redirect::Kind::DupFd) return true;
    const std::string* t = sx::lit(r.target);
    return t && *t == "/dev/null" && r.kind != sx::Redirect::Kind::In;
}

// Does this command WRITE? Checked on every command in the script, nested
// ones included, before anything else. The one mistake that can't be
// undone is calling a write a read.
bool writes(const sx::Command& c) {
    for (const auto& r : c.redirects)
        if (r.writes_file()) return true;
    const auto p = base_name(c.program());
    if (p == "tee" || p == "truncate" || p == "dd" || p == "sponge") return true;
    auto arg_is = [&](auto pred) {
        for (std::size_t i = 1; i < c.argv.size(); ++i) {
            const std::string_view a = sx::spelling(c.argv[i]);
            if (a == "--") break;
            if (pred(a)) return true;
        }
        return false;
    };
    // sed/perl/ruby -i, -i.bak, -pi, -ni, --in-place: a short-flag cluster
    // (letters up to the first non-letter) that contains `i`.
    auto inplace = [](std::string_view a) {
        if (a.starts_with("--in-place")) return true;
        if (a.size() < 2 || a[0] != '-' || a[1] == '-') return false;
        for (std::size_t k = 1; k < a.size(); ++k) {
            const char ch = a[k];
            if (ch == 'i') return true;
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) break;
        }
        return false;
    };
    if (p == "sed" || p == "perl" || p == "ruby" || p == "gsed")
        return arg_is(inplace);
    if (p == "awk" || p == "gawk")
        return arg_is([](std::string_view a) { return a == "-i" || a == "inplace"; });
    if (p == "sort")
        return arg_is([](std::string_view a) { return a == "-o" || a.starts_with("--output"); });
    return false;
}

// A pipe stage that only BOUNDS the output: `head -N`, `head -n N`, `tail -N`.
// No file argument, no other flag. That is a result limit, which every
// native tool takes as a parameter; the model pipes because it doesn't
// know that.
std::optional<Bound> as_bound(const sx::Command& c) {
    const auto p = base_name(c.program());
    if (p != "head" && p != "tail") return std::nullopt;
    if (!c.redirects.empty() || !c.env.empty()) return std::nullopt;
    int n = 0;
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* a = sx::lit(c.argv[i]);
        if (!a) return std::nullopt;
        std::string_view v = *a;
        if (v == "-n" && i + 1 < c.argv.size()) {
            const std::string* nx = sx::lit(c.argv[++i]);
            if (!nx) return std::nullopt;
            v = *nx;
        } else if (v.starts_with("-n")) v.remove_prefix(2);
        else if (v.starts_with("-")) v.remove_prefix(1);
        else return std::nullopt;          // a file: reading, not bounding
        if (v.empty() || v.find_first_not_of("0123456789") != std::string_view::npos)
            return std::nullopt;           // -f, -c, +K …: not a plain limit
        n = 0;
        for (char ch : v) n = n * 10 + (ch - '0');
    }
    if (n <= 0) return std::nullopt;
    return Bound{n, p == "tail"};
}

bool has_flag(const sx::Command& c, std::string_view shortf, std::string_view longf) {
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string_view a = sx::spelling(c.argv[i]);
        if (a == "--") break;
        if (!longf.empty() && a == longf) return true;
        if (a.size() > 1 && a[0] == '-' && a[1] != '-' && a.find(shortf) != std::string_view::npos)
            return true;
    }
    return false;
}

}  // namespace

Detour analyze_detour(std::string_view cmd) {
    Detour d;
    const sx::Script s = sx::analyze(cmd);

    // Writes first, over EVERY command (nested, substituted, conditional).
    for (const auto& c : s.commands)
        if (writes(c)) {
            d.intent = Intent::Write;
            d.needs_shell = true;
            return d;
        }
    // A parse we can't fully trust gets no advice. Silence is always safe.
    if (!s.clean || s.truncated || s.commands.empty()) { d.needs_shell = true; return d; }

    // One pipeline, no chaining, no nesting: `a && b`, `a; b`, `$(…)`,
    // loops, subshells and `&` are shell work. Shell work can still end in
    // `| tail -20`, and that bound has a native answer (head_lines /
    // tail_lines), so recover it before going quiet.
    const auto tops = s.top_level();
    auto shell_work = [&]() -> Detour& {
        d.needs_shell = true;
        if (!tops.empty() && tops.back()->stage > 0)
            if (auto b = as_bound(*tops.back())) d.bound = *b;
        return d;
    };
    if (tops.size() != s.commands.size()) return shell_work();
    for (const auto* c : tops)
        if (c->pipeline != tops.front()->pipeline
            || (c->ctx != sx::Ctx::None && c->ctx != sx::Ctx::Piped)
            || c->join == sx::Join::PipeErr)
            return shell_work();

    const sx::Command& head = *tops.front();
    // Expansion ($X, $(…), ~, globs in the program) is a value only the
    // shell knows. A quoted `$` is a Lit, so `grep '$HOME' src` passes.
    // Globs in ARGUMENTS are fine for ls/find (they describe a pattern the
    // native tools also take); anything else Dyn bails.
    for (const auto* c : tops) {
        if (!c->env.empty()) { d.needs_shell = true; return d; }
        for (std::size_t i = 0; i < c->argv.size(); ++i)
            if (const auto* dy = std::get_if<sx::Dyn>(&c->argv[i])) {
                const bool glob = dy->why == sx::Dyn::Why::Glob || dy->why == sx::Dyn::Why::Brace;
                if (!(glob && i > 0 && c == &head)) { d.needs_shell = true; return d; }
            }
        for (const auto& r : c->redirects)
            if (!noise_redirect(r)) { d.needs_shell = true; return d; }   // `< f`, heredoc
    }

    // Trailing stages that only bound become a Bound; any real transform
    // (sort, uniq, awk, a second grep) is a two-tool composition: silent.
    for (std::size_t i = 1; i < tops.size(); ++i) {
        auto b = as_bound(*tops[i]);
        if (!b) { d.needs_shell = true; return d; }
        d.bound = *b;
    }

    const auto p = base_name(head.program());
    const bool grep_like = p == "grep" || p == "rg" || p == "egrep" || p == "fgrep";

    // Counts are their own intent: a different parameter, not a read.
    if (p == "wc" || (grep_like && has_flag(head, "c", "--count"))) {
        d.intent = Intent::CountOnly;
        d.tool   = "grep";
        d.reason = "`grep` with output:\"count\" returns per-file match counts "
                   "directly \u2014 no shell-out, and it skips generated trees.";
        return d;
    }
    if (p == "cat" || p == "head" || p == "tail") {
        if (head.argv.size() < 2) { d.needs_shell = true; return d; }   // reads stdin
        if (p == "tail" && has_flag(head, "f", "--follow")) { d.needs_shell = true; return d; }
        d.intent = Intent::ReadFile;
        d.tool   = "read";
        // Name the PARAMETER: a tip that names the tool gets ignored.
        if (p == "tail")
            d.reason = "`read` with offset:-N returns the LAST N lines (like "
                       "`tail -n N`) \u2014 ideal for logs, and no need to know "
                       "the file length.";
        else
            d.reason = "`read` takes offset/limit (and start_line/end_line) "
                       "for a line window, reports how many lines remain, and "
                       "caches re-reads \u2014 so you don't re-shell for the next "
                       "chunk.";
        return d;
    }
    if (p == "sed") {
        // Only `sed -n …p` prints; anything else is a transform.
        if (!has_flag(head, "n", "--quiet")) { d.needs_shell = true; return d; }
        d.intent = Intent::ReadFile;
        d.tool   = "read";
        d.reason = "to read a line range use `read` with start_line/end_line; to "
                   "read one function/type's body use `read` with symbol=\"name\" "
                   "\u2014 no line arithmetic.";
        return d;
    }
    if (grep_like) {
        if (head.argv.size() < 3 && p != "rg") { d.needs_shell = true; return d; }   // grep on stdin
        d.intent = Intent::Search;
        d.tool   = "grep";
        d.reason = "the `grep` tool is ripgrep-backed, skips build/vendor "
                   "trees, groups hits by enclosing symbol, and supports "
                   "word=true / context:\"block\".";
        return d;
    }
    if (p == "find") {
        // find with an action runs things; only a pure name search is a glob.
        for (std::size_t i = 1; i < head.argv.size(); ++i) {
            const auto a = sx::spelling(head.argv[i]);
            if (a == "-exec" || a == "-execdir" || a == "-delete" || a == "-ok"
                || a == "-okdir" || a == "-fprint" || a == "-fprintf" || a == "-fls") {
                d.needs_shell = true;
                return d;
            }
        }
        d.intent = Intent::FindFiles;
        d.tool   = "glob";
        d.reason = "the `glob` tool finds files by pattern (e.g. '**/*.ts') "
                   "without crawling generated trees.";
        return d;
    }
    if (p == "ls") {
        d.intent = Intent::ListDir;
        d.tool   = "list_dir";
        d.reason = "the `list_dir` tool gives a structured listing (type, "
                   "size), and `glob` matches names without crawling "
                   "generated trees.";
        return d;
    }
    d.needs_shell = true;
    return d;
}

std::string bash_tool_suggestion(std::string_view cmd) {
    const auto d = analyze_detour(cmd);
    // A bounded pipe on a command that genuinely needs the shell still has a
    // native answer — just not a different TOOL. `make 2>&1 | tail -20`
    // should stay in bash, but the pipe filters at the wrong layer: it
    // discards the rest before the terminal card sees it, so the user loses
    // output they were watching in order to save the model's context.
    // head_lines/tail_lines bound only what reaches the model.
    if (!d.substitutable()) {
        if (d.bound && d.intent != Intent::Write) {
            std::string tip = "tip: `";
            tip += d.bound->from_tail ? "tail_lines" : "head_lines";
            tip += ": " + std::to_string(d.bound->limit);
            tip += "` bounds what comes back to you without hiding the rest "
                   "from the user \u2014 a `| ";
            tip += d.bound->from_tail ? "tail" : "head";
            tip += "` drops those lines before the terminal card ever "
                   "shows them.";
            return tip;
        }
        return {};
    }
    std::string tip = "tip: " + d.reason;
    // When the model bounded the output with `| head -N`, name the native
    // parameter that replaces the pipe — otherwise it keeps reaching for it
    // because it doesn't know the tool can bound itself.
    if (d.bound) {
        tip += " (`";
        tip += d.bound->from_tail ? "offset:-" : "limit:";
        tip += std::to_string(d.bound->limit);
        tip += d.bound->from_tail
            ? "` tails the last N lines directly.)"
            : "` bounds the output \u2014 no `| head` needed.)";
    }
    return tip;
}

} // namespace mcp::tools::util
