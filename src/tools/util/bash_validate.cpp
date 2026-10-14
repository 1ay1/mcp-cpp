// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/bash_validate.hpp>

#include <algorithm>
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

} // namespace mcp::tools::util
