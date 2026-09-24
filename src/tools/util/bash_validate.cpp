// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/bash_validate.hpp>
#include <mcp/tools/util/shellx.hpp>

#include <filesystem>
#include <initializer_list>
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


// ── Verdict ─────────────────────────────────────────────────────────────
//
// How the verdict is reached (measured on 11.5k real shell calls):
//
//   * The script is split into top-level PIPELINES. `cd X`, `echo "=== x
//     ==="` separators and `true` are scaffolding and carry no intent; the
//     model adds them around real work. 19% of calls are `cd X && inspect`
//     and 28% carry echo separators, so treating either as "shell work"
//     silenced the tip on most of the detours that matter.
//   * Each remaining pipeline is judged on its own. The whole call is a
//     detour only when EVERY one of them is inspection a native tool
//     answers; one build, one loop, one `sort | uniq` and it is shell work.
//     This is the Codex rule (parse_command: all parts must summarize, or
//     the call is Unknown), which errs toward silence.
//   * Writes are found first over EVERY command, nested or not. A write
//     anywhere means no advice at all, ever.
//   * Formatting tails (`| head -N`, `| tail -N`, `| wc -l`) fold into the
//     verdict as a bound or a count. Any other stage (sort, awk, a second
//     grep) is a composition the native tools can't express: silent.

namespace {

struct PipeVerdict {
    Intent intent = Intent::Other;
    std::string_view tool;
    std::string reason;
    std::string param;
    std::optional<Bound> bound;
    bool shell = false;       // needs the shell (not inspection)
};

bool is_int(std::string_view v) {
    return !v.empty() && v.find_first_not_of("0123456789") == std::string_view::npos;
}
int to_int(std::string_view v) {
    int n = 0;
    for (char ch : v) { n = n * 10 + (ch - '0'); if (n > 1'000'000) return 1'000'000; }
    return n;
}

// Non-flag operands of an argv, skipping the values of flags that take one.
// `--` ends flags. Glob/brace words are fine (they are patterns the native
// tools take too); any other expansion ($X, $(…), ~) → nullopt.
std::optional<std::vector<std::string_view>>
operands(const sx::Command& c, std::initializer_list<std::string_view> takes_value) {
    std::vector<std::string_view> out;
    bool flags = true;
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        if (const auto* dy = std::get_if<sx::Dyn>(&c.argv[i]))
            if (dy->why != sx::Dyn::Why::Glob && dy->why != sx::Dyn::Why::Brace)
                return std::nullopt;
        std::string_view v = sx::spelling(c.argv[i]);
        if (flags && v == "--") { flags = false; continue; }
        if (flags && v.size() > 1 && v[0] == '-') {
            for (auto t : takes_value)
                if (v == t) { ++i; break; }
            continue;
        }
        out.push_back(v);
    }
    return out;
}

// `head -N f`, `head -n N f`, `head -nN f`, `tail -N f`, … → N, or 0.
int count_flag(const sx::Command& c) {
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* a = sx::lit(c.argv[i]);
        if (!a) return 0;
        std::string_view v = *a;
        if ((v == "-n" || v == "--lines") && i + 1 < c.argv.size()) {
            const std::string* nx = sx::lit(c.argv[i + 1]);
            return nx && is_int(*nx) ? to_int(*nx) : 0;
        }
        if (v.starts_with("--lines=")) v.remove_prefix(8);
        else if (v.starts_with("-n")) v.remove_prefix(2);
        else if (v.starts_with("-")) v.remove_prefix(1);
        else continue;
        if (is_int(v)) return to_int(v);
    }
    return 0;
}

// `sed -n '10,40p' f` / `-n 10p f` / `-n -e '10,40p' f` → {10,40}. Only a
// single address range with the `p` command; anything else is a real sed
// program. The script is the `-e` value, or else the first operand.
std::optional<std::pair<int, int>> sed_range(const sx::Command& c) {
    std::optional<std::string_view> script;
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* a = sx::lit(c.argv[i]);
        if (!a) return std::nullopt;
        std::string_view v = *a;
        if (v == "-n" || v == "--quiet" || v == "--silent") continue;
        if (v == "-e" || v == "--expression") {
            if (script || i + 1 >= c.argv.size()) return std::nullopt;
            const std::string* nx = sx::lit(c.argv[++i]);
            if (!nx) return std::nullopt;
            script = *nx;
            continue;
        }
        if (v.starts_with("-")) return std::nullopt;   // -E, -z, -s …: a program
        if (!script) script = v;                       // first operand = script
    }
    if (!script) return std::nullopt;
    std::string_view v = *script;
    if (!v.ends_with("p")) return std::nullopt;
    v.remove_suffix(1);
    const auto comma = v.find(',');
    const auto lo = v.substr(0, comma);
    const auto hi = comma == std::string_view::npos ? lo : v.substr(comma + 1);
    if (!is_int(lo) || !(is_int(hi) || hi == "$")) return std::nullopt;
    return std::pair{to_int(lo), hi == "$" ? 0 : to_int(hi)};
}

// Judge one top-level pipeline. `stages` are its commands in order.
PipeVerdict judge(const std::vector<const sx::Command*>& stages) {
    PipeVerdict v;
    const sx::Command& head = *stages.front();

    // Formatting tails. `| head -N` / `| tail -N` bound; `| wc -l` counts.
    bool counted = false;
    for (std::size_t i = 1; i < stages.size(); ++i) {
        const sx::Command& st = *stages[i];
        if (auto b = as_bound(st)) { v.bound = *b; continue; }
        const auto sp = base_name(st.program());
        if (sp == "wc" && st.argv.size() == 2 && sx::spelling(st.argv[1]) == "-l"
            && i + 1 == stages.size()) { counted = true; continue; }
        v.shell = true;          // sort, uniq, awk, grep -v …: a composition
        return v;
    }

    // Values only the shell knows. A quoted `$` is a Lit, so `grep '$x' f`
    // passes; globs in arguments are patterns the native tools also take.
    for (const auto* c : stages) {
        if (!c->env.empty()) { v.shell = true; return v; }
        for (std::size_t i = 0; i < c->argv.size(); ++i)
            if (const auto* dy = std::get_if<sx::Dyn>(&c->argv[i])) {
                const bool glob = dy->why == sx::Dyn::Why::Glob || dy->why == sx::Dyn::Why::Brace;
                if (!(glob && i > 0 && c == &head)) { v.shell = true; return v; }
            }
        for (const auto& r : c->redirects)
            if (!noise_redirect(r)) { v.shell = true; return v; }
    }

    const auto p = base_name(head.program());
    const bool grep_like = p == "grep" || p == "rg" || p == "egrep" || p == "fgrep";

    if (grep_like) {
        // grep with no path reads stdin; rg with none searches cwd.
        auto ops = operands(head, {"-e", "-f", "-A", "-B", "-C", "-m", "--include",
                                   "--exclude", "--exclude-dir", "-g", "--glob", "-t",
                                   "--type", "--max-count", "--context"});
        if (!ops) { v.shell = true; return v; }
        const bool has_e = has_flag(head, "e", "--regexp");
        const std::size_t need = has_e ? 1 : 2;
        if (p != "rg" && ops->size() < need) { v.shell = true; return v; }
        if (has_flag(head, "f", "--file")) { v.shell = true; return v; }
        v.tool = "grep";
        if (counted || has_flag(head, "c", "--count")) {
            v.intent = Intent::CountOnly;
            v.reason = "`grep` with output:\"count\" returns per-file match counts "
                       "directly \u2014 no shell-out, and it skips generated trees.";
            v.param = "output: \"count\"";
            return v;
        }
        v.intent = Intent::Search;
        v.reason = "the `grep` tool is ripgrep-backed, skips build/vendor "
                   "trees, groups hits by enclosing symbol, and supports "
                   "word=true / context:\"block\".";
        // Map the flags the model actually typed to the native params.
        std::vector<std::string> ps;
        if (has_flag(head, "l", "--files-with-matches")) ps.push_back("output: \"files\"");
        if (has_flag(head, "w", "--word-regexp")) ps.push_back("word: true");
        if (p != "rg" && !has_flag(head, "i", "--ignore-case")) ps.push_back("case_sensitive: true");
        int ctx = -1;
        std::string glob;
        for (std::size_t i = 1; i < head.argv.size(); ++i) {
            std::string_view a = sx::spelling(head.argv[i]);
            if (a == "--") break;
            std::string_view val;
            if ((a == "-A" || a == "-B" || a == "-C" || a == "--context")
                && i + 1 < head.argv.size())
                val = sx::spelling(head.argv[i + 1]);
            else if (a.size() > 2 && (a.starts_with("-A") || a.starts_with("-B") || a.starts_with("-C")))
                val = a.substr(2);
            if (is_int(val)) ctx = std::max(ctx, to_int(val));
            if (a.starts_with("--include=")) glob = std::string{a.substr(10)};
            else if ((a == "--include" || a == "-g" || a == "--glob") && i + 1 < head.argv.size())
                glob = std::string{sx::spelling(head.argv[i + 1])};
            else if (a.starts_with("--glob=")) glob = std::string{a.substr(7)};
        }
        if (ctx >= 0) ps.push_back("context: \"" + std::to_string(std::min(ctx, 60)) + "\"");
        if (!glob.empty() && glob.find('"') == std::string::npos) ps.push_back("glob: \"" + glob + "\"");
        for (auto& s : ps) v.param += (v.param.empty() ? "" : ", ") + s;
        // GNU grep's BRE `\|` is alternation; ripgrep (the native tool)
        // reads it as a literal `|` and silently finds nothing. 24% of real
        // grep calls use it, so a model copying its pattern across would
        // get a wrong "no matches". Say so.
        if (p == "grep" && !has_flag(head, "E", "--extended-regexp")
            && !has_flag(head, "F", "--fixed-strings") && !has_flag(head, "P", "--perl-regexp"))
            for (std::size_t i = 1; i < head.argv.size(); ++i)
                if (sx::spelling(head.argv[i]).find("\\|") != std::string_view::npos) {
                    v.reason += " Its pattern is ripgrep syntax: write `a|b`, not `a\\|b`.";
                    break;
                }
        return v;
    }
    if (counted) { v.shell = true; return v; }   // `ls | wc -l` etc.

    if (p == "wc") {
        v.intent = Intent::CountOnly;
        v.tool   = "grep";
        v.reason = "`grep` with output:\"count\" returns per-file match counts "
                   "directly \u2014 no shell-out, and it skips generated trees.";
        v.param  = "output: \"count\"";
        auto ops = operands(head, {});
        if (!ops || ops->empty()) { v.shell = true; }   // wc on stdin
        return v;
    }
    if (p == "cat" || p == "head" || p == "tail" || p == "nl") {
        auto ops = operands(head, {"-n", "-c", "--lines", "--bytes"});
        if (!ops || ops->empty()) { v.shell = true; return v; }   // stdin
        if (ops->size() > 1 && p != "cat") { v.shell = true; return v; }
        if (has_flag(head, "f", "--follow") || has_flag(head, "F", "")) { v.shell = true; return v; }
        if (has_flag(head, "c", "--bytes")) { v.shell = true; return v; }
        v.intent = Intent::ReadFile;
        v.tool   = "read";
        const int n = count_flag(head);
        if (p == "tail") {
            v.reason = "`read` with offset:-N returns the LAST N lines (like "
                       "`tail -n N`) \u2014 ideal for logs, and no need to know "
                       "the file length.";
            if (n > 0) v.param = "offset: -" + std::to_string(n);
        } else {
            v.reason = "`read` takes offset/limit (and start_line/end_line) "
                       "for a line window, reports how many lines remain, and "
                       "caches re-reads \u2014 so you don't re-shell for the next "
                       "chunk.";
            if (p == "head" && n > 0) v.param = "limit: " + std::to_string(n);
        }
        // `cat f | head -20` / `cat f | tail -20`: the bound IS the window.
        if (v.param.empty() && v.bound) {
            v.param = v.bound->from_tail ? "offset: -" + std::to_string(v.bound->limit)
                                         : "limit: " + std::to_string(v.bound->limit);
            v.bound.reset();
        }
        return v;
    }
    if (p == "sed") {
        if (!has_flag(head, "n", "--quiet") && !has_flag(head, "n", "--silent")) {
            v.shell = true;
            return v;
        }
        auto r = sed_range(head);
        auto ops = operands(head, {"-e", "--expression"});
        const bool has_e = has_flag(head, "e", "--expression");
        // exactly one file: `sed -n 5p a b` concatenates, read can't.
        if (!r || !ops || ops->size() != (has_e ? 1u : 2u)) { v.shell = true; return v; }
        v.intent = Intent::ReadFile;
        v.tool   = "read";
        v.reason = "to read one function/type's body use `read` with "
                   "symbol=\"name\" \u2014 no line arithmetic.";
        v.param  = r->second > 0
            ? "start_line: " + std::to_string(r->first) + ", end_line: " + std::to_string(r->second)
            : "start_line: " + std::to_string(r->first);
        return v;
    }
    if (p == "find") {
        for (std::size_t i = 1; i < head.argv.size(); ++i) {
            const auto a = sx::spelling(head.argv[i]);
            if (a == "-exec" || a == "-execdir" || a == "-delete" || a == "-ok"
                || a == "-okdir" || a == "-fprint" || a == "-fprintf" || a == "-fls"
                || a == "-fprint0" || a == "-printf" || a == "-newer" || a == "-mtime"
                || a == "-mmin" || a == "-size" || a == "-perm" || a == "-user") {
                v.shell = true;     // an action, or a predicate glob can't express
                return v;
            }
        }
        v.intent = Intent::FindFiles;
        v.tool   = "glob";
        v.reason = "the `glob` tool finds files by pattern (e.g. '**/*.ts') "
                   "without crawling generated trees.";
        return v;
    }
    if (p == "ls" || p == "tree") {
        if (has_flag(head, "t", "") || has_flag(head, "S", "") || has_flag(head, "r", "")) {
            v.shell = true;         // sorted by time/size: list_dir can't order
            return v;
        }
        v.intent = Intent::ListDir;
        v.tool   = "list_dir";
        v.reason = "the `list_dir` tool gives a structured listing (type, "
                   "size), and `glob` matches names without crawling "
                   "generated trees.";
        if (p == "tree" || has_flag(head, "R", "--recursive")) v.param = "recursive: true";
        return v;
    }
    if (p == "git") {
        // Only read-only subcommands with a native twin, and only flags that
        // twin can express. Anything else (a push, an add, --grep, -S,
        // --word-diff, pathspec magic) is silent: a wrong tip is worse than
        // none.
        std::vector<std::string_view> a;
        for (std::size_t i = 1; i < head.argv.size(); ++i) {
            const std::string* w = sx::lit(head.argv[i]);
            if (!w) { v.shell = true; return v; }
            a.push_back(*w);
        }
        std::string dir;
        if (a.size() >= 2 && a[0] == "-C") { dir = std::string{a[1]}; a.erase(a.begin(), a.begin() + 2); }
        if (a.empty() || a[0].starts_with("-")) { v.shell = true; return v; }
        const std::string_view sub = a[0];
        std::vector<std::string_view> flags, pos;
        bool after_dd = false;
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "--") { after_dd = true; continue; }
            (!after_dd && a[i].starts_with("-") ? flags : pos).push_back(a[i]);
        }
        auto only = [&](std::initializer_list<std::string_view> ok) {
            for (auto f : flags) {
                bool hit = false;
                for (auto o : ok)
                    if (f == o || (o.ends_with("=") && f.starts_with(o))) hit = true;
                if (!hit) return false;
            }
            return true;
        };
        auto has = [&](std::string_view f) {
            for (auto x : flags) if (x == f || x.starts_with(std::string{f} + "=")) return true;
            return false;
        };
        std::vector<std::string> ps;
        if (!dir.empty()) ps.push_back("path: \"" + dir + "\"");
        // A positional is a path after `--`, or when it has a `/` or a file
        // extension and no rev syntax. `HEAD~3..HEAD` and `v1.2` are refs.
        auto as_arg = [&](std::string_view x) {
            const bool rev = x.find("..") != std::string_view::npos || x.find('~') != std::string_view::npos
                          || x.find('^') != std::string_view::npos || x.find('@') != std::string_view::npos;
            const bool pathy = after_dd || x.find('/') != std::string_view::npos
                            || (x.find('.') != std::string_view::npos && !x.starts_with("v"));
            return (!rev && pathy ? "path: \"" : "ref: \"") + std::string{x} + "\"";
        };
        auto count_flag_ok = [](std::string_view f) { return f.size() > 1 && is_int(f.substr(1)); };
        v.intent = Intent::GitRead;
        if (sub == "status") {
            if (!only({"-s", "--short", "-sb", "-b", "--branch", "--porcelain"}) || !pos.empty()) {
                v.shell = true;
                return v;
            }
            v.tool = "git_status";
        } else if (sub == "log") {
            // Custom --format/--pretty/--stat/-p ask for a shape git_log
            // doesn't give; stay silent rather than promise it.
            for (auto f : flags)
                if (!(f == "--oneline" || f == "-n" || f.starts_with("--max-count=")
                      || f == "--no-merges" || f == "--first-parent" || f == "--no-decorate"
                      || count_flag_ok(f))) {
                    v.shell = true;
                    return v;
                }
            v.tool = "git_log";
            int n = 0;
            for (auto f : flags) {
                if (count_flag_ok(f)) n = to_int(f.substr(1));
                if (f.starts_with("--max-count=") && is_int(f.substr(12))) n = to_int(f.substr(12));
            }
            if (has("-n")) {                         // `-n 5`: value is the next word
                if (pos.empty() || !is_int(pos.front())) { v.shell = true; return v; }
                n = to_int(pos.front());
                pos.erase(pos.begin());
            }
            if (n > 0) ps.push_back("count: " + std::to_string(n));
            if (has("--oneline")) ps.push_back("oneline: true");
            if (pos.size() > 2) { v.shell = true; return v; }
            for (auto x : pos) ps.push_back(as_arg(x));
        } else if (sub == "diff") {
            for (auto f : flags)
                if (!(f == "--stat" || f == "--staged" || f == "--cached" || f == "--no-color"
                      || (f.starts_with("-U") && is_int(f.substr(2))))) {
                    v.shell = true;
                    return v;
                }
            if (pos.size() > 2) { v.shell = true; return v; }
            v.tool = "git_diff";
            if (has("--staged") || has("--cached")) ps.push_back("staged: true");
            if (has("--stat")) ps.push_back("stat_only: true");
            for (auto f : flags)
                if (f.starts_with("-U") && is_int(f.substr(2)))
                    ps.push_back("context: " + std::string{f.substr(2)});
            for (auto x : pos) ps.push_back(as_arg(x));
        } else if (sub == "show") {
            // git_show gives metadata + patch, or a file at a rev. --stat
            // and custom formats are shapes it doesn't have.
            if (!only({"--no-color"}) || pos.size() > 1) { v.shell = true; return v; }
            v.tool = "git_show";
            if (!pos.empty()) {
                const std::string_view x = pos.front();
                if (auto c = x.find(':'); c != std::string_view::npos && c + 1 < x.size()) {
                    ps.push_back("ref: \"" + std::string{x.substr(0, c)} + "\"");
                    ps.push_back("path: \"" + std::string{x.substr(c + 1)} + "\"");
                    ps.push_back("format: \"file\"");
                } else {
                    ps.push_back("ref: \"" + std::string{x} + "\"");
                }
            }
        } else if (sub == "blame") {
            if (pos.empty() || pos.size() > 2) { v.shell = true; return v; }
            int lo = 0, hi = 0;
            for (std::size_t i = 0; i < flags.size(); ++i) {
                std::string_view f = flags[i];
                if (f == "-L") { v.shell = true; return v; }   // value was split off; rare
                if (f.starts_with("-L")) {
                    f.remove_prefix(2);
                    const auto c = f.find(',');
                    if (c == std::string_view::npos || !is_int(f.substr(0, c)) || !is_int(f.substr(c + 1))) {
                        v.shell = true;
                        return v;
                    }
                    lo = to_int(f.substr(0, c));
                    hi = to_int(f.substr(c + 1));
                } else if (f != "--no-color") {
                    v.shell = true;
                    return v;
                }
            }
            v.tool = "git_blame";
            if (pos.size() == 2) ps.push_back("ref: \"" + std::string{pos[0]} + "\"");
            ps.push_back("path: \"" + std::string{pos.back()} + "\"");
            if (lo > 0) {
                ps.push_back("start_line: " + std::to_string(lo));
                ps.push_back("end_line: " + std::to_string(hi));
            }
        } else {
            v.shell = true;        // add, push, commit, checkout, stash …
            return v;
        }
        v.reason = "it returns structured output, keeps the pager out, and the "
                   "user gets a proper card.";
        for (auto& s : ps) v.param += (v.param.empty() ? "" : ", ") + s;
        return v;
    }
    v.shell = true;
    return v;
}

// Scaffolding the model wraps around real work: it has no intent of its own.
bool scaffolding(const sx::Command& c) {
    const auto p = base_name(c.program());
    if (p == "cd" || p == "pwd" || p == "true" || p == ":") return true;
    if (p == "echo" || p == "printf") {
        for (std::size_t i = 1; i < c.argv.size(); ++i)
            if (!sx::lit(c.argv[i])) return false;      // echo $(…) / $X does work
        return c.redirects.empty();
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

    // Anything nested ($(…), loops, if, subshells, functions, `&`) is shell
    // work, but a bound on its last top-level stage still has an answer.
    const auto tops = s.top_level();
    auto shell_work = [&]() -> Detour& {
        d.needs_shell = true;
        d.intent = Intent::Other;
        d.tool = {};
        d.reason.clear();
        d.param.clear();
        d.steps.clear();
        d.bound.reset();
        if (!tops.empty() && tops.back()->stage > 0)
            if (auto b = as_bound(*tops.back())) d.bound = *b;
        return d;
    };
    if (tops.size() != s.commands.size()) return shell_work();

    // Group into pipelines; `|&` (stderr into the pipe) is shell plumbing.
    std::vector<std::vector<const sx::Command*>> pipes;
    for (const auto* c : tops) {
        if (c->join == sx::Join::PipeErr) return shell_work();
        if (pipes.empty() || c->stage == 0) pipes.emplace_back();
        pipes.back().push_back(c);
    }

    // Judge each non-scaffolding pipeline. All must be inspection, and they
    // must agree on the tool, or the tip would be naming half a call.
    bool any = false;
    std::string cwd;           // literal `cd` target so far, for the steps list
    for (const auto& pl : pipes) {
        if (pl.size() == 1 && base_name(pl.front()->program()) == "cd") {
            const sx::Command& c = *pl.front();
            const std::string* t = c.argv.size() == 2 ? sx::lit(c.argv[1]) : nullptr;
            if (!t || t->empty() || *t == "-") cwd = "?";          // unknown dir
            else if (cwd == "?" ) { if (t->front() == '/') cwd = *t; }
            else if (t->front() == '/') cwd = *t;
            else cwd = cwd.empty() ? *t : cwd + "/" + *t;
            if (cwd != "?") {
                cwd = std::filesystem::path{cwd}.lexically_normal().generic_string();
                if (cwd == "." || cwd == "./") cwd.clear();
                while (cwd.size() > 1 && cwd.back() == '/') cwd.pop_back();
            }
            continue;
        }
        if (pl.size() == 1 && scaffolding(*pl.front())) continue;
        PipeVerdict v = judge(pl);
        if (v.shell) return shell_work();
        {
            std::string st{v.tool};
            if (!v.param.empty()) st += " " + v.param;
            if (!cwd.empty() && cwd != "?") st += " (in " + cwd + ")";
            d.steps.push_back(std::move(st));
        }
        if (!any) {
            d.intent = v.intent;
            d.tool   = v.tool;
            d.reason = std::move(v.reason);
            d.param  = std::move(v.param);
            d.bound  = v.bound;
            any = true;
        } else {
            if (v.tool != d.tool) {
                // Mixed native tools (a read and a grep): still a detour.
                // The steps list names each call.
                d.param.clear();
                d.bound.reset();
                d.reason = "each step is its own native call; make them in one "
                           "turn and they run in parallel, each with its own card.";
                d.tool = "read";
                d.intent = Intent::ReadFile;
            } else if (v.param != d.param) {
                d.param.clear();
            }
            if (!v.bound || !d.bound || v.bound->limit != d.bound->limit) d.bound.reset();
        }
    }
    if (!any) return shell_work();       // only cd/echo: nothing to say
    return d;
}

std::string bash_tool_suggestion(const Detour& d) {
    // Shell work that ends in `| head -N` / `| tail -N`: the pipe filters at
    // the wrong layer. It discards the rest before the terminal card sees it,
    // so the user loses output they were watching to save the model's
    // context. head_lines/tail_lines bound only what reaches the model.
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
    // Lead with the PARAMETER, read off the model's own command. Naming the
    // tool is the advice that was already being ignored; naming the exact
    // value it just typed is not.
    std::string tip = "tip: ";
    if (d.steps.size() > 1) {
        // Several native steps: list each call, capped so a long chain
        // doesn't turn the tip into a wall.
        tip += "these are " + std::to_string(d.steps.size()) + " native calls: ";
        constexpr std::size_t kShow = 4;
        for (std::size_t i = 0; i < d.steps.size() && i < kShow; ++i)
            tip += (i ? "; " : "") + ("`" + d.steps[i] + "`");
        if (d.steps.size() > kShow) tip += "; \u2026";
        tip += " \u2014 make them in one turn and they run in parallel, each "
               "with its own card.";
        return tip;
    }
    if (!d.param.empty()) {
        tip += "`" + std::string{d.tool} + "` with `" + d.param + "` does this directly \u2014 ";
        tip += d.reason;
    } else if (d.intent == Intent::GitRead) {
        tip += "`" + std::string{d.tool} + "` does this directly \u2014 " + d.reason;
    } else {
        tip += d.reason;
    }
    // A `| head -N` on a search/list: name the native tool's own bound. Only
    // name parameters the tool really has: read and grep take limit (grep
    // pages at 20 by default); list_dir/glob are already bounded. Naming a
    // parameter the tool doesn't have is worse than no tip.
    if (d.bound) {
        if (d.tool == "read")
            tip += d.bound->from_tail
                ? " (`offset:-" + std::to_string(d.bound->limit) + "` tails it directly.)"
                : " (`limit:" + std::to_string(d.bound->limit) + "` bounds it directly.)";
        else if (d.tool == "grep" && !d.bound->from_tail)
            tip += " (`limit:" + std::to_string(d.bound->limit)
                 + "` bounds the output \u2014 no `| head` needed.)";
        else if (d.tool == "grep")
            tip += " (it pages 20 hits at a time; `offset:` pages on.)";
        else
            tip += " (its output is already bounded \u2014 no `| head` needed.)";
    }
    return tip;
}

std::string bash_tool_suggestion(std::string_view cmd) {
    return bash_tool_suggestion(analyze_detour(cmd));
}

} // namespace mcp::tools::util
