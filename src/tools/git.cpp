// SPDX-License-Identifier: Apache-2.0
//
// git.cpp — register_git_tools: git_status / git_diff / git_log / git_commit.
// Faithful port of agentty's src/tool/tools/git.cpp. These four subtools
// drive agentty's diff-review / changes-strip UI and stay load-bearing.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/subprocess.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

ToolError classify_git_failure(const util::SubprocessResult& r,
                               std::string_view op) {
    if (!r.started)
        return ToolError::spawn(std::string{op} + ": " + r.start_error
            + " (is `git` installed and on PATH?)");

    std::string_view out = r.output;
    auto contains = [&](std::string_view needle) {
        return out.find(needle) != std::string_view::npos;
    };

    if (contains("not a git repository"))
        return ToolError::not_found(std::string{op}
            + " failed: not inside a git repository. Run `git init` first, "
              "or invoke from a directory under an existing repo.");

    if (contains("Please tell me who you are") || contains("empty ident"))
        return ToolError::subprocess(std::string{op}
            + " failed: git identity not configured. Run "
              "`git config user.email \"you@example.com\"` and "
              "`git config user.name \"Your Name\"` (drop `--global` to "
              "scope to this repo only).");

    if (contains("unknown revision") || contains("bad revision"))
        return ToolError::not_found(std::string{op}
            + " failed: unknown revision/ref. " + std::string{out});

    if (contains(".git/index.lock"))
        return ToolError::subprocess(std::string{op}
            + " failed: another git process holds .git/index.lock. Wait for "
              "it to finish, or remove the stale lock if no git is running.");

    if (r.timed_out)
        return ToolError::subprocess(std::string{op}
            + " timed out. Output so far:\n" + r.output);

    return ToolError::subprocess(std::string{op}
        + " failed (exit " + std::to_string(r.exit_code) + "):\n"
        + r.output);
}

std::expected<std::string, ToolError>
run_git(const std::vector<std::string>& argv, std::string_view op,
        std::size_t max_bytes = 30'000) {
    auto r = util::run_argv_s(argv, max_bytes);
    if (!r.started || r.timed_out || r.exit_code != 0)
        return std::unexpected(classify_git_failure(r, op));
    std::string out = std::move(r.output);
    if (r.truncated) out += "\n[output truncated]";
    return out;
}

// Resolve the repository directory to run git in. Given a raw path (a
// directory OR a file — the workspace-checked string for the tool's
// `path`/first `files[]` entry), ask git for the enclosing worktree
// toplevel so every subcommand runs against the RIGHT repo regardless
// of the process cwd. Falls back to the checked path's own directory
// (or the workspace root when no path was given) so the classic
// "cwd isn't the repo" failure can't happen. The returned directory is
// always inside the workspace: `checked` already passed the containment
// gate, and rev-parse only ever walks UP to an ancestor that still
// contains it — we clamp the result back to the workspace root if git
// somehow reports a toplevel above it.
std::string resolve_git_dir(std::string_view checked) {
    namespace fs = std::filesystem;
    fs::path start = checked.empty()
        ? util::workspace_root()
        : fs::path{std::string{checked}};
    std::error_code ec;
    // If `start` is a file, its parent is the directory to probe.
    fs::path dir = start;
    if (!checked.empty() && !fs::is_directory(start, ec))
        dir = start.parent_path();
    if (dir.empty()) dir = util::workspace_root();

    auto r = util::run_argv_s(
        {"git", "-C", dir.string(), "rev-parse", "--show-toplevel"}, 4096);
    if (r.started && !r.timed_out && r.exit_code == 0) {
        std::string top = std::move(r.output);
        while (!top.empty() && (top.back() == '\n' || top.back() == '\r'))
            top.pop_back();
        if (!top.empty()) {
            // Clamp: the repo must contain the workspace-checked path, so
            // its toplevel is at or below the workspace root. If git
            // reports an ancestor ABOVE the workspace (nested-checkout
            // edge), prefer the workspace root — never escape the gate.
            const fs::path& ws = util::workspace_root();
            fs::path topc = fs::weakly_canonical(fs::path{top}, ec);
            fs::path wsc  = fs::weakly_canonical(ws, ec);
            auto within = [&](const fs::path& a, const fs::path& b) {
                auto ra = a.string(), rb = b.string();
                return ra.size() >= rb.size() && ra.compare(0, rb.size(), rb) == 0;
            };
            if (within(topc, wsc)) return top;
            return ws.string();
        }
    }
    // rev-parse failed (not a repo yet, or git missing) — fall back to the
    // directory itself; the subcommand's own error is clearer than ours.
    return dir.string();
}

// ── git_status ─────────────────────────────────────────────────────────

struct GitStatusArgs {
    std::string root;
    std::string display_description;
};

std::expected<GitStatusArgs, ToolError> parse_git_status_args(const json& j) {
    util::ArgReader ar(j);
    return GitStatusArgs{
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_status(const GitStatusArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "git_status");
    if (!wp) return std::unexpected(std::move(wp.error()));
    const std::string git_dir = resolve_git_dir(wp->string());
    // porcelain=v1 (the `git status -s` short format: `XY path`, one line per
    // change, plus a `## branch...upstream [ahead/behind]` header). Stable
    // and script-parseable like v2, but READABLE — v2's
    // `1 .M N... 100644 100644 100644 <sha> <sha> path` machine rows are
    // gibberish to a human and to the model. The tool card body shows this
    // verbatim, so it must be the form a person would want to read.
    auto out = run_git({"git", "-C", git_dir, "status",
                        "--porcelain=v1", "--branch"}, "git_status");
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    // With --branch the output is never empty (the `## ` line always prints),
    // so a clean tree is the lone branch line; spell that out.
    if (output.empty()) {
        output = "working tree clean";
    } else {
        // Clean iff there is exactly the one `## ` branch line (no change
        // rows). find('\n') past the first line tells us if more follow.
        const auto nl = output.find('\n');
        const bool only_branch =
            output.rfind("## ", 0) == 0
            && (nl == std::string::npos
                || output.find_first_not_of(" \r\n", nl) == std::string::npos);
        if (only_branch) {
            while (!output.empty()
                   && (output.back() == '\n' || output.back() == '\r'))
                output.pop_back();
            output += "\nworking tree clean";
        }
    }
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── git_diff ───────────────────────────────────────────────────────────

struct GitDiffArgs {
    std::string path;
    bool staged;
    std::string ref;
    std::string display_description;
};

std::expected<GitDiffArgs, ToolError> parse_git_diff_args(const json& j) {
    util::ArgReader ar(j);
    return GitDiffArgs{
        ar.str("path", ""),
        ar.boolean("staged", false),
        ar.str("ref", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_diff(const GitDiffArgs& a) {
    std::string checked;
    std::string pathspec;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_diff");
        if (!wp) return std::unexpected(std::move(wp.error()));
        checked  = wp->string();
        pathspec = wp->string();
    }
    const std::string git_dir = resolve_git_dir(checked);
    std::vector<std::string> argv = {"git", "-C", git_dir, "diff",
                                     "--stat", "-p"};
    if (a.staged) argv.push_back("--cached");
    if (!a.ref.empty()) argv.push_back(a.ref);
    if (!pathspec.empty()) {
        argv.push_back("--");
        argv.push_back(pathspec);
    }
    auto out = run_git(argv, "git_diff", 50'000);
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) return ToolOutput{"no changes", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── git_log ────────────────────────────────────────────────────────────

struct GitLogArgs {
    int count;
    std::string path;
    std::string ref;
    bool oneline;
    std::string display_description;
};

std::expected<GitLogArgs, ToolError> parse_git_log_args(const json& j) {
    util::ArgReader ar(j);
    return GitLogArgs{
        ar.integer("count", 20),
        ar.str("path", ""),
        ar.str("ref", "HEAD"),
        ar.boolean("oneline", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_log(const GitLogArgs& a) {
    int n = a.count;
    if (n <= 0) n = 20;
    if (n > 1000) n = 1000;

    std::string checked;
    std::string pathspec;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_log");
        if (!wp) return std::unexpected(std::move(wp.error()));
        checked  = wp->string();
        pathspec = wp->string();
    }
    const std::string git_dir = resolve_git_dir(checked);
    std::vector<std::string> argv = {"git", "-C", git_dir, "log"};
    if (a.oneline) {
        argv.push_back("--oneline");
    } else {
        argv.push_back("--format=%h %ad %an%n  %s");
        argv.push_back("--date=short");
    }
    argv.push_back("-" + std::to_string(n));
    argv.push_back(a.ref.empty() ? std::string{"HEAD"} : a.ref);
    if (!pathspec.empty()) {
        argv.push_back("--");
        argv.push_back(pathspec);
    }
    auto out = run_git(argv, "git_log");
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) return ToolOutput{"no commits", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── git_commit ─────────────────────────────────────────────────────────

struct GitCommitArgs {
    std::string message;
    std::vector<std::string> files;
    bool stage_all;
    std::string path;
    std::string display_description;
};

std::expected<GitCommitArgs, ToolError> parse_git_commit_args(const json& j) {
    util::ArgReader ar(j);
    auto msg_opt = ar.require_str("message");
    if (!msg_opt)
        return std::unexpected(ToolError::invalid_args("commit message required"));

    std::string msg = std::move(*msg_opt);
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(msg.begin(), msg.end(), not_space);
    auto last  = std::find_if(msg.rbegin(), msg.rend(), not_space).base();
    if (first >= last)
        return std::unexpected(ToolError::invalid_args(
            "commit message is empty / whitespace only"));
    msg.assign(first, last);

    std::vector<std::string> files;
    if (const json* f = ar.raw("files"); f && f->is_array()) {
        files.reserve(f->size());
        for (const auto& el : *f) {
            if (el.is_string()) {
                auto s = el.get<std::string>();
                if (!s.empty()) files.push_back(std::move(s));
            }
        }
    }

    return GitCommitArgs{
        std::move(msg),
        std::move(files),
        ar.boolean("stage_all", false),
        ar.str("path", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_commit(const GitCommitArgs& a) {
    // Resolve the repo to commit in from (in priority order): the explicit
    // `path` arg, the first staged file, else the workspace root. This is
    // what makes committing files in a sibling checkout Just Work instead
    // of failing with "not inside a git repository" when the process cwd
    // happens to be elsewhere.
    std::string repo_hint;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_commit");
        if (!wp) return std::unexpected(std::move(wp.error()));
        repo_hint = wp->string();
    } else if (!a.files.empty()) {
        auto wp = util::make_workspace_path_checked(a.files.front(),
                                                    "git_commit");
        if (!wp) return std::unexpected(std::move(wp.error()));
        repo_hint = wp->string();
    }
    const std::string git_dir = resolve_git_dir(repo_hint);

    if (a.stage_all) {
        if (auto r = run_git({"git", "-C", git_dir, "add", "-A"},
                             "git_commit (add -A)"); !r)
            return std::unexpected(std::move(r.error()));
    }
    for (const auto& f : a.files) {
        auto wp = util::make_workspace_path_checked(f, "git_commit");
        if (!wp) return std::unexpected(std::move(wp.error()));
        if (auto r = run_git({"git", "-C", git_dir, "add", "--",
                              wp->string()}, "git_commit (add)"); !r)
            return std::unexpected(std::move(r.error()));
    }

    auto r = util::run_argv_s(
        {"git", "-C", git_dir, "commit", "-m", a.message});
    if (!r.started || r.timed_out || r.exit_code != 0) {
        std::string_view out = r.output;
        if (out.find("nothing to commit") != std::string_view::npos
         || out.find("no changes added to commit") != std::string_view::npos)
            return std::unexpected(ToolError::invalid_args(
                "nothing to commit — working tree clean, or no files staged. "
                "Pass `stage_all: true`, or list files in `files: [...]`."));
        return std::unexpected(classify_git_failure(r, "git_commit"));
    }
    std::string output = std::move(r.output);
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── Schemas ────────────────────────────────────────────────────────────

json git_status_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path", {{"type","string"}, {"description","Repository path (default: cwd)"}}},
        }},
    };
}

json git_diff_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",    {{"type","string"}, {"description","File or directory to diff"}}},
            {"staged",  {{"type","boolean"}, {"description","Show staged changes (default: false)"}}},
            {"ref",     {{"type","string"}, {"description","Git ref or range (e.g. HEAD~3, main..HEAD)"}}},
        }},
    };
}

json git_log_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"count",   {{"type","integer"}, {"description","Number of commits (default: 20)"}}},
            {"path",    {{"type","string"}, {"description","Filter by file path"}}},
            {"ref",     {{"type","string"}, {"description","Branch or ref (default: HEAD)"}}},
            {"oneline", {{"type","boolean"}, {"description","One-line format (default: false)"}}},
        }},
    };
}

json git_commit_schema() {
    return json{
        {"type","object"},
        {"required", {"message"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"message",   {{"type","string"}, {"description","Commit message"}}},
            {"files",     {{"type","array"}, {"items",{{"type","string"}}},
                           {"description","Files to stage before committing"}}},
            {"stage_all", {{"type","boolean"}, {"description","Stage all changes (default: false)"}}},
            {"path",      {{"type","string"}, {"description","Repository path "
                           "(default: derived from files, else workspace root)"}}},
        }},
    };
}

} // namespace

void register_git_tools(Shells& sh) {
    sh.add("git_status",
        "Show the current git status: branch, staged/unstaged changes, "
        "untracked files, ahead/behind counts.",
        git_status_schema(), EffectSet{Effect::ReadFs},
        body<GitStatusArgs>(run_git_status, parse_git_status_args), 30'000);

    sh.add("git_diff",
        "Show git diff. By default shows unstaged changes. Use staged=true "
        "for staged changes, or specify a ref/range.",
        git_diff_schema(), EffectSet{Effect::ReadFs},
        body<GitDiffArgs>(run_git_diff, parse_git_diff_args), 60'000);

    sh.add("git_log",
        "Show git commit history. Returns commit hash, author, date, and message.",
        git_log_schema(), EffectSet{Effect::ReadFs},
        body<GitLogArgs>(run_git_log, parse_git_log_args), 30'000);

    sh.add("git_commit",
        "Stage files and create a git commit. Specify files to stage, "
        "or use stage_all to stage everything.",
        git_commit_schema(), EffectSet{Effect::WriteFs},
        body<GitCommitArgs>(run_git_commit, parse_git_commit_args), 0);
}

} // namespace mcp::tools::detail
