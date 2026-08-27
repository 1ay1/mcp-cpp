// SPDX-License-Identifier: Apache-2.0
//
// apply_patch_test.cpp — the apply_patch tool: unified-diff application with
// fuzzy hunk location, driven through make_provider(). Pins the contract:
// clean multi-hunk apply, fuzzy apply after the file drifts, ATOMICITY (a bad
// hunk writes nothing), ambiguity rejection, and the FileChange carry.

#include <chrono>
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define mcp_getpid _getpid
#else
#  include <unistd.h>
#  define mcp_getpid getpid
#endif

using namespace mcp::tools;
namespace fs = std::filesystem;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }
static void wr(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}
static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

TEST_CASE("apply_patch") {
    auto root = fs::temp_directory_path() / ("mcp_patch_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    util::set_workspace_root(root);
    auto prev = fs::current_path();
    fs::current_path(root);

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    const std::string base =
        "line one\n"
        "line two\n"
        "line three\n"
        "line four\n"
        "line five\n";

    // ── clean single-hunk apply + FileChange carried ─────────────────────
    {
        wr(root / "a.txt", base);
        auto a = obj();
        a["path"] = (root / "a.txt").string();
        a["patch"] =
            "@@ -1,3 +1,3 @@\n"
            " line one\n"
            "-line two\n"
            "+line TWO changed\n"
            " line three\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "clean patch applies");
        check(has(rd(root / "a.txt"), "line TWO changed"), "removed→added line landed");
        check(has(rd(root / "a.txt"), "line one") &&
              has(rd(root / "a.txt"), "line five"), "context untouched");
        check(read_effects(r).has(Effect::WriteFs), "apply_patch is WriteFs");
        auto ch = read_change(r);
        check(ch.has_value(), "carries a FileChange for the diff-review UI");
        check(ch && ch->added >= 1 && ch->removed >= 1, "FileChange has add/remove counts");
    }

    // ── multi-hunk apply, both hunks land ────────────────────────────────
    {
        wr(root / "b.txt", base);
        auto a = obj();
        a["path"] = (root / "b.txt").string();
        a["patch"] =
            "@@ -1,2 +1,2 @@\n"
            " line one\n"
            "-line two\n"
            "+HUNK1\n"
            "@@ -4,2 +4,2 @@\n"
            " line four\n"
            "-line five\n"
            "+HUNK2\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "multi-hunk patch applies");
        auto out = rd(root / "b.txt");
        check(has(out, "HUNK1") && has(out, "HUNK2"), "both hunks landed");
        check(!has(out, "line two") && !has(out, "line five"), "both removals took");
    }

    // ── fuzzy: file DRIFTED (extra leading lines shift line numbers) ─────
    {
        wr(root / "c.txt",
           "// a new header comment\n"
           "// another new line\n"
           + base);   // base now starts at line 3, but @@ says line 1
        auto a = obj();
        a["path"] = (root / "c.txt").string();
        a["patch"] =
            "@@ -3,3 +3,3 @@\n"
            " line two\n"
            "-line three\n"
            "+line THREE\n"
            " line four\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "patch applies despite drifted line numbers");
        check(has(rd(root / "c.txt"), "line THREE"),
              "fuzzy located the hunk past the drift");
        check(has(rd(root / "c.txt"), "// a new header comment"),
              "the drifted-in lines are preserved");
    }

    // ── ATOMICITY: one good hunk + one bad hunk → NOTHING written ────────
    {
        wr(root / "d.txt", base);
        auto before = rd(root / "d.txt");
        auto a = obj();
        a["path"] = (root / "d.txt").string();
        a["patch"] =
            "@@ -1,2 +1,2 @@\n"
            " line one\n"
            "-line two\n"
            "+GOOD\n"
            "@@ -4,2 +4,2 @@\n"
            " line four\n"
            "-this line does not exist anywhere\n"   // bad hunk
            "+NOPE\n";
        auto r = call(*provider, "apply_patch", a);
        check(r.is_error, "a patch with an unmatchable hunk is an error");
        check(rd(root / "d.txt") == before,
              "ATOMIC: the good hunk was NOT written when a sibling hunk failed");
        check(!has(rd(root / "d.txt"), "GOOD"), "no partial application");
    }

    // ── no-op patch: already in the target state ─────────────────────────
    {
        wr(root / "e.txt", base);
        auto a = obj();
        a["path"] = (root / "e.txt").string();
        // A hunk whose - and + sides are identical context → no change.
        a["patch"] =
            "@@ -1,3 +1,3 @@\n"
            " line one\n"
            " line two\n"
            " line three\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "no-op patch is not an error");
        check(has(r.text, "no change"), "no-op is reported as such");
    }

    // ── error paths ──────────────────────────────────────────────────────
    {
        auto a = obj();               // missing patch
        a["path"] = (root / "a.txt").string();
        check(call(*provider, "apply_patch", a).is_error, "missing patch is an error");

        auto b = obj();               // no @@ hunks
        b["path"] = (root / "a.txt").string();
        b["patch"] = "this is not a diff at all\n";
        check(call(*provider, "apply_patch", b).is_error, "a patch with no hunks is an error");

        auto c = obj();               // nonexistent file
        c["path"] = (root / "nope.txt").string();
        c["patch"] = "@@ -1,1 +1,1 @@\n-x\n+y\n";
        check(call(*provider, "apply_patch", c).is_error, "patching a missing file is an error");

        auto d = obj();               // pure insertion (no context) → rejected
        wr(root / "f.txt", base);
        d["path"] = (root / "f.txt").string();
        d["patch"] = "@@ -0,0 +1,1 @@\n+inserted with no context\n";
        check(call(*provider, "apply_patch", d).is_error,
              "a context-less pure insertion is rejected (can't place it)");
    }

    // ── DoS BOUND: hunk count is capped. Each hunk runs a full fuzzy_find
    //    over the file; the per-hunk cost is bounded but the COUNT was not, so
    //    a patch with thousands of hunks turned a bounded search into a
    //    minutes-long apply. A patch far over the cap must be REJECTED fast,
    //    not ground through. (Well over any real edit to a small file.)
    {
        wr(root / "many.txt", base);
        std::string patch;
        for (int i = 0; i < 5000; ++i)
            patch += "@@ -1,1 +1,1 @@\n-line one\n+changed\n";
        auto a = obj();
        a["path"]  = (root / "many.txt").string();
        a["patch"] = patch;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = call(*provider, "apply_patch", a);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(r.is_error, "a patch with thousands of hunks is rejected");
        check(has(r.text, "hunks") || has(r.text, "cap"),
              "rejection names the hunk cap");
        check(ms < 1'000, "over-cap patch is rejected fast, not ground through");
        check(rd(root / "many.txt") == base, "ATOMIC: nothing written on reject");
    }

    fs::current_path(prev);
    fs::remove_all(root);
}
