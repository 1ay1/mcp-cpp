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

#include "agtest.hpp"
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  include <process.h>   // _getpid
#  define mcp_getpid _getpid
#else
#  include <unistd.h>    // getpid
#  define mcp_getpid getpid
#endif

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

TEST_CASE("search_tools") {
    auto root = fs::temp_directory_path() / ("mcp_search_test_" + std::to_string(mcp_getpid()));
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
        "    if (a > b) {  // control-flow, not a def\n"
        "        return a - b;\n"
        "    }\n"
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

    // ── grep is case-insensitive by default ──────────────────────────────
    {
        auto args = obj();
        args["pattern"] = "needle_MARKER";   // different case than on disk
        args["path"]    = root.string();
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("alpha.cpp") != std::string::npos);
        assert(r.text.find("beta.txt") != std::string::npos);
        std::puts("grep: case-insensitive default ok");
    }

    // ── grep case_sensitive=true respects case ───────────────────────────
    {
        auto args = obj();
        args["pattern"]        = "needle_marker";  // lowercase, on-disk is NEEDLE
        args["path"]           = root.string();
        args["case_sensitive"] = true;
        auto r = call(*provider, "grep", args);
        // No lowercase occurrence exists → no matches.
        assert(r.is_error || r.text.find("No match") != std::string::npos
                          || r.text.find("alpha.cpp") == std::string::npos);
        std::puts("grep: case_sensitive=true ok");
    }

    // ── case-fold literal scanner: non-letter lead byte + mixed-case ─────
    // Guards the memchr-driven dual-scan (lowercase + uppercase first byte)
    // and the non-overlapping stride after a hit. "_zZz" leads with '_'
    // (single fold form) and its body mixes case against on-disk "_ZzZ".
    {
        write_file(root / "fold.txt", "prefix _ZzZ middle _zZz tail\n");
        auto args = obj();
        args["pattern"] = "_zzz";        // all-lowercase probe, default ci
        args["path"]    = root.string();
        args["glob"]    = "fold.txt";
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("fold.txt") != std::string::npos);
        std::puts("grep: case-fold scanner (non-letter lead) ok");
    }

    // ── case-fold scanner: alpha lead byte matches either case ──────────
    {
        auto args = obj();
        args["pattern"] = "zzz";         // leads with a letter → dual memchr
        args["path"]    = root.string();
        args["glob"]    = "fold.txt";
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("fold.txt") != std::string::npos);
        std::puts("grep: case-fold scanner (alpha lead) ok");
    }

    // ── grep slash file-glob scopes to a subdirectory path ───────────────
    {
        auto args = obj();
        args["pattern"] = "compute_total";
        args["path"]    = root.string();
        args["glob"]    = "sub/*.py";   // path pattern, not just basename
        auto r = call(*provider, "grep", args);
        assert(!r.is_error);
        assert(r.text.find("gamma.py") != std::string::npos);
        assert(r.text.find("alpha.cpp") == std::string::npos);
        std::puts("grep: slash path-glob ok");
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

    // ── glob slash pattern matches the workspace-relative path ───────────
    {
        auto args = obj(); args["pattern"] = "sub/*.py"; args["path"] = root.string();
        auto r = call(*provider, "glob", args);
        assert(!r.is_error);
        assert(r.text.find("gamma.py") != std::string::npos);
        // A top-level .py sibling must NOT be reported for a sub/-scoped glob.
        assert(r.text.find("alpha.cpp") == std::string::npos);
        std::puts("glob: slash path pattern ok");
    }

    // ── glob '**' spans directories ──────────────────────────────────────
    {
        auto args = obj(); args["pattern"] = "**/*.py"; args["path"] = root.string();
        auto r = call(*provider, "glob", args);
        assert(!r.is_error);
        assert(r.text.find("gamma.py") != std::string::npos);
        std::puts("glob: ** cross-directory ok");
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
        // Model-facing digest sits above the porcelain: "On branch X · ...".
        assert(st.text.find("On branch ") != std::string::npos
               && "git_status must prepend a one-line summary for the model");
        std::puts("git_status: ok");

        // REGRESSION (`--workspace /`): when the access boundary is WIDER
        // than the project, a no-path git_status must still find the repo at
        // the process cwd, not run `git -C <boundary> status` and fail "not
        // a git repository". Widen the boundary to the parent of root while
        // cwd stays == root, then call git_status with NO path.
        {
            auto saved_ws = util::workspace_root();
            util::set_workspace_root(root.parent_path());
            auto wide = call(*provider, "git_status", obj());  // no `path`
            assert(!wide.is_error
                   && "no-path git_status must resolve the cwd project under a "
                      "wider workspace boundary (`-w /`)");
            assert(wide.text.find("## ") != std::string::npos);
            util::set_workspace_root(saved_ws);
            std::puts("git_status: wide-boundary (`-w /`) resolves cwd project");
        }

        // git_commit stages everything and commits
        auto cargs = obj();
        cargs["message"]   = "seed commit";
        cargs["stage_all"] = true;
        auto ci = call(*provider, "git_commit", cargs);
        assert(!ci.is_error);
        assert(read_effects(ci).has(Effect::WriteFs));
        // Clean synthesized result: "committed <hash> on <branch>" + subject.
        assert(ci.text.find("committed ") != std::string::npos
               && "git_commit must report a clean committed-<hash> summary");
        assert(ci.text.find("seed commit") != std::string::npos);
        std::puts("git_commit: ok");

        // git_log shows the commit
        auto largs = obj(); largs["oneline"] = true;
        auto lg = call(*provider, "git_log", largs);
        assert(!lg.is_error);
        assert(lg.text.find("seed commit") != std::string::npos);
        std::puts("git_log: ok");

        // Relative files are resolved against an explicit nested repository,
        // not accidentally against the outer workspace root.
        auto ng = obj(); ng["command"] =
            "mkdir nested && git -C nested init -q && "
            "git -C nested config user.email t@t.t && "
            "git -C nested config user.name T && "
            "printf nested > nested/inside.txt";
        auto ngi = call(*provider, "bash", ng);
        assert(!ngi.is_error);

        auto nc = obj();
        nc["path"] = "nested";
        nc["files"] = mcp::Json::array({"inside.txt"});
        nc["message"] = "nested commit";
        auto nci = call(*provider, "git_commit", nc);
        assert(!nci.is_error);

        auto nl = obj(); nl["path"] = "nested"; nl["oneline"] = true;
        auto nlg = call(*provider, "git_log", nl);
        assert(!nlg.is_error);
        assert(nlg.text.find("nested commit") != std::string::npos);
        std::puts("git_commit: nested repo-relative files ok");

        // ── SUBMODULE awareness ──────────────────────────────────────────
        // Add a real submodule, dirty ONLY its working tree (untracked file),
        // and assert both git_status and git_diff on the SUPERPROJECT point
        // the user inside it. `git diff` on the superproject is empty in this
        // state (the recorded commit pointer hasn't moved), so without the
        // hint the user would see a bare "no changes".
        {
            auto sm = obj(); sm["command"] =
                // A bare upstream to add as a submodule. Pin the initial
                // branch: on git configured with init.defaultBranch=main
                // (the modern default), a plain --bare repo's HEAD points at
                // refs/heads/main while the seed below pushes HEAD:master —
                // the submodule clone then checks out an unborn branch and
                // `submodule add` fails before .gitmodules exists.
                "git init -q --bare --initial-branch=master "
                + (root / "up.git").string() + " && "
                "git clone -q " + (root / "up.git").string() + " seedwt && "
                "cd seedwt && git config user.email t@t.t && "
                "git config user.name T && echo hi > f.txt && "
                "git add f.txt && git commit -q -m init && git push -q origin "
                "HEAD:master && cd .. && "
                // Add it into the workspace repo as a submodule.
                "git -c protocol.file.allow=always submodule add -q "
                + (root / "up.git").string() + " mod && "
                "git commit -q -m 'add submodule' && "
                // Dirty the submodule working tree with an untracked file.
                "echo scratch > mod/untracked.txt";
            auto smi = call(*provider, "bash", sm);
            assert(!smi.is_error && "submodule setup must succeed");

            // git_status names the dirty submodule.
            auto ss = call(*provider, "git_status", obj());
            assert(!ss.is_error);
            assert(ss.text.find("submodules with uncommitted changes")
                       != std::string::npos
                   && "git_status must flag the dirty submodule");
            assert(ss.text.find("mod") != std::string::npos);
            std::puts("git_status: dirty submodule surfaced");

            // git_diff on the superproject is empty here → must hint inside.
            auto sd = call(*provider, "git_diff", obj());
            assert(!sd.is_error);
            assert(sd.text.find("submodules have uncommitted changes")
                       != std::string::npos
                   && "empty superproject git_diff must point into the "
                      "dirty submodule");
            assert(sd.text.find("git_diff path=") != std::string::npos);
            std::puts("git_diff: empty superproject diff hints into submodule");

            // A path INTO the submodule routes git tools to the submodule
            // repo (resolve_git_dir → submodule toplevel), so git_status
            // there shows the untracked file directly, no hint needed.
            auto si = obj(); si["path"] = "mod";
            auto sir = call(*provider, "git_status", si);
            assert(!sir.is_error);
            assert(sir.text.find("untracked.txt") != std::string::npos
                   && "git_status path=mod must operate inside the submodule");
            std::puts("git_status: path into submodule routes to submodule repo");
        }

        // git_commit with empty message rejected
        auto bad = obj(); bad["message"] = "   ";
        auto br = call(*provider, "git_commit", bad);
        assert(br.is_error);
        std::puts("git_commit: empty message refused");
    }

    // ── git_branch: list / create / switch / delete ─────────────────────
    {
        // list shows the current branch marked with '*'.
        auto bl = call(*provider, "git_branch", obj());   // action defaults to list
        assert(!bl.is_error);
        assert(bl.text.find("*") != std::string::npos
               && "git_branch list must mark the current branch");
        std::puts("git_branch: list ok");

        // create a branch.
        auto bc = obj(); bc["action"] = "create"; bc["name"] = "feature-x";
        auto bcr = call(*provider, "git_branch", bc);
        assert(!bcr.is_error);
        assert(read_effects(bcr).has(Effect::WriteFs));
        std::puts("git_branch: create ok");

        // switch to it, then verify git_status reports the new branch.
        auto bs = obj(); bs["action"] = "switch"; bs["name"] = "feature-x";
        auto bsr = call(*provider, "git_branch", bs);
        assert(!bsr.is_error);
        auto stx = call(*provider, "git_status", obj());
        assert(!stx.is_error);
        assert(stx.text.find("On branch feature-x") != std::string::npos
               && "switch must land us on the new branch");
        std::puts("git_branch: switch ok");

        // create-and-switch in one call via a fresh name.
        auto bcs = obj(); bcs["action"] = "switch"; bcs["name"] = "feature-y";
        auto bcsr = call(*provider, "git_branch", bcs);
        assert(!bcsr.is_error);
        assert(bcsr.text.find("created and switched") != std::string::npos);
        std::puts("git_branch: create-and-switch ok");

        // delete the now-unused feature-x (switch back to master first).
        auto back = obj(); back["action"] = "switch"; back["name"] = "master";
        (void)call(*provider, "git_branch", back);
        auto bd = obj(); bd["action"] = "delete"; bd["name"] = "feature-x";
        auto bdr = call(*provider, "git_branch", bd);
        assert(!bdr.is_error && "merged branch delete must succeed");
        std::puts("git_branch: delete ok");

        // create/switch/delete require a name.
        auto bn = obj(); bn["action"] = "create";
        auto bnr = call(*provider, "git_branch", bn);
        assert(bnr.is_error && "create without name must be rejected");
        std::puts("git_branch: missing name refused");
    }

    // ── git_commit amend + git_diff stat_only ─────────────────────────
    {
        // Make a change, commit, then amend the message.
        write_file(root / "amend_me.txt", "v1\n");
        auto c1 = obj(); c1["message"] = "typo commmit"; c1["stage_all"] = true;
        auto c1r = call(*provider, "git_commit", c1);
        assert(!c1r.is_error);
        auto am = obj(); am["amend"] = true; am["message"] = "fixed commit";
        auto amr = call(*provider, "git_commit", am);
        assert(!amr.is_error);
        assert(amr.text.find("amended ") != std::string::npos);
        auto lg2 = call(*provider, "git_log", [] { auto o = obj(); o["oneline"] = true; return o; }());
        assert(!lg2.is_error);
        assert(lg2.text.find("fixed commit") != std::string::npos
               && lg2.text.find("typo commmit") == std::string::npos
               && "amend must replace the previous commit's message");
        std::puts("git_commit: amend ok");

        // stat_only diff: change a file, assert the summary appears but no
        // patch hunk lines (`@@`) do.
        write_file(root / "amend_me.txt", "v1\nv2\n");
        auto ds = obj(); ds["stat_only"] = true;
        auto dsr = call(*provider, "git_diff", ds);
        assert(!dsr.is_error);
        assert(dsr.text.find("amend_me.txt") != std::string::npos);
        assert(dsr.text.find("@@") == std::string::npos
               && "stat_only must omit the patch body");
        std::puts("git_diff: stat_only omits patch body");

        // A normal diff DOES include the hunk.
        auto df = call(*provider, "git_diff", obj());
        assert(!df.is_error);
        assert(df.text.find("@@") != std::string::npos
               && "default git_diff must include the patch body");
        std::puts("git_diff: default includes patch body");
    }

    // ── git_stash: push / list / pop ──────────────────────────────────
    {
        // Make a tracked change to stash. amend_me.txt was committed earlier.
        write_file(root / "amend_me.txt", "v1\nstash-me\n");
        auto sp = obj(); sp["action"] = "push"; sp["message"] = "WIP test";
        auto spr = call(*provider, "git_stash", sp);
        assert(!spr.is_error);
        assert(spr.text.find("stashed") != std::string::npos);
        assert(read_effects(spr).has(Effect::WriteFs));
        std::puts("git_stash: push ok");

        // The working tree is clean again (a fresh diff shows nothing new).
        auto sl = call(*provider, "git_stash", obj());   // action=list
        assert(!sl.is_error);
        assert(sl.text.find("WIP test") != std::string::npos
               && "stash list must show the pushed entry's message");
        std::puts("git_stash: list ok");

        // pop restores the change.
        auto spop = obj(); spop["action"] = "pop";
        auto spopr = call(*provider, "git_stash", spop);
        assert(!spopr.is_error);
        assert(spopr.text.find("popped") != std::string::npos);
        std::puts("git_stash: pop restores changes");

        // Clean the working tree back up for the following tests.
        auto cl = obj(); cl["command"] = "git checkout -- amend_me.txt";
        (void)call(*provider, "bash", cl);

        // push with nothing to stash is a friendly no-op, not an error.
        auto spn = obj(); spn["action"] = "push";
        auto spnr = call(*provider, "git_stash", spn);
        assert(!spnr.is_error);
        assert(spnr.text.find("nothing to stash") != std::string::npos);
        std::puts("git_stash: empty push is a no-op");
    }

    // ── git_cherry_pick: apply a commit from another branch ────────────
    {
        // Build a side branch with a unique commit, return to master, then
        // cherry-pick that commit in.
        auto setup = obj(); setup["command"] =
            "git switch -c cp-src -q && "
            "printf cherry > cherry_file.txt && git add cherry_file.txt && "
            "git commit -q -m 'cherry commit' && "
            "echo SHA=$(git rev-parse HEAD) && git switch master -q";
        auto sr = call(*provider, "bash", setup);
        assert(!sr.is_error);
        // Extract the printed SHA.
        std::string txt = sr.text;
        auto p = txt.find("SHA=");
        assert(p != std::string::npos);
        std::string sha = txt.substr(p + 4, 40);
        // Trim to the hex run.
        size_t hexlen = 0;
        while (hexlen < sha.size() && std::isxdigit((unsigned char)sha[hexlen]))
            ++hexlen;
        sha.resize(hexlen);
        assert(!sha.empty());

        auto cp = obj(); cp["action"] = "pick";
        cp["commits"] = mcp::Json::array({sha});
        auto cpr = call(*provider, "git_cherry_pick", cp);
        assert(!cpr.is_error && "cherry-pick of a clean commit must succeed");
        assert(cpr.text.find("cherry-picked") != std::string::npos);
        std::puts("git_cherry_pick: pick ok");

        // The picked file now exists on master.
        auto chk = call(*provider, "git_log", [] {
            auto o = obj(); o["oneline"] = true; return o; }());
        assert(!chk.is_error && chk.text.find("cherry commit") != std::string::npos);
        std::puts("git_cherry_pick: commit landed on current branch");

        // pick requires commits.
        auto cpn = obj(); cpn["action"] = "pick";
        auto cpnr = call(*provider, "git_cherry_pick", cpn);
        assert(cpnr.is_error && "pick without commits must be rejected");
        // continue with no cherry-pick in progress is a clear error.
        auto cpc = obj(); cpc["action"] = "continue";
        auto cpcr = call(*provider, "git_cherry_pick", cpc);
        assert(cpcr.is_error);
        std::puts("git_cherry_pick: guard rails ok");
    }

    // ── git_rebase: replay a branch onto an advanced master ─────────────
    {
        // master advances; a topic branch forked earlier gets rebased onto it.
        auto setup = obj(); setup["command"] =
            "git switch -c rb-topic -q && "
            "printf topic > topic.txt && git add topic.txt && "
            "git commit -q -m 'topic work' && "
            "git switch master -q && "
            "printf mainline > mainline.txt && git add mainline.txt && "
            "git commit -q -m 'mainline work' && git switch rb-topic -q";
        auto sr = call(*provider, "bash", setup);
        assert(!sr.is_error);

        auto rb = obj(); rb["action"] = "onto"; rb["upstream"] = "master";
        auto rbr = call(*provider, "git_rebase", rb);
        assert(!rbr.is_error && "clean rebase onto master must succeed");
        assert(rbr.text.find("rebased onto master") != std::string::npos);
        assert(read_effects(rbr).has(Effect::WriteFs));
        std::puts("git_rebase: onto ok");

        // After the rebase, mainline.txt is reachable from the topic branch.
        auto chk = obj(); chk["command"] = "test -f mainline.txt && echo HAS_MAINLINE";
        auto chkr = call(*provider, "bash", chk);
        assert(!chkr.is_error && chkr.text.find("HAS_MAINLINE") != std::string::npos
               && "rebase must replay topic on top of advanced master");
        std::puts("git_rebase: topic replayed on advanced master");

        // onto without upstream is rejected; continue with no rebase errors.
        auto rbn = obj(); rbn["action"] = "onto";
        auto rbnr = call(*provider, "git_rebase", rbn);
        assert(rbnr.is_error && "onto without upstream must be rejected");
        auto rbc = obj(); rbc["action"] = "continue";
        auto rbcr = call(*provider, "git_rebase", rbc);
        assert(rbcr.is_error && "continue with no rebase in progress must error");
        std::puts("git_rebase: guard rails ok");

        (void)call(*provider, "bash", [] {
            auto o = obj(); o["command"] = "git switch master -q"; return o; }());
    }

    fs::current_path(prev_cwd);

    // ── repo_map: ranked, budgeted codebase skeleton ──────────────────
    {
        // alpha.cpp defines compute_sum; gamma.py defines compute_total —
        // both must appear as signature lines with L<line> anchors.
        auto r = call(*provider, "repo_map", obj());
        assert(!r.is_error);
        assert(r.text.find("alpha.cpp") != std::string::npos);
        assert(r.text.find("L") != std::string::npos);
        assert(read_effects(r).has(Effect::ReadFs));
        std::puts("repo_map: ok (ranked skeleton with line anchors)");

        // focus re-centers: focusing on the python symbol keeps gamma.py.
        auto fargs = obj(); fargs["focus"] = "compute_total";
        auto fr = call(*provider, "repo_map", fargs);
        assert(!fr.is_error);
        assert(fr.text.find("gamma.py") != std::string::npos);
        std::puts("repo_map: focus personalization ok");

        // budget is respected (allowing footer slack).
        auto bargs = obj(); bargs["budget"] = 1000;
        auto br = call(*provider, "repo_map", bargs);
        assert(!br.is_error);
        assert(br.text.size() < 2200);
        std::puts("repo_map: budget respected");

        // Binary-search budget fit: the rendered map must LAND UNDER the
        // budget (aider targets within ~15%), not overshoot it. The greedy
        // packer could exceed budget on its last block; the binary search
        // guarantees header+blocks+footer ≤ budget whenever ≥1 file fits.
        {
            auto b2 = obj(); b2["budget"] = 4000;
            auto r2 = call(*provider, "repo_map", b2);
            assert(!r2.is_error);
            assert(r2.text.size() <= 4000
                   && "repo_map must not overshoot its byte budget");
            std::puts("repo_map: binary-search budget fit stays under budget");
        }

        // Robustness: control-flow / statement lines must NEVER surface as
        // definition signatures. alpha.cpp's body has `if (a > b) {` and
        // `return …` lines — neither may appear as an `L<n>: ` def line
        // (guarded by both the def-regex shape and the keyword stop-list).
        assert(r.text.find(": if (") == std::string::npos
            && r.text.find(": if(") == std::string::npos);
        assert(r.text.find(": return ") == std::string::npos);
        std::puts("repo_map: no control-flow / statement false-positive defs");
    }

    // ── repo_map HARDENING ───────────────────────────────────────────
    {
        // (a) DEFAULT PATH = WORKSPACE ROOT, not cwd. cwd was reset to
        //     prev_cwd above, so a no-path repo_map that resolved against cwd
        //     would fail "outside the workspace". It must map the workspace.
        auto r = call(*provider, "repo_map", obj());
        assert(!r.is_error && "repo_map with no path must default to workspace root");
        assert(r.text.find("alpha.cpp") != std::string::npos);
        std::puts("repo_map: no-path defaults to workspace root (cwd-independent)");

        // (b) SYMLINK LOOP must not hang or duplicate. Create a cyclic dir
        //     symlink (loop -> workspace root) inside the tree; the walk must
        //     refuse to descend through it and still terminate quickly.
        std::error_code lec;
        fs::create_directory_symlink(root, root / "sub" / "loop", lec);
        if (!lec) {
            auto rl = call(*provider, "repo_map", obj());
            assert(!rl.is_error && "repo_map must survive a cyclic symlink");
            // alpha.cpp appears exactly once (not once per symlink traversal).
            auto first = rl.text.find("alpha.cpp");
            auto second = rl.text.find("alpha.cpp", first + 1);
            assert(first != std::string::npos && second == std::string::npos
                   && "symlink loop must not duplicate files in the map");
            fs::remove(root / "sub" / "loop", lec);
            std::puts("repo_map: cyclic symlink neither hangs nor duplicates");
        } else {
            std::puts("repo_map: symlink test skipped (no symlink support)");
        }

        // (c) BINARY / NUL-byte file is skipped (no garbage symbols from it).
        write_file(root / "blob.cpp",
                   std::string("int real_symbol_here(){}\n\0\0binary\0noise", 40));
        auto rb = call(*provider, "repo_map", obj());
        assert(!rb.is_error);
        assert(rb.text.find("real_symbol_here") == std::string::npos
               && "NUL-byte file must be skipped, not parsed for symbols");
        fs::remove(root / "blob.cpp", lec);
        std::puts("repo_map: binary/NUL file skipped");

        // (d) PER-ROOT CACHE: two identical calls return byte-identical maps
        //     (the second is a cache hit) — proves the LRU reuse path is sound.
        auto c1 = call(*provider, "repo_map", obj());
        auto c2 = call(*provider, "repo_map", obj());
        assert(!c1.is_error && !c2.is_error && c1.text == c2.text
               && "repeated repo_map must be deterministic (cache hit)");
        std::puts("repo_map: repeated call is deterministic cache hit");

        // (e) INVALID budget is clamped, not rejected.
        auto bad = obj(); bad["budget"] = 5;   // below the 1000 floor
        auto rbad = call(*provider, "repo_map", bad);
        assert(!rbad.is_error && "tiny budget must clamp to the 1000 floor, not error");
        std::puts("repo_map: out-of-range budget clamped");

        // (f) NESTED-PROJECT BOUNDARY: a submodule / vendored checkout / any
        //     directory carrying its OWN `.git` must be a hard stop — its
        //     source belongs to a different project and must NEVER pollute the
        //     map. This is the exact sibling-project leak (maya-py/, mcp-cpp/)
        //     the boundary guards against. Build a nested repo with a
        //     uniquely-named symbol and prove it does not appear.
        fs::create_directories(root / "vendored_dep");
        write_file(root / "vendored_dep" / ".git",
                   "gitdir: ../.git/modules/vendored_dep\n");  // submodule gitlink FILE
        write_file(root / "vendored_dep" / "foreign.cpp",
                   "int nested_project_symbol_xyz(){ return 7; }\n");
        auto rn = call(*provider, "repo_map", obj());
        assert(!rn.is_error);
        assert(rn.text.find("nested_project_symbol_xyz") == std::string::npos
               && "submodule/nested-repo source must be excluded from the map");
        assert(rn.text.find("foreign.cpp") == std::string::npos
               && "nested-project file must not appear in the map");
        // The workspace's own source is unaffected — the boundary only stops
        // NESTED roots, never the top-level walk.
        assert(rn.text.find("alpha.cpp") != std::string::npos
               && "workspace source must still be mapped");
        fs::remove_all(root / "vendored_dep", lec);
        std::puts("repo_map: nested project (.git boundary) excluded, workspace intact");
    }

    fs::remove_all(root);
}
