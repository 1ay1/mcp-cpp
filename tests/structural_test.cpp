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

    // ── 1. malloc($SIZE) matches the single-NODE-arg calls (L2, L3), and
    //       neither the comment nor the string that also contain "malloc(".
    //       Per spacegrep/ast-grep semantics a single $X binds exactly ONE
    //       node (one atom or one balanced group), so the multi-node arg on
    //       L8 (64 * sz) is NOT a $SIZE match — use $$$ for that. ────────────
    {
        auto args = sobj();
        args["pattern"] = "malloc($SIZE)";
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L2") != std::string::npos);   // malloc(n)
        assert(r.text.find("> L3") != std::string::npos);   // malloc(count)
        assert(r.text.find("> L8") == std::string::npos);   // 64 * sz is 3 nodes
        // The comment (L4) and the string (L5) must never be MATCHED.
        assert(r.text.find("> L4") == std::string::npos);
        assert(r.text.find("> L5") == std::string::npos);
        assert(read_effects(r).has(Effect::ReadFs));
        std::puts("structural: malloc($SIZE) binds one node, skips comments/strings ok");
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

    // ── 6a2. expand uses the TREE for the enclosing scope, so a `}` inside a
    //      string literal can't mis-balance it — the whole function returns.
    {
        swrite(root / "hardexp.c",
            "int f() {\n"                                 // L1
            "    log(\"has a } brace in string\");\n"      // L2 fake close-brace
            "    int x = compute(1);\n"                    // L3 <-- hit
            "    return x;\n"                              // L4 must be included
            "}\n");                                       // L5
        auto args = sobj();
        args["pattern"] = "compute($$$)";
        args["path"]    = root.string();
        args["glob"]    = "hardexp.c";
        args["expand"]  = true;
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L3") != std::string::npos);        // the match
        assert(r.text.find("int f() {") != std::string::npos);   // scope start
        assert(r.text.find("return x;") != std::string::npos);   // full body, past the fake brace
        std::puts("structural: tree-based expand ignores brace-in-string ok");
    }

    // ── 6a3. The tokenizer understands C++ raw strings R"(…)", so a quote or
    //      brace inside one can't corrupt tokenization or the tree. ───────
    {
        swrite(root / "rawtok.cpp",
            "int f() {\n"                                 // L1
            "    auto re = R\"(a \" and } trap)\";\n"      // L2 quote+brace in raw string
            "    int found = 9;\n"                         // L3 <-- hit
            "    return found;\n"                          // L4
            "}\n");                                       // L5
        auto args = sobj();
        args["pattern"] = "found = $X";
        args["path"]    = root.string();
        args["glob"]    = "rawtok.cpp";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L3") != std::string::npos);   // matched past the raw-string trap
        std::puts("structural: tokenizer parses C++ raw strings ok");
    }

    // ── 6b. Nested-document model: a metavar binds a whole GROUP, and the
    //     matcher recurses into nested calls so an inner call still matches.
    {
        swrite(root / "nest.c",
            "int p() { return outer(inner(x, y), z); }\n"   // L1: nested calls
            "int q() { log(\"inner(a, b)\"); return 0; }\n"); // L2: call only in a string
        // inner($$$) must find the nested inner(x,y) on L1 (recursion into the
        // outer(...) group) and NOT the one inside the L2 string literal.
        auto args = sobj();
        args["pattern"] = "inner($$$)";
        args["path"]    = root.string();
        args["glob"]    = "nest.c";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L1") != std::string::npos);   // nested call matched
        assert(r.text.find("> L2") == std::string::npos);   // string literal ignored
        std::puts("structural: nested-group recursion matches inner call ok");
    }

    // ── 6c. One-node vs multi-node metavar distinction (ast-grep semantics).
    //     A single $C binds ONE node; a multi-node condition needs $$$C.
    {
        swrite(root / "cond.c",
            "int a() { if (ready) return 1; return 0; }\n"     // L1: 1-node cond
            "int b() { if (!ready) return 1; return 0; }\n");  // L2: 2-node cond
        {
            auto args = sobj();
            args["pattern"] = "if ($C) return 1;";
            args["path"] = root.string(); args["glob"] = "cond.c";
            auto r = scall(*provider, "search_structural", args);
            assert(!r.is_error);
            assert(r.text.find("> L1") != std::string::npos);   // (ready) = 1 node
            assert(r.text.find("> L2") == std::string::npos);   // (!ready) = 2 nodes
        }
        {
            auto args = sobj();
            args["pattern"] = "if ($$$C) return 1;";
            args["path"] = root.string(); args["glob"] = "cond.c";
            auto r = scall(*provider, "search_structural", args);
            assert(!r.is_error);
            assert(r.text.find("> L1") != std::string::npos);   // both match now
            assert(r.text.find("> L2") != std::string::npos);
        }
        std::puts("structural: $C (one node) vs $$$C (many) distinction ok");
    }

    // ── 7. No structural match → clean not-found (not an error) ───────
    {
        auto args = sobj();
        args["pattern"] = "free($PTR)";   // no free() calls in the tree
        args["path"]    = root.string();
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("no structural match") != std::string::npos);
        std::puts("structural: no-match reported cleanly ok");
    }

    // ── 8. Rust semantics: lifetimes are NOT string-opens; r#"…"# raw strings
    //      and nested block comments don't corrupt bracket balance ───────
    {
        swrite(root / "lib.rs",
            "fn get<'a>(m: &'a Map) -> &'a str {\n"        // L1 lifetimes galore
            "    let re = Regex::new(r#\"fn \\w+\\(\"#);\n" // L2 raw str w/ unbalanced (
            "    /* outer /* nested */ still comment: target(1) */\n" // L3 nested cmt
            "    target(seed)\n"                            // L4 <-- the real hit
            "}\n");
        auto args = sobj();
        args["pattern"] = "target($X)";
        args["path"]    = root.string();
        args["glob"]    = "*.rs";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L4") != std::string::npos);   // real call matched
        assert(r.text.find("> L3") == std::string::npos);   // nested comment skipped
        std::puts("structural: rust lifetimes + r#raw + nested comments ok");
    }

    // ── 8b. Rust: '\'' char literals still lex as chars, and a lifetime
    //      before a brace doesn't swallow the block — expand still finds it ─
    {
        swrite(root / "ch.rs",
            "fn f() -> char {\n"                            // L1
            "    let c: char = '{';\n"                       // L2 brace-in-char
            "    victim(c)\n"                                // L3 <-- hit
            "}\n");                                         // L4
        auto args = sobj();
        args["pattern"] = "victim($X)";
        args["path"]    = root.string();
        args["glob"]    = "*.rs";
        args["expand"]  = true;
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L3") != std::string::npos);
        assert(r.text.find("fn f") != std::string::npos);   // expand got whole fn
        std::puts("structural: rust char-literal brace + expand ok");
    }

    // ── 9. Go: backtick strings are RAW — a trailing backslash inside one
    //      must not swallow the closing backtick (and braces inside are text) ─
    {
        swrite(root / "main.go",
            "func f() string {\n"                             // L1
            "    p := `C:\\dir\\` // raw: ends at 2nd backtick\n" // L2 trap
            "    q := `has a } brace`\n"                       // L3 trap
            "    return mark(p, q)\n"                          // L4 <-- hit
            "}\n");
        auto args = sobj();
        args["pattern"] = "mark($$$)";
        args["path"]    = root.string();
        args["glob"]    = "*.go";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L4") != std::string::npos);
        std::puts("structural: go raw backtick strings ok");
    }

    // ── 10. JS: template literal with ${…} interpolation and an escaped
    //      backtick — the string is one token; code around it still matches ─
    {
        swrite(root / "tpl.js",
            "function t(x) {\n"                                     // L1
            "    const s = `a ${x + 1} b \\` c { unbalanced`;\n"     // L2 trap
            "    return probe(s);\n"                                 // L3 <-- hit
            "}\n");
        auto args = sobj();
        args["pattern"] = "probe($X)";
        args["path"]    = root.string();
        args["glob"]    = "*.js";
        auto r = scall(*provider, "search_structural", args);
        assert(!r.is_error);
        assert(r.text.find("> L3") != std::string::npos);
        std::puts("structural: js template literal w/ escape + brace ok");
    }

    fs::current_path(prev_cwd);
    std::error_code ec;
    fs::remove_all(root, ec);
}
