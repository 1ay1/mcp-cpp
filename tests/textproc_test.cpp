// SPDX-License-Identifier: Apache-2.0
//
// textproc_test.cpp — the transform/aggregate layer (extract / aggregate /
// replace / read_filter) driven end-to-end through make_provider(), proving
// each tool's core contract and that the workspace boundary + effects flow.

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
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST_CASE("textproc") {
    auto root = fs::temp_directory_path() / ("mcp_textproc_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    util::set_workspace_root(root);
    auto prev_cwd = fs::current_path();
    fs::current_path(root);

    wr(root / "src" / "a.ts",
       "import { Foo } from './foo';\n"
       "import { Bar } from './bar';\n"
       "// TODO(alice): wire it\n"
       "const x = 1;\n");
    wr(root / "src" / "b.ts",
       "import { Foo } from './foo';\n"       // Foo again → dup
       "import { Baz } from './baz';\n"
       "// TODO(bob): later\n"
       "// TODO(alice): and this\n");
    wr(root / "nums.txt", "score 10\nscore 25\nscore 5\n");

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── extract: capture group 1 = import identifier ─────────────────────
    {
        auto a = obj();
        a["pattern"] = "import \\{ (\\w+) \\}";
        a["group"] = 1;
        a["path"] = root.string();
        a["glob"] = "*.ts";
        auto r = call(*provider, "extract", a);
        check(!r.is_error, "extract runs");
        check(has(r.text, "Foo") && has(r.text, "Bar") &&
              has(r.text, "Baz") && has(r.text, "Baz"),
              "extract pulls each capture-group value");
        check(read_effects(r).has(Effect::ReadFs), "extract is ReadFs");
    }

    // ── extract count: Foo appears twice, sorted desc ────────────────────
    {
        auto a = obj();
        a["pattern"] = "import \\{ (\\w+) \\}";
        a["group"] = 1; a["count"] = true;
        a["path"] = root.string(); a["glob"] = "*.ts";
        auto r = call(*provider, "extract", a);
        check(!r.is_error, "extract count runs");
        // Foo (2) should appear before the singletons in the output.
        auto pfoo = r.text.find("Foo");
        auto pbar = r.text.find("Bar");
        check(pfoo != std::string::npos && pbar != std::string::npos && pfoo < pbar,
              "count sorts the 2x value ahead of 1x values");
        check(has(r.text, "2\tFoo"), "count shows Foo\u21922");
    }

    // ── extract field/awk mode: column 2 of whitespace-split ─────────────
    {
        auto a = obj();
        a["pattern"] = "score";
        a["column"] = 2;                 // the number after "score"
        a["path"] = root.string(); a["glob"] = "nums.txt";
        auto r = call(*provider, "extract", a);
        check(!r.is_error, "extract field mode runs");
        check(has(r.text, "10") && has(r.text, "25") && has(r.text, "5"),
              "field mode emits column 2 of each matching line");
    }

    // ── extract response_format: detailed tags file:line, concise doesn't ──
    {
        auto a = obj();
        a["pattern"] = "import \\{ (\\w+) \\}";
        a["group"] = 1; a["response_format"] = "detailed";
        a["path"] = root.string(); a["glob"] = "*.ts";
        auto r = call(*provider, "extract", a);
        check(!r.is_error, "extract detailed runs");
        check(has(r.text, "a.ts:") || has(r.text, "src/a.ts:"),
              "response_format=detailed tags each value with file:line");
        // concise (default) omits the path.
        auto b = obj();
        b["pattern"] = "import \\{ (\\w+) \\}"; b["group"] = 1;
        b["path"] = root.string(); b["glob"] = "*.ts";
        auto r2 = call(*provider, "extract", b);
        check(!has(r2.text, ".ts:"), "concise (default) omits file:line");
    }

    // ── aggregate by=capture on the TODO owner ───────────────────────────
    {
        auto a = obj();
        a["pattern"] = "TODO\\((\\w+)\\)";
        a["by"] = "capture"; a["group"] = 1;
        a["path"] = root.string(); a["glob"] = "*.ts";
        auto r = call(*provider, "aggregate", a);
        check(!r.is_error, "aggregate runs");
        // alice has 2 TODOs, bob has 1 → alice ranked first.
        check(has(r.text, "2\talice"), "aggregate counts alice=2");
        check(has(r.text, "1\tbob"), "aggregate counts bob=1");
        auto pa = r.text.find("alice"), pb = r.text.find("bob");
        check(pa < pb, "aggregate sorts the bigger group first");
    }

    // ── aggregate by=file: which files import Foo ────────────────────────
    {
        auto a = obj();
        a["pattern"] = "import"; a["by"] = "file";
        a["path"] = root.string(); a["glob"] = "*.ts";
        auto r = call(*provider, "aggregate", a);
        check(!r.is_error, "aggregate by=file runs");
        check(has(r.text, "a.ts") && has(r.text, "b.ts"),
              "aggregate by=file lists both files");
    }

    // ── aggregate op=sum over the score numbers ──────────────────────────
    {
        auto a = obj();
        a["pattern"] = "\\d+"; a["by"] = "match"; a["op"] = "sum";
        a["path"] = root.string(); a["glob"] = "nums.txt";
        auto r = call(*provider, "aggregate", a);
        check(!r.is_error, "aggregate sum runs");
        // by=match groups distinct numbers; each sums to itself. Just prove
        // the numbers show up (10, 25, 5).
        check(has(r.text, "10") && has(r.text, "25"), "sum surfaces the values");
    }

    // ── replace DRY RUN: no write, preview shows the swap ────────────────
    {
        auto a = obj();
        a["find"] = "Foo"; a["replacement"] = "Qux";
        a["path"] = root.string(); a["glob"] = "*.ts";
        auto r = call(*provider, "replace", a);
        check(!r.is_error, "replace dry-run runs");
        check(has(r.text, "DRY RUN"), "dry run is labelled");
        check(has(r.text, "Qux"), "preview shows the replacement");
        // File on disk MUST be untouched.
        check(has(rd(root / "src" / "a.ts"), "Foo"),
              "dry run does NOT modify the file");
        check(!has(rd(root / "src" / "a.ts"), "Qux"), "no Qux on disk yet");
    }

    // ── replace apply: writes the swap ───────────────────────────────────
    {
        auto a = obj();
        a["find"] = "Foo"; a["replacement"] = "Qux";
        a["path"] = root.string(); a["glob"] = "*.ts";
        a["apply"] = true;
        auto r = call(*provider, "replace", a);
        check(!r.is_error, "replace apply runs");
        check(read_effects(r).has(Effect::WriteFs), "apply is WriteFs");
        check(has(rd(root / "src" / "a.ts"), "Qux") &&
              !has(rd(root / "src" / "a.ts"), "Foo"),
              "apply rewrites Foo→Qux on disk");
        check(has(rd(root / "src" / "b.ts"), "Qux"),
              "apply hits every matching file");
        // Multi-file diff-review feed: one FileChange per written file, each
        // with before/after (so the host can rebuild hunks + queue for review).
        auto changes = mcp::tools::read_changes(r);
        check(changes.size() == 2, "apply emits one change per written file");
        bool ca=false, cb=false;
        for (auto& ch : changes) {
            if (ch.path.find("a.ts") != std::string::npos) ca = true;
            if (ch.path.find("b.ts") != std::string::npos) cb = true;
            check(has(ch.after, "Qux") && has(ch.before, "Foo"),
                  "each change carries before/after");
        }
        check(ca && cb, "both edited files are in the change set");
    }

    // ── read_filter: only TODO lines + context, gaps collapsed ───────────
    {
        auto a = obj();
        a["path"] = (root / "src" / "b.ts").string();
        a["pattern"] = "TODO";
        a["context"] = 0;
        auto r = call(*provider, "read_filter", a);
        check(!r.is_error, "read_filter runs");
        check(has(r.text, "TODO(bob)") && has(r.text, "TODO(alice)"),
              "read_filter keeps matching lines");
        check(has(r.text, "\u22ef"), "read_filter collapses non-matching gaps");
        check(!has(r.text, "import { Qux }"),
              "read_filter drops non-matching lines");
    }

    // ── read_filter invert: keep NON-matching (grep -v) ──────────────────
    {
        auto a = obj();
        a["path"] = (root / "src" / "b.ts").string();
        a["pattern"] = "TODO"; a["invert"] = true; a["context"] = 0;
        auto r = call(*provider, "read_filter", a);
        check(!r.is_error, "read_filter invert runs");
        check(has(r.text, "import"), "invert keeps the import lines");
        check(!has(r.text, "TODO(bob)"), "invert drops the TODO lines");
    }

    // ── error paths ──────────────────────────────────────────────────────
    {
        auto a = obj(); a["path"] = root.string();  // no pattern
        auto r = call(*provider, "extract", a);
        check(r.is_error, "extract without pattern is an error");

        auto b = obj();
        b["pattern"] = "["; b["path"] = root.string();  // bad regex
        auto r2 = call(*provider, "extract", b);
        check(r2.is_error, "extract with a broken regex is an error");
    }

    // ── ReDoS GUARD: a nested unbounded quantifier is rejected at compile
    //    time. std::regex is a backtracking engine with no step limit or
    //    interrupt, so `(a+)+$` vs a run of 'a' backtracks EXPONENTIALLY
    //    (~30 s at 28 chars) and the tool's catch(...) never fires — it hangs.
    //    compile_pattern must refuse the pattern up front, FAST, for extract,
    //    aggregate, and regex replace. We seed a file of 'a' to make the
    //    catastrophic case reachable if the guard ever regresses.
    {
        wr(root / "aaa.txt", std::string(40, 'a'));
        for (const char* danger : {"(a+)+$", "(a*)*", "(.+)*x", "(\\d+)+"}) {
            for (const char* tool : {"extract", "aggregate"}) {
                auto a = obj();
                a["pattern"] = danger;
                a["path"] = root.string(); a["glob"] = "aaa.txt";
                if (std::string(tool) == "aggregate") a["by"] = "match";
                const auto t0 = std::chrono::steady_clock::now();
                auto r = call(*provider, tool, a);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                check(r.is_error, std::string(tool) + ": nested-quantifier regex rejected");
                check(ms < 500, std::string(tool) + ": rejected fast (no backtracking)");
            }
        }
        // A legitimate quantified group must STILL work.
        auto ok = obj();
        ok["pattern"] = "(a)+"; ok["path"] = root.string(); ok["glob"] = "aaa.txt";
        auto r = call(*provider, "extract", ok);
        check(!r.is_error, "a safe quantified group (a)+ still compiles + runs");
    }

    fs::current_path(prev_cwd);
    fs::remove_all(root);
}
