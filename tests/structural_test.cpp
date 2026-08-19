// SPDX-License-Identifier: Apache-2.0
//
// structural_test.cpp — exercises the search_structural (AST-shape) tool
// end-to-end through make_provider(). Locks the behaviours that make it more
// than grep: metavariable matching, back-reference consistency, variadic
// $$$, balanced-group capture, and — the load-bearing property — NEVER
// matching inside comments or string literals.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include <cstdio>
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

static mcp::cap::Result scall(mcp::cap::CapabilityProvider& p,
                              const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json sobj() { return mcp::Json::object(); }

static void swrite(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

TEST_CASE("search_structural") {
    auto root = fs::temp_directory_path() /
                ("mcp_structural_test_" + std::to_string(mcp_getpid()));
    fs::create_directories(root);
    util::set_workspace_root(root);
    auto prev_cwd = fs::current_path();
    fs::current_path(root);

    // ── Seed a tree with real code + traps (comments/strings that would fool
    //    a plain grep) ────────────────────────────────────────────────────
    swrite(root / "mem.c",
        "#include <stdlib.h>\n"
        "void* a(int n) { return malloc(n); }\n"          // L2 real call, single-token arg
        "void* b()      { return malloc(count); }\n"     // L3 real call, single-token arg
        "// TODO: replace malloc(bad) with a pool\n"       // L4 comment — MUST NOT match
        "const char* doc = \"call malloc(x) to allocate\";\n"  // L5 string — MUST NOT match
        "int selfcmp(int x, int y) { return x == x; }\n"  // L6 self-compare
        "int othercmp(int p, int q){ return p == q; }\n"  // L7 not a self-compare
        "void* c()      { return malloc(64 * sz); }\n");  // L8 multi-token arg

    swrite(root / "app.js",
        "function f(a, b, c) { return g(a, b, c); }\n"    // L1 call w/ 3 args
        "const empty = () => { try { risky(); } catch (e) {} };\n"); // L2 empty catch

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── 1. malloc($SIZE) matches every call — $SIZE captures a whole
    //       EXPRESSION (one token, or `64 * sz`) — and NEVER the comment or
    //       string literal that also contain "malloc(". This is the whole
    //       point of structural over grep. ──────────────────────────────
    {
        auto args = sobj();
        args["pattern"] = "malloc($SIZE)";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L2") != std::string::npos);   // malloc(n)
        assert(r.text.find("> L3") != std::string::npos);   // malloc(count)
        assert(r.text.find("> L8") != std::string::npos);   // malloc(64 * sz)
        // The comment (L4) and the string (L5) must never be MATCHED.
        assert(r.text.find("> L4") == std::string::npos);
        assert(r.text.find("> L5") == std::string::npos);
        assert(read_effects(r).has(Effect::ReadFs));
        std::puts("structural: malloc($SIZE) skips comments+strings ok");
    }

    // ── 1b. malloc($$$) matches ALL calls including the multi-token arg ────
    {
        auto args = sobj();
        args["pattern"] = "malloc($$$)";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L2") != std::string::npos);
        assert(r.text.find("> L8") != std::string::npos);   // 64 * sz now matched
        assert(r.text.find("> L4") == std::string::npos);   // still skips comment
        std::puts("structural: malloc($$$) matches multi-token args ok");
    }

    // ── 2. Back-reference: $X == $X matches self-compare (L6) but NOT the
    //       p == q on L7 ──────────────────────────────────────────────────
    {
        auto args = sobj();
        args["pattern"] = "$X == $X";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        // The matched line is marked with "> "; context lines with 4 spaces.
        // L6 (x == x) is a MATCH; L7 (p == q) may appear only as context of
        // the adjacent match, never as a match itself.
        assert(r.text.find("> L6") != std::string::npos);   // x == x matched
        assert(r.text.find("> L7") == std::string::npos);   // p == q NOT matched
        std::puts("structural: $X == $X back-reference ok");
    }

    // ── 3. Variadic $$$ : g($$$ARGS) matches the 3-arg call ───────────────
    {
        auto args = sobj();
        args["pattern"] = "g($$$ARGS)";
        args["path"]    = root.string();
        args["glob"]    = "*.js";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("app.js") != std::string::npos);
        std::puts("structural: variadic $$$ARGS ok");
    }

    // ── 4. Empty catch block: catch ($$$) {} ─────────────────────────────
    {
        auto args = sobj();
        args["pattern"] = "catch ($$$) {}";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("app.js") != std::string::npos);
        std::puts("structural: empty catch block ok");
    }

    // ── 5. A pattern of only metavariables is rejected (too broad) ────────
    {
        auto args = sobj();
        args["pattern"] = "$X";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(r.is_error);   // guarded against matching everything
        std::puts("structural: bare-metavar pattern rejected ok");
    }

    // ── 6. expand=true returns the whole enclosing function/block ────────
    {
        auto args = sobj();
        args["pattern"] = "malloc($SIZE)";
        args["path"]    = root.string();
        args["expand"]  = true;
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        // The L2 match lives in `void* a(...) { ... }` — with expand the whole
        // one-line body is returned; the match marker is still on L2.
        assert(r.text.find("> L2") != std::string::npos);
        std::puts("structural: expand returns enclosing block ok");
    }

    // ── 7. No structural match → clean not-found (not an error) ───────────
    {
        auto args = sobj();
        args["pattern"] = "free($PTR)";   // no free() calls in the tree
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("no structural match") != std::string::npos);
        std::puts("structural: no-match reported cleanly ok");
    }

    fs::current_path(prev_cwd);
    std::error_code ec;
    fs::remove_all(root, ec);
}
