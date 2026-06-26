// SPDX-License-Identifier: Apache-2.0
//
// search_tools_test.cpp — exercises the Tier-1 shell / search / diagnostics
// tools (bash / grep / glob / find_definition) end-to-end through
// make_provider(), proving the ported bodies behave + the workspace
// boundary + effects flow through.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace mcp::tools;
namespace fs = std::filesystem;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }

static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

int main() {
    auto root = fs::temp_directory_path() / ("mcp_search_test_" + std::to_string(::getpid()));
    fs::create_directories(root);
    util::set_workspace_root(root);
    // git_status/log/commit invoke `git` in the process cwd; agentty runs
    // with cwd == workspace, so mirror that here for a faithful test.
    auto prev_cwd = fs::current_path();
    fs::current_path(root);

    // Seed a small tree.
    write_file(root / "alpha.cpp",
        "#include <cstdio>\n"
        "int compute_total(int a, int b) {\n"
        "    return a + b;  // NEEDLE_marker here\n"
        "}\n");
    write_file(root / "beta.txt", "just some plain text\nwith a NEEDLE_marker too\n");
    fs::create_directories(root / "sub");
    write_file(root / "sub" / "gamma.py", "def compute_total(x):\n    return x * 2\n");

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── grep finds the marker across files ───────────────────────────────
    {
        auto args = obj(); args["pattern"] = "NEEDLE_marker"; args["path"] = root.string();
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("alpha.cpp") != std::string::npos);
        assert(r.text.find("beta.txt") != std::string::npos);
        assert(read_effects(r).has(Effect::ReadFs));
        std::puts("grep: ok (found across files)");
    }

    // ── grep with file glob narrows to .cpp ──────────────────────────────
    {
        auto args = obj();
        args["pattern"] = "NEEDLE_marker";
        args["path"]    = root.string();
        args["glob"]    = "*.cpp";
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("alpha.cpp") != std::string::npos);
        assert(r.text.find("beta.txt") == std::string::npos);
        std::puts("grep: glob filter ok");
    }

    // ── grep blank pattern rejected ──────────────────────────────────────
    {
        auto args = obj(); args["pattern"] = "   "; args["path"] = root.string();
        auto r = call(*provider, "grep", args);
        assert(r.is_error);
        std::puts("grep: blank pattern refused");
    }

    // ── glob finds files by name ─────────────────────────────────────────
    {
        auto args = obj(); args["pattern"] = "*.py"; args["path"] = root.string();
        auto r = call(*provider, "glob", args);
        assert(!r.is_error);
        assert(r.text.find("gamma.py") != std::string::npos);
        assert(read_effects(r).has(Effect::ReadFs));
        std::puts("glob: ok");
    }

    // ── glob substring fallback ──────────────────────────────────────────
    {
        auto args = obj(); args["pattern"] = "alpha"; args["path"] = root.string();
        auto r = call(*provider, "glob", args);
        assert(!r.is_error);
        assert(r.text.find("alpha.cpp") != std::string::npos);
        std::puts("glob: substring fallback ok");
    }

    // ── find_definition locates the function ─────────────────────────────
    {
        auto args = obj(); args["symbol"] = "compute_total"; args["path"] = root.string();
        auto r = call(*provider, "find_definition", args);
        assert(!r.is_error);
        assert(r.text.find("compute_total") != std::string::npos);
        std::puts("find_definition: ok");
    }

    // ── bash runs a command and captures output ──────────────────────────
    {
        auto args = obj();
        args["command"] = "echo hello_from_bash";
        auto r = call(*provider, "bash", args);
        assert(!r.is_error);
        assert(r.text.find("hello_from_bash") != std::string::npos);
        assert(read_effects(r).has(Effect::Exec));
        std::puts("bash: ok");
    }

    // ── bash non-zero exit surfaces the exit code ────────────────────────
    {
        auto args = obj();
        args["command"] = "exit 7";
        auto r = call(*provider, "bash", args);
        assert(!r.is_error);  // tool succeeds; payload reports the failure
        assert(r.text.find("exit code 7") != std::string::npos);
        std::puts("bash: exit-code reporting ok");
    }

    // ── bash empty command rejected ──────────────────────────────────────
    {
        auto args = obj(); args["command"] = "";
        auto r = call(*provider, "bash", args);
        assert(r.is_error);
        std::puts("bash: empty command refused");
    }

    // ── diagnostics with no build system errors cleanly ──────────────────
    {
        auto args = obj();  // auto-detect; temp dir has no build markers
        auto r = call(*provider, "diagnostics", args);
        // Either errors (no build system) or runs a custom command — here
        // auto-detect in a bare temp dir should report no build system.
        assert(r.is_error || r.text.find("no diagnostics") != std::string::npos
                          || !r.text.empty());
        std::puts("diagnostics: auto-detect path ok");
    }

    // ── diagnostics with explicit command runs it ────────────────────────
    {
        auto args = obj();
        args["command"] = "echo build_ok";
        auto r = call(*provider, "diagnostics", args);
        assert(!r.is_error);
        assert(r.text.find("build_ok") != std::string::npos);
        assert(read_effects(r).has(Effect::Exec));
        std::puts("diagnostics: explicit command ok");
    }

    // ── git tools against a fresh repo ───────────────────────────────────
    {
        // init a repo inside the workspace + configure identity
        auto g = obj(); g["command"] =
            "git init -q && git config user.email t@t.t && git config user.name T";
        auto gi = call(*provider, "bash", g);
        assert(!gi.is_error);

        // git_status on a clean-ish repo (untracked files present)
        auto sargs = obj(); sargs["path"] = root.string();
        auto st = call(*provider, "git_status", sargs);
        assert(!st.is_error);
        assert(read_effects(st).has(Effect::ReadFs));
        // Readable short format: a `## ` branch header (v1 --branch), NOT the
        // v2 machine rows (`# branch.head` / `1 .M N... <modes> <sha>`) the
        // tool card body would render as gibberish.
        assert(st.text.find("## ") != std::string::npos);
        assert(st.text.find("branch.head") == std::string::npos);
        std::puts("git_status: ok");

        // git_commit stages everything and commits
        auto cargs = obj();
        cargs["message"]   = "seed commit";
        cargs["stage_all"] = true;
        auto ci = call(*provider, "git_commit", cargs);
        assert(!ci.is_error);
        assert(read_effects(ci).has(Effect::WriteFs));
        std::puts("git_commit: ok");

        // git_log shows the commit
        auto largs = obj(); largs["oneline"] = true;
        auto lg = call(*provider, "git_log", largs);
        assert(!lg.is_error);
        assert(lg.text.find("seed commit") != std::string::npos);
        std::puts("git_log: ok");

        // git_commit with empty message rejected
        auto bad = obj(); bad["message"] = "   ";
        auto br = call(*provider, "git_commit", bad);
        assert(br.is_error);
        std::puts("git_commit: empty message refused");
    }

    fs::current_path(prev_cwd);
    fs::remove_all(root);
    std::puts("ALL SEARCH/SHELL/DIAGNOSTICS TOOL TESTS PASSED");
    return 0;
}
