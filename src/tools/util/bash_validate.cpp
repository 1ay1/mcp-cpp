// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/bash_validate.hpp>

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

} // namespace

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
    // Root-wipe patterns need the `/` to be a WHOLE argument, not a
    // path prefix — otherwise `rm -rf /home/x/build` (a legitimate
    // absolute-path delete) is falsely refused because it contains the
    // substring "rm -rf /". Match only when the slash is the target:
    // end-of-command, followed by whitespace, or the `/*` glob.
    {
        static constexpr std::string_view kRmPrefixes[] = {
            "rm -rf ", "rm -fr ", "rm -r -f ", "rm -f -r ",
        };
        for (auto pre : kRmPrefixes) {
            size_t p = 0;
            while ((p = cmd.find(pre, p)) != std::string::npos) {
                size_t a = p + pre.size();
                while (a < cmd.size() && (cmd[a] == ' ' || cmd[a] == '\t')) a++;
                if (a < cmd.size() && cmd[a] == '/') {
                    size_t after = a + 1;
                    bool root_target =
                        after == cmd.size()
                        || cmd[after] == ' ' || cmd[after] == '\t'
                        || cmd[after] == ';' || cmd[after] == '&'
                        || cmd[after] == '|' || cmd[after] == '\n'
                        || (cmd[after] == '*');
                    if (root_target)
                        return "refusing wide rm that could wipe the filesystem root";
                }
                if (a < cmd.size() && cmd[a] == '~'
                    && (a + 1 == cmd.size() || cmd[a + 1] == ' '
                        || cmd[a + 1] == '\t' || cmd[a + 1] == '/'))
                    return "refusing to recursively delete the home directory";
                p += pre.size();
            }
        }
    }
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
// The rewrite of a heuristic that a probe caught missing 6 of 10 real
// shell-outs. See the header for the measurement; the two structural fixes
// are (1) parse the pipeline instead of bailing on the first `|`, and
// (2) decide READ vs WRITE before saying anything.

namespace {

// Strip one layer of surrounding quotes, and tell whether a byte at index i
// sits inside quotes. Needed because `echo "grep foo"` and a path like
// "my grep dir/f" must not read as commands.
bool in_quotes(std::string_view s, size_t idx) noexcept {
    bool sq = false, dq = false;
    for (size_t i = 0; i < idx && i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '\'' && !dq) sq = !sq;
        else if (s[i] == '"' && !sq) dq = !dq;
    }
    return sq || dq;
}

// Split a pipeline on unquoted `|`. The whole point of the rewrite: the old
// code treated any `|` as "bash is doing real work" and went silent, which
// is where most of its misses came from.
std::vector<std::string_view> pipeline_stages(std::string_view cmd) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] == '\\') { ++i; continue; }
        if (cmd[i] != '|' || in_quotes(cmd, i)) continue;
        if (i + 1 < cmd.size() && cmd[i + 1] == '|') { ++i; continue; } // `||`
        out.push_back(cmd.substr(start, i - start));
        start = i + 1;
    }
    out.push_back(cmd.substr(start));
    return out;
}

// Trim ASCII whitespace.
std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// `2>/dev/null` (and `2>&1`) is NOISE SUPPRESSION, not a redirect to a file:
// the model adds it because a missing path makes the shell shout. A native
// tool simply reports "no matches" instead, so this must not be read as
// "the shell is doing real work" — it was one of the two dominant misses.
bool only_discards_stderr(std::string_view s) noexcept {
    return s == "2>/dev/null" || s == "2>&1" || s == "2>nul"
        || s == "2>/dev/null 2>&1";
}

// Remove every trailing stderr-discard from ONE stage. Per-stage rather than
// once over the whole command, because the model writes it mid-pipeline too
// (`ls a b 2>/dev/null | head -20`), and a `>` left anywhere reads as a
// write and silences the verdict.
std::string_view strip_stderr_discard(std::string_view s) noexcept {
    for (;;) {
        s = trim(s);
        size_t r = s.rfind("2>");
        if (r == std::string_view::npos) return s;
        if (!only_discards_stderr(trim(s.substr(r)))) return s;
        s = s.substr(0, r);
    }
}

// A bounded tail stage is a RESULT LIMIT, which every native tool takes as a
// parameter — not shell work. Returns the N from `head -N` / `tail -N`.
std::optional<Bound> as_bound(std::string_view stage) {
    stage = trim(stage);
    auto tok = first_token(stage);
    if (tok != "head" && tok != "tail") return std::nullopt;
    const bool tail = tok == "tail";
    auto rest = trim(stage.substr(stage.find(tok) + tok.size()));
    // Accept `-N`, `-n N`, `-n5`; anything else (a file argument, `-f`,
    // `-c`) means this is not a plain limit and the shell is doing more.
    int n = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '-') {
            size_t j = i + 1;
            if (j < rest.size() && rest[j] == 'n') ++j;
            while (j < rest.size() && rest[j] == ' ') ++j;
            size_t d = j;
            while (d < rest.size() && rest[d] >= '0' && rest[d] <= '9') {
                n = n * 10 + (rest[d] - '0');
                ++d;
            }
            if (d == j) return std::nullopt;   // a flag, not a count
            i = d - 1;
            continue;
        }
        if (rest[i] == ' ' || rest[i] == '\t') continue;
        return std::nullopt;   // a file argument — reading a FILE, not limiting
    }
    if (n <= 0) return std::nullopt;
    return Bound{n, tail};
}

// Does this stage WRITE? The safety gate: `sed -i`, `cat > f`, a heredoc and
// `tee` all used to match a READ suggestion, which is the one mistake that
// cannot be recovered from if a caller ever acts on the verdict.
bool writes(std::string_view stage) noexcept {
    for (size_t i = 0; i < stage.size(); ++i) {
        if (stage[i] == '\\') { ++i; continue; }
        if (in_quotes(stage, i)) continue;
        // `>` or `>>` to anything (but `2>/dev/null` is handled by the caller)
        if (stage[i] == '>') return true;
    }
    auto tok = first_token(stage);
    if (tok == "tee" || tok == "truncate" || tok == "dd") return true;
    // `sed -i` / `sed --in-place` edits the file in place.
    if (tok == "sed" || tok == "perl" || tok == "awk") {
        if (stage.find(" -i") != std::string_view::npos
            || stage.find("--in-place") != std::string_view::npos)
            return true;
    }
    return false;
}

}  // namespace

Detour analyze_detour(std::string_view cmd) {
    Detour d;

    // `2>/dev/null` is NOISE SUPPRESSION, not a redirect to a file — the
    // model appends it because a missing path makes the shell shout, and a
    // native tool just reports "no matches". Stripped per stage BEFORE the
    // redirect scan below, because otherwise its `>` reads as a write and
    // silences the whole verdict. This was one of the two dominant misses.
    //
    // Split first, strip each stage, then rejoin the analysis — the discard
    // can sit on any stage, not just the last.
    std::vector<std::string_view> stages;
    for (auto s : pipeline_stages(cmd))
        stages.push_back(strip_stderr_discard(s));

    // Command substitution and chaining genuinely need the shell.
    for (auto stage : stages) {
        for (size_t i = 0; i < stage.size(); ++i) {
            if (stage[i] == '\\') { ++i; continue; }
            if (in_quotes(stage, i)) continue;
            if (stage[i] == '`' || stage[i] == ';' || stage[i] == '\n'
                || stage[i] == '&') { d.needs_shell = true; break; }
            if (stage[i] == '$' && i + 1 < stage.size() && stage[i + 1] == '(') {
                d.needs_shell = true; break;
            }
            if (stage[i] == '<') { d.needs_shell = true; break; } // heredoc
        }
        if (d.needs_shell) break;
    }

    // A WRITE anywhere disqualifies the whole command from any read
    // substitution — and is reported as Write so a caller can tell the
    // difference between "no better tool" and "actively dangerous to swap".
    for (auto s : stages) {
        if (writes(s)) {
            d.intent = Intent::Write;
            d.needs_shell = true;
            return d;
        }
    }
    if (d.needs_shell) return d;

    // Fold trailing stages that are merely LIMITS into a bound. What remains
    // must be a single real command.
    std::string_view head_stage = trim(stages.front());
    for (size_t i = 1; i < stages.size(); ++i) {
        auto s = trim(stages[i]);
        if (auto b = as_bound(s)) { d.bound = *b; continue; }
        d.needs_shell = true;   // a genuine transform: sort, uniq, xargs, awk…
    }
    if (d.needs_shell) return d;

    const auto tok = first_token(head_stage);

    // `wc -l` and `grep -c` are COUNTS, not reads — a different parameter,
    // so they get their own intent rather than a read suggestion.
    const bool counts =
        tok == "wc"
        || ((tok == "grep" || tok == "rg")
            && (head_stage.find(" -c") != std::string_view::npos
                || head_stage.find("--count") != std::string_view::npos));

    if (counts) {
        d.intent = Intent::CountOnly;
        d.tool   = "grep";
        d.reason = "`grep` with output:\"count\" returns per-file match counts "
                   "directly \u2014 no shell-out, and it skips generated trees.";
        return d;
    }
    if (tok == "cat" || tok == "head" || tok == "tail") {
        d.intent = Intent::ReadFile;
        d.tool   = "read";
        // `tail -N file` has an exact native counterpart the model rarely
        // knows about, so name it rather than describing offset/limit in
        // the abstract: a tip that names the PARAMETER gets used, a tip
        // that names the tool gets ignored.
        if (tok == "tail")
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
    if (tok == "sed") {
        // Only `sed -n Np` style printing is a read; everything else is a
        // transform the shell should keep.
        if (head_stage.find("-n") == std::string_view::npos) return d;
        d.intent = Intent::ReadFile;
        d.tool   = "read";
        d.reason = "to read a line range use `read` with offset+limit; to read "
                   "one function/type's body use `read` with symbol=\"name\" "
                   "\u2014 no line arithmetic.";
        return d;
    }
    if (tok == "grep" || tok == "rg") {
        d.intent = Intent::Search;
        d.tool   = "grep";
        d.reason = "the `grep` tool is ripgrep-backed, skips build/vendor "
                   "trees, groups hits by enclosing symbol, and supports "
                   "word=true / context:\"block\".";
        return d;
    }
    if (tok == "find") {
        d.intent = Intent::FindFiles;
        d.tool   = "glob";
        d.reason = "the `glob` tool finds files by pattern (e.g. '**/*.ts') "
                   "without crawling generated trees.";
        return d;
    }
    if (tok == "ls") {
        d.intent = Intent::ListDir;
        d.tool   = "list_dir";
        d.reason = "the `list_dir` tool gives a structured listing (type, "
                   "size), and `glob` matches names without crawling "
                   "generated trees.";
        return d;
    }
    return d;
}

std::string bash_tool_suggestion(std::string_view cmd) {
    const auto d = analyze_detour(cmd);
    if (!d.substitutable()) return {};
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
