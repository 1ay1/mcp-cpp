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

namespace fs = std::filesystem;
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

// Reject a ref/range that would be parsed by git as an OPTION rather than a
// revision. `a.ref` is pushed as a bare positional before the `--` separator,
// so a value like "--output=/etc/x", "--upload-pack=…", or "-p" would smuggle
// a git flag in. Every legitimate ref/range (HEAD~3, main..HEAD, abc:file,
// v1.2.0, a raw sha) starts with an alnum, `_`, `.`, or `/` — never `-`.
[[nodiscard]] std::expected<void, ToolError> validate_ref(std::string_view ref) {
    if (!ref.empty() && ref.front() == '-')
        return std::unexpected(ToolError::invalid_args(
            "ref may not begin with '-' (looks like a git option, not a "
            "revision): '" + std::string{ref} + "'"));
    return {};
}

// Component-wise "child is under root" — a plain string startsWith would
// let /home/user/project-other through when root is /home/user/project.
[[nodiscard]] bool path_under(const std::filesystem::path& child,
                              const std::filesystem::path& root) {
    auto ci = child.begin();
    auto ri = root.begin();
    for (; ri != root.end() && ci != child.end(); ++ri, ++ci)
        if (*ri != *ci) return false;
    return ri == root.end();
}

// The directory to run git in WHEN no explicit `path` was given. The
// workspace root is the filesystem *access* boundary and is routinely
// widened all the way to `--workspace /` for full-disk power — but `-w /`
// is not a claim that the user's project IS the filesystem root, and
// `git -C / status` just fails with "not a git repository". agentty never
// chdir's away from the directory it was launched in, so the PROCESS CWD
// is the user's actual project. Prefer it (checkpoint.cpp resolves the
// active repo the same way): probe from cwd, and if the enclosing repo
// toplevel is inside the access boundary, that's the repo to use. Only
// fall back to the workspace root itself when cwd is unusable or its repo
// escapes the boundary.
std::filesystem::path default_git_start() {
    namespace fs = std::filesystem;
    const fs::path& ws = util::workspace_root();
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec || cwd.empty()) return ws;

    fs::path cwdc = fs::weakly_canonical(cwd, ec);
    if (ec || cwdc.empty()) cwdc = cwd;
    fs::path wsc = fs::weakly_canonical(ws, ec);
    if (ec || wsc.empty()) wsc = ws;
    // cwd must itself be inside the access boundary; if the user launched
    // outside a wider `-w` scope (unusual) fall back to the boundary.
    return path_under(cwdc, wsc) ? cwd : ws;
}

// Resolve the repository directory to run git in. Given a raw path (a
// directory OR a file — the workspace-checked string for the tool's
// `path`/first `files[]` entry), ask git for the enclosing worktree
// toplevel so every subcommand runs against the RIGHT repo regardless
// of the process cwd. When `checked` is empty (no explicit `path` arg),
// start from default_git_start() — the process cwd (the project) — rather
// than the raw access boundary, so `--workspace /` keeps full filesystem
// power without breaking git on the project you launched in. A repository
// rooted above the workspace is rejected: merely replacing its path with
// the workspace is not containment because Git would discover the same
// parent repository again.
std::expected<std::string, ToolError>
resolve_git_dir(std::string_view checked) {
    namespace fs = std::filesystem;
    fs::path start = checked.empty()
        ? default_git_start()
        : fs::path{std::string{checked}};
    std::error_code ec;
    // If `start` is a file, its parent is the directory to probe.
    fs::path dir = start;
    if (!checked.empty() && !fs::is_directory(start, ec))
        dir = start.parent_path();
    if (dir.empty()) dir = default_git_start();

    auto r = util::run_argv_s(
        {"git", "-C", dir.string(), "rev-parse", "--show-toplevel"}, 4096);
    if (r.started && !r.timed_out && r.exit_code == 0) {
        std::string top = std::move(r.output);
        while (!top.empty() && (top.back() == '\n' || top.back() == '\r'))
            top.pop_back();
        if (!top.empty()) {
            const fs::path& ws = util::workspace_root();
            fs::path topc = fs::weakly_canonical(fs::path{top}, ec);
            fs::path wsc  = fs::weakly_canonical(ws, ec);
            if (path_under(topc, wsc)) return top;
            return std::unexpected(ToolError::out_of_workspace(
                "git repository root '" + topc.string()
                + "' is outside workspace root '" + wsc.string() + "'"));
        }
    }
    // rev-parse failed (not a repo yet, or git missing) — fall back to the
    // directory itself; the subcommand's own error is clearer than ours.
    return dir.string();
}

// Names of submodules under `git_dir` whose WORKING TREE is dirty (modified,
// untracked-content, or a checked-out commit different from the one recorded
// in the superproject). `git status --porcelain` marks these repo entries but
// gives no detail, and — crucially — `git diff` shows NOTHING for a submodule
// dirtied only by untracked content, so a user who sees `? mcp-cpp` in status
// and then runs git_diff gets a confusing empty result. We surface the names
// so the callers can point the user INTO the submodule. Best-effort: any
// failure yields an empty list (never an error — this is only a hint).
std::vector<std::string> dirty_submodules(const std::string& git_dir) {
    std::vector<std::string> out;
    // Fast path: no `.gitmodules` at the repo root ⇒ no submodules ⇒ don't
    // pay for a `git submodule foreach` subprocess on every status/diff in
    // the overwhelmingly common non-submodule repo.
    std::error_code ec;
    if (!fs::exists(fs::path{git_dir} / ".gitmodules", ec)) return out;
    // `git submodule status` prefixes each line with a status char:
    //   ' ' clean, '+' checked-out commit differs, '-' not initialised,
    //   'U' merge conflicts. A trailing ` (dirty)`-style suffix isn't emitted
    //   here, so we also consult `status --porcelain` for working-tree dirt.
    // Path column of any porcelain row whose entry is a submodule shows up as
    // a plain `?`/`M` against the submodule dir; `--porcelain` alone can't
    // tell us it's a submodule. So ask git directly for the submodule paths
    // that are not clean.
    auto r = util::run_argv_s(
        {"git", "-C", git_dir, "submodule", "--quiet", "foreach",
         "--recursive",
         // Print the submodule's display path when its own working tree is
         // not clean. $displaypath and $sm_path are set by `foreach`.
         "git diff --quiet && git diff --cached --quiet && "
         "test -z \"$(git status --porcelain)\" || echo \"$displaypath\""},
        8192);
    if (!r.started || r.timed_out || r.exit_code != 0) return out;
    std::string_view sv{r.output};
    size_t pos = 0;
    while (pos < sv.size()) {
        size_t nl = sv.find('\n', pos);
        std::string_view line = sv.substr(pos, nl == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (!line.empty()) out.emplace_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

// ── git_status ─────────────────────────────────────────────────────────

// Parse `git status --porcelain=v1 --branch` into a compact one-line digest
// the MODEL reads at a glance: "On branch main, ahead 2 · 1 staged, 3
// modified, 2 untracked". The agentty UI computes its own header from the
// porcelain rows, so this is complementary and lives ABOVE the `## ` line
// (never starting a line with `## ` or an `XY ` change row) so it can't
// perturb either parser. Returns empty when the porcelain has no `## ` line.
std::string status_summary(std::string_view porcelain) {
    std::string branch;
    std::string upstream_delta;   // "ahead 2", "behind 1", "ahead 2, behind 1"
    int staged = 0, modified = 0, untracked = 0, conflicts = 0;
    bool seen_branch = false;
    size_t lo = 0;
    while (lo < porcelain.size()) {
        size_t eol = porcelain.find('\n', lo);
        std::string_view line = porcelain.substr(
            lo, (eol == std::string_view::npos ? porcelain.size() : eol) - lo);
        if (!seen_branch && line.rfind("## ", 0) == 0) {
            seen_branch = true;
            std::string_view b = line.substr(3);
            size_t cut = b.size();
            if (auto d = b.find("..."); d != std::string_view::npos)
                cut = std::min(cut, d);
            if (auto s = b.find(' '); s != std::string_view::npos)
                cut = std::min(cut, s);
            branch = std::string{b.substr(0, cut)};
            // Ahead/behind live in the trailing `[ahead N, behind M]`.
            if (auto lb = b.find('['); lb != std::string_view::npos) {
                if (auto rb = b.find(']', lb); rb != std::string_view::npos)
                    upstream_delta = std::string{b.substr(lb + 1, rb - lb - 1)};
            }
        } else if (seen_branch && line.size() >= 2) {
            char x = line[0], y = line[1];
            if (x == '?' && y == '?')            ++untracked;
            else if (x == 'U' || y == 'U'
                     || (x == 'A' && y == 'A')
                     || (x == 'D' && y == 'D'))  ++conflicts;
            else {
                if (x != ' ' && x != '?')        ++staged;    // index side
                if (y != ' ' && y != '?')        ++modified;  // worktree side
            }
        }
        if (eol == std::string_view::npos) break;
        lo = eol + 1;
    }
    if (!seen_branch) return {};
    std::string s = "On branch "
        + (branch.empty() ? std::string{"(detached HEAD)"} : branch);
    if (!upstream_delta.empty()) s += ", " + upstream_delta;
    std::string counts;
    auto add = [&](int n, const char* label) {
        if (n <= 0) return;
        if (!counts.empty()) counts += ", ";
        counts += std::to_string(n) + " " + label;
    };
    add(staged, "staged");
    add(modified, "modified");
    add(untracked, "untracked");
    add(conflicts, "conflicted");
    s += counts.empty() ? " \xc2\xb7 clean" : (" \xc2\xb7 " + counts);
    return s;
}

struct GitStatusArgs {
    std::string root;
    std::string display_description;
};

std::expected<GitStatusArgs, ToolError> parse_git_status_args(const json& j) {
    util::ArgReader ar(j);
    // Default is empty, NOT ".": an empty path lets resolve_git_dir() pick
    // the smart default (the process cwd / the project). A literal "." is
    // workspace-checked into the access boundary, which under `--workspace /`
    // becomes `/` and makes `git -C / status` fail "not a git repository".
    return GitStatusArgs{
        ar.str("path", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_status(const GitStatusArgs& a) {
    std::string checked;
    if (!a.root.empty()) {
        auto wp = util::make_workspace_path_checked(a.root, "git_status");
        if (!wp) return std::unexpected(std::move(wp.error()));
        checked = wp->string();
    }
    auto git_dir = resolve_git_dir(checked);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    // porcelain=v1 (the `git status -s` short format: `XY path`, one line per
    // change, plus a `## branch...upstream [ahead/behind]` header). Stable
    // and script-parseable like v2, but READABLE — v2's
    // `1 .M N... 100644 100644 100644 <sha> <sha> path` machine rows are
    // gibberish to a human and to the model. The tool card body shows this
    // verbatim, so it must be the form a person would want to read.
    auto out = run_git({"git", "-C", *git_dir, "status",
                        "--porcelain=v1", "--branch"}, "git_status");
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    // A compact digest the model reads without parsing porcelain. Sits ABOVE
    // the `## ` header so neither the porcelain nor the UI parser is touched.
    std::string summary = status_summary(output);
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
    // A submodule shows up as a single `M`/`?` row (e.g. ` M mcp-cpp`) with no
    // indication of WHAT changed inside it. Name the dirty ones so the user
    // knows a git_diff/git_commit needs to target the submodule, not the
    // superproject. Best-effort; skipped silently if there are none.
    if (auto subs = dirty_submodules(*git_dir); !subs.empty()) {
        while (!output.empty()
               && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        output += "\n\nsubmodules with uncommitted changes (diff/commit inside "
                  "them, not here): ";
        for (size_t i = 0; i < subs.size(); ++i)
            output += (i ? ", " : "") + subs[i];
    }
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    if (!summary.empty())
        output = summary + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── git_diff ───────────────────────────────────────────────────────────

struct GitDiffArgs {
    std::string path;
    bool staged;
    std::string ref;
    bool stat_only;
    int context;
    std::string display_description;
};

std::expected<GitDiffArgs, ToolError> parse_git_diff_args(const json& j) {
    util::ArgReader ar(j);
    return GitDiffArgs{
        ar.str("path", ""),
        ar.boolean("staged", false),
        ar.str("ref", ""),
        ar.boolean("stat_only", false),
        ar.integer("context", 3),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_diff(const GitDiffArgs& a) {
    if (auto v = validate_ref(a.ref); !v) return std::unexpected(std::move(v.error()));
    std::string checked;
    std::string pathspec;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_diff");
        if (!wp) return std::unexpected(std::move(wp.error()));
        checked  = wp->string();
        pathspec = wp->string();
    }
    auto git_dir = resolve_git_dir(checked);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    std::vector<std::string> argv = {"git", "-C", *git_dir, "diff",
                                     "--stat"};
    // stat_only: just the per-file summary — cheap for huge diffs and enough
    // for the model to decide where to look. Otherwise include the patch,
    // with a caller-tunable context window (clamped to a sane range).
    if (!a.stat_only) {
        argv.push_back("-p");
        int ctx = a.context;
        if (ctx < 0)   ctx = 0;
        if (ctx > 100) ctx = 100;
        argv.push_back("--unified=" + std::to_string(ctx));
    }
    if (a.staged) argv.push_back("--cached");
    if (!a.ref.empty()) argv.push_back(a.ref);
    if (!pathspec.empty()) {
        argv.push_back("--");
        argv.push_back(pathspec);
    }
    auto out = run_git(argv, "git_diff", 50'000);
    if (!out) return std::unexpected(std::move(out.error()));
    std::string output = std::move(*out);
    if (output.empty()) {
        // `git diff` on a superproject shows nothing for a submodule that is
        // dirty only in its working tree (esp. untracked content) — the
        // recorded commit pointer hasn't moved. Without a hint the user sees
        // "no changes" right after git_status flagged the submodule. Point
        // them inside. Only for a whole-repo diff (no pathspec/ref).
        if (pathspec.empty() && a.ref.empty()) {
            auto subs = dirty_submodules(*git_dir);
            if (!subs.empty()) {
                std::string hint = "no changes in this repo, but these "
                    "submodules have uncommitted changes: ";
                for (size_t i = 0; i < subs.size(); ++i)
                    hint += (i ? ", " : "") + subs[i];
                hint += ".\nDiff inside one with e.g. git_diff path=\""
                     + subs.front() + "\".";
                return ToolOutput{std::move(hint), std::nullopt};
            }
        }
        return ToolOutput{"no changes", std::nullopt};
    }
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
    if (auto v = validate_ref(a.ref); !v) return std::unexpected(std::move(v.error()));
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
    auto git_dir = resolve_git_dir(checked);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    std::vector<std::string> argv = {"git", "-C", *git_dir, "log"};
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
    bool amend;
    std::string path;
    std::string display_description;
};

std::expected<GitCommitArgs, ToolError> parse_git_commit_args(const json& j) {
    util::ArgReader ar(j);
    const bool amend = ar.boolean("amend", false);
    auto msg_opt = ar.require_str("message");
    // An amend may reuse the existing message (`--amend --no-edit`), so the
    // message is optional in that mode only.
    if (!msg_opt && !amend)
        return std::unexpected(ToolError::invalid_args("commit message required"));

    std::string msg = msg_opt ? std::move(*msg_opt) : std::string{};
    if (!msg.empty()) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        auto first = std::find_if(msg.begin(), msg.end(), not_space);
        auto last  = std::find_if(msg.rbegin(), msg.rend(), not_space).base();
        if (first >= last) {
            if (!amend)
                return std::unexpected(ToolError::invalid_args(
                    "commit message is empty / whitespace only"));
            msg.clear();   // amend + blank message ⇒ keep the old message
        } else {
            msg.assign(first, last);
        }
    }

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
        amend,
        ar.str("path", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_commit(const GitCommitArgs& a) {
    // Resolve the repo to commit in from (in priority order): the explicit
    // `path` arg, the first staged file, else the smart default (the process
    // cwd — the project). This is what makes committing files in a sibling
    // checkout Just Work instead of failing with "not inside a git
    // repository" when the workspace boundary is wide (e.g. `--workspace /`).
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
    auto git_dir = resolve_git_dir(repo_hint);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));

    if (a.stage_all) {
        if (auto r = run_git({"git", "-C", *git_dir, "add", "-A"},
                             "git_commit (add -A)"); !r)
            return std::unexpected(std::move(r.error()));
    }
    for (const auto& f : a.files) {
        // Relative file lists are conventionally relative to the repository
        // named by `path`, not to agentty's workspace root. The old code always
        // resolved them against the workspace, so committing
        // path="maya", files=["include/…"] tried to stage
        // <workspace>/include/… and failed before git ran. Still accept an
        // explicitly workspace-relative path ("maya/include/…") when it
        // already falls inside this repo.
        fs::path stage_path;
        fs::path raw{f};
        if (raw.is_absolute()) {
            auto wp = util::make_workspace_path_checked(f, "git_commit");
            if (!wp) return std::unexpected(std::move(wp.error()));
            stage_path = fs::path{wp->string()};
        } else {
            auto workspace_path = util::make_workspace_path_checked(f, "git_commit");
            if (!workspace_path)
                return std::unexpected(std::move(workspace_path.error()));

            std::error_code ec;
            const fs::path checked_workspace{workspace_path->string()};
            auto rel = fs::relative(checked_workspace, fs::path{*git_dir}, ec);
            const bool inside_repo = !ec && !rel.empty()
                && *rel.begin() != fs::path{".."};
            if (inside_repo) {
                stage_path = checked_workspace;
            } else {
                auto repo_relative = util::make_workspace_path_checked(
                    (fs::path{*git_dir} / raw).string(), "git_commit");
                if (!repo_relative)
                    return std::unexpected(std::move(repo_relative.error()));
                stage_path = fs::path{repo_relative->string()};
            }
        }
        if (auto r = run_git({"git", "-C", *git_dir, "add", "--",
                              stage_path.string()}, "git_commit (add)"); !r)
            return std::unexpected(std::move(r.error()));
    }

    auto r = util::run_argv_s(
        [&] {
            std::vector<std::string> argv{"git", "-C", *git_dir, "commit"};
            if (a.amend) {
                argv.push_back("--amend");
                if (a.message.empty()) argv.push_back("--no-edit");
            }
            if (!a.message.empty()) {
                argv.push_back("-m");
                argv.push_back(a.message);
            }
            return argv;
        }());
    if (!r.started || r.timed_out || r.exit_code != 0) {
        std::string_view out = r.output;
        if (out.find("nothing to commit") != std::string_view::npos
         || out.find("no changes added to commit") != std::string_view::npos)
            return std::unexpected(ToolError::invalid_args(
                "nothing to commit — working tree clean, or no files staged. "
                "Pass `stage_all: true`, or list files in `files: [...]`."));
        return std::unexpected(classify_git_failure(r, "git_commit"));
    }
    // git's own commit summary is noisy (`[branch abc123] subject` + churn
    // stats spread across lines). Synthesize a clean, uniform one instead:
    // "<shorthash> <subject>  (N files changed, +A/-D)" on the current branch.
    std::string output;
    {
        auto hash = util::run_argv_s(
            {"git", "-C", *git_dir, "rev-parse", "--short", "HEAD"}, 256);
        auto subj = util::run_argv_s(
            {"git", "-C", *git_dir, "log", "-1", "--format=%s"}, 4096);
        auto brch = util::run_argv_s(
            {"git", "-C", *git_dir, "rev-parse", "--abbrev-ref", "HEAD"}, 256);
        auto stat = util::run_argv_s(
            {"git", "-C", *git_dir, "show", "--stat", "--format=", "HEAD"},
            8192);
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            return s;
        };
        std::string h = trim(std::move(hash.output));
        std::string s = trim(std::move(subj.output));
        std::string b = trim(std::move(brch.output));
        // Last non-empty line of --stat is the ` N files changed, ...` summary.
        std::string st = stat.output;
        std::string summary_line;
        {
            size_t end = st.find_last_not_of("\n\r");
            if (end != std::string::npos) {
                size_t bol = st.rfind('\n', end);
                summary_line = st.substr(bol == std::string::npos ? 0 : bol + 1,
                                         end - (bol == std::string::npos ? 0 : bol));
                // strip leading spaces git puts on the summary line
                size_t nb = summary_line.find_first_not_of(' ');
                if (nb != std::string::npos) summary_line.erase(0, nb);
            }
        }
        output = (a.amend ? "amended " : "committed ");
        if (!h.empty()) output += h + " ";
        output += "on " + (b.empty() ? std::string{"(detached)"} : b);
        if (!s.empty()) output += "\n  " + s;
        if (!summary_line.empty()) output += "\n  " + summary_line;
    }
    if (!a.display_description.empty())
        output = a.display_description + "\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

// ── git_show / git_blame ───────────────────────────────────────────────

struct GitShowArgs {
    std::string ref;
    std::string path;
    bool file_content = false;
};

std::expected<GitShowArgs, ToolError> parse_git_show_args(const json& j) {
    util::ArgReader ar(j);
    const auto format = ar.str("format", "commit");
    if (format != "commit" && format != "file")
        return std::unexpected(ToolError::invalid_args("format must be 'commit' or 'file'"));
    auto path = ar.str("path", "");
    if (format == "file" && path.empty())
        return std::unexpected(ToolError::invalid_args("path is required when format=file"));
    return GitShowArgs{ar.str("ref", "HEAD"), std::move(path), format == "file"};
}

ExecResult run_git_show(const GitShowArgs& a) {
    if (auto v = validate_ref(a.ref); !v) return std::unexpected(std::move(v.error()));
    std::string checked_path;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_show");
        if (!wp) return std::unexpected(wp.error());
        checked_path = wp->string();
    }
    auto git_dir = resolve_git_dir(checked_path);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    std::vector<std::string> argv{"git", "-C", *git_dir, "show"};
    if (a.file_content) {
        std::error_code ec;
        auto relative = fs::relative(checked_path, *git_dir, ec);
        if (ec || relative.empty())
            return std::unexpected(ToolError::invalid_args("path is not inside the repository"));
        argv.push_back(a.ref + ":" + relative.generic_string());
    } else {
        argv.insert(argv.end(), {"--format=fuller", "--stat", "--patch", a.ref});
        if (!checked_path.empty()) {
            argv.push_back("--");
            argv.push_back(checked_path);
        }
    }
    auto out = run_git(argv, "git_show");
    if (!out) return std::unexpected(out.error());
    return ToolOutput{out->empty() ? "(no output)" : std::move(*out), std::nullopt};
}

struct GitBlameArgs { std::string path; std::string ref; int start = 0; int end = 0; };

std::expected<GitBlameArgs, ToolError> parse_git_blame_args(const json& j) {
    util::ArgReader ar(j);
    auto path = ar.require_str("path");
    if (!path || path->empty())
        return std::unexpected(ToolError::invalid_args("path is required"));
    int start = ar.integer("start_line", 0);
    int end = ar.integer("end_line", 0);
    if ((start > 0 || end > 0) && (start < 1 || end < start))
        return std::unexpected(ToolError::invalid_args("line range must satisfy 1 <= start_line <= end_line"));
    return GitBlameArgs{*path, ar.str("ref", "HEAD"), start, end};
}

ExecResult run_git_blame(const GitBlameArgs& a) {
    if (auto v = validate_ref(a.ref); !v) return std::unexpected(std::move(v.error()));
    auto wp = util::make_workspace_path_checked(a.path, "git_blame");
    if (!wp) return std::unexpected(wp.error());
    auto git_dir = resolve_git_dir(wp->string());
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    std::vector<std::string> argv{"git", "-C", *git_dir, "blame", "--date=short"};
    if (a.start > 0) argv.insert(argv.end(), {"-L", std::to_string(a.start) + "," + std::to_string(a.end)});
    argv.push_back(a.ref);
    argv.push_back("--");
    argv.push_back(wp->string());
    auto out = run_git(argv, "git_blame");
    if (!out) return std::unexpected(out.error());
    return ToolOutput{out->empty() ? "(no blame information)" : std::move(*out), std::nullopt};
}

// ── git_branch ───────────────────────────────────────────────────────

// A single tool for the branch operations an agent actually needs, gated by
// `action`: list (default, read-only), create, switch (checkout, optionally
// creating), delete. Kept as ONE tool rather than four so the model has a
// small, obvious surface — mirrors how `git_show` folds commit/file.
struct GitBranchArgs {
    std::string action;   // list | create | switch | delete
    std::string name;
    std::string start_point;
    bool force = false;
    std::string path;
    std::string display_description;
};

std::expected<GitBranchArgs, ToolError> parse_git_branch_args(const json& j) {
    util::ArgReader ar(j);
    std::string action = ar.str("action", "list");
    if (action != "list" && action != "create"
     && action != "switch" && action != "delete")
        return std::unexpected(ToolError::invalid_args(
            "action must be one of: list, create, switch, delete"));
    std::string name = ar.str("name", "");
    if (action != "list" && name.empty())
        return std::unexpected(ToolError::invalid_args(
            "branch `name` is required for action=" + action));
    // A branch name may not look like an option or contain shell/ref-unsafe
    // characters. validate_ref already rejects a leading '-'; reuse it and
    // add a whitespace guard (git would reject these anyway, but a clear
    // up-front error beats a cryptic git one).
    if (!name.empty()) {
        if (auto v = validate_ref(name); !v)
            return std::unexpected(std::move(v.error()));
        if (name.find_first_of(" \t\n") != std::string::npos)
            return std::unexpected(ToolError::invalid_args(
                "branch name may not contain whitespace: '" + name + "'"));
    }
    return GitBranchArgs{
        std::move(action),
        std::move(name),
        ar.str("start_point", ""),
        ar.boolean("force", false),
        ar.str("path", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_branch(const GitBranchArgs& a) {
    std::string checked;
    if (!a.path.empty()) {
        auto wp = util::make_workspace_path_checked(a.path, "git_branch");
        if (!wp) return std::unexpected(std::move(wp.error()));
        checked = wp->string();
    }
    auto git_dir = resolve_git_dir(checked);
    if (!git_dir) return std::unexpected(std::move(git_dir.error()));
    const std::string& d = *git_dir;

    if (a.action == "list") {
        // Columns: current marker, name, upstream tracking, last-commit subject.
        auto out = run_git(
            {"git", "-C", d, "branch", "--list",
             "--format=%(if)%(HEAD)%(then)* %(else)  %(end)"
             "%(refname:short)\t%(upstream:short)\t%(contents:subject)"},
            "git_branch");
        if (!out) return std::unexpected(std::move(out.error()));
        return ToolOutput{out->empty() ? "(no branches)" : std::move(*out),
                          std::nullopt};
    }

    if (a.action == "create") {
        std::vector<std::string> argv{"git", "-C", d, "branch"};
        if (a.force) argv.push_back("--force");
        argv.push_back(a.name);
        if (!a.start_point.empty()) argv.push_back(a.start_point);
        if (auto r = run_git(argv, "git_branch (create)"); !r)
            return std::unexpected(std::move(r.error()));
        return ToolOutput{"created branch " + a.name
            + (a.start_point.empty() ? "" : " at " + a.start_point),
            std::nullopt};
    }

    if (a.action == "switch") {
        // `git switch` is the modern, safer verb: it refuses to switch away
        // from an operation and won't detach unless asked. Create-and-switch
        // when a start_point is given OR the branch doesn't exist yet.
        std::vector<std::string> argv{"git", "-C", d, "switch"};
        bool exists = false;
        {
            auto chk = util::run_argv_s(
                {"git", "-C", d, "rev-parse", "--verify", "--quiet",
                 "refs/heads/" + a.name}, 128);
            exists = chk.started && chk.exit_code == 0;
        }
        if (!exists || !a.start_point.empty()) {
            argv.push_back(a.force ? "-C" : "-c");   // create (or force-create)
            argv.push_back(a.name);
            if (!a.start_point.empty()) argv.push_back(a.start_point);
        } else {
            argv.push_back(a.name);
        }
        if (auto r = run_git(argv, "git_branch (switch)"); !r)
            return std::unexpected(std::move(r.error()));
        return ToolOutput{(exists && a.start_point.empty()
                              ? "switched to branch " : "created and switched to ")
                          + a.name, std::nullopt};
    }

    // delete
    std::vector<std::string> argv{"git", "-C", d, "branch",
                                  a.force ? "-D" : "-d", a.name};
    auto r = util::run_argv_s(argv);
    if (!r.started || r.timed_out || r.exit_code != 0) {
        std::string_view o = r.output;
        if (o.find("not fully merged") != std::string_view::npos)
            return std::unexpected(ToolError::invalid_args(
                "branch '" + a.name + "' is not fully merged; pass force=true "
                "to delete it anyway (this discards its unmerged commits)."));
        return std::unexpected(classify_git_failure(r, "git_branch (delete)"));
    }
    return ToolOutput{"deleted branch " + a.name, std::nullopt};
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
            {"stat_only", {{"type","boolean"}, {"description","Only the per-file "
                           "change summary, no patch body — cheap for large diffs (default: false)"}}},
            {"context", {{"type","integer"}, {"description","Lines of context "
                         "around each hunk (default: 3, 0–100)"}}},
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

json git_show_schema() {
    return json{{"type","object"}, {"properties", {
        {"ref", {{"type","string"}, {"description","Commit/ref (default HEAD)."}}},
        {"path", {{"type","string"}, {"description","Optional workspace file/path filter."}}},
        {"format", {{"type","string"}, {"enum", {"commit","file"}},
                    {"description","commit shows metadata+patch; file shows file contents at ref."}}}
    }}};
}

json git_blame_schema() {
    return json{{"type","object"}, {"required", {"path"}}, {"properties", {
        {"path", {{"type","string"}, {"description","Workspace file to annotate."}}},
        {"ref", {{"type","string"}, {"description","Commit/ref (default HEAD)."}}},
        {"start_line", {{"type","integer"}, {"minimum",1}}},
        {"end_line", {{"type","integer"}, {"minimum",1}}}
    }}};
}

json git_commit_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"message",   {{"type","string"}, {"description","Commit message "
                           "(required unless amend=true, where it defaults to the existing message)"}}},
            {"files",     {{"type","array"}, {"items",{{"type","string"}}},
                           {"description","Files to stage before committing"}}},
            {"stage_all", {{"type","boolean"}, {"description","Stage all changes (default: false)"}}},
            {"amend",     {{"type","boolean"}, {"description","Amend the previous "
                           "commit instead of creating a new one; omit message to keep it (default: false)"}}},
            {"path",      {{"type","string"}, {"description","Repository path "
                           "(default: derived from files, else the current project)"}}},
        }},
    };
}

json git_branch_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"action", {{"type","string"}, {"enum", {"list","create","switch","delete"}},
                        {"description","list (default, read-only) | create | "
                         "switch (checkout, creating if needed) | delete"}}},
            {"name",   {{"type","string"}, {"description","Branch name "
                        "(required for create/switch/delete)"}}},
            {"start_point", {{"type","string"}, {"description","Ref to base a "
                             "new branch on (create/switch); default HEAD"}}},
            {"force",  {{"type","boolean"}, {"description","create: reset an "
                        "existing branch; delete: drop even if unmerged (default: false)"}}},
            {"path",   {{"type","string"}, {"description","Repository path "
                        "(default: the current project)"}}},
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

    sh.add("git_show",
        "Show a commit with metadata and patch, or read one file exactly as it existed at a revision.",
        git_show_schema(), EffectSet{Effect::ReadFs},
        body<GitShowArgs>(run_git_show, parse_git_show_args), 60'000);

    sh.add("git_blame",
        "Annotate a file or line range with the commit, author, date, and source line that last changed it.",
        git_blame_schema(), EffectSet{Effect::ReadFs},
        body<GitBlameArgs>(run_git_blame, parse_git_blame_args), 40'000);

    sh.add("git_commit",
        "Stage files and create a git commit. Specify files to stage, "
        "or use stage_all to stage everything.",
        git_commit_schema(), EffectSet{Effect::WriteFs},
        body<GitCommitArgs>(run_git_commit, parse_git_commit_args), 0);

    sh.add("git_branch",
        "List, create, switch, or delete git branches. action=list (default) "
        "is read-only; create/switch/delete take a `name`.",
        git_branch_schema(), EffectSet{Effect::WriteFs},
        body<GitBranchArgs>(run_git_branch, parse_git_branch_args), 20'000);
}

} // namespace mcp::tools::detail
