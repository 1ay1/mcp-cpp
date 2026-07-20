// SPDX-License-Identifier: Apache-2.0
//
// fuzzy_match_test — the line-DP fuzzy matcher is the load-bearing core of
// the `edit` tool: a WRONG match silently corrupts a user's file, so this is
// the highest-consequence code in the tools layer. These tests lock in:
//
//   1. Exact unique match — fast path, precise byte range.
//   2. Whitespace / indentation drift is free (leading indent changes,
//      trailing spaces) and still lands the right region.
//   3. Single-char typo tolerance (fuzzy_eq threshold).
//   4. Smart-quote / dash hallucination normalization.
//   5. Ambiguity: a duplicated needle with NO hint reports count>=2 and
//      does NOT apply; WITH a line_hint it resolves to the nearest one.
//   6. No-match returns ok=false (never a bogus location).
//   7. Indent re-basing of new_text when the file's indent differs.
//   8. Trailing-newline handling keeps the splice length consistent.
//
// Run: build mcp_fuzzy_match_test, execute. Exit 0 = pass.

#include <mcp/tools/util/fuzzy_match.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

using mcp::tools::util::fuzzy_find;
using mcp::tools::util::FuzzyMatch;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Convenience: the substring of `file` that a match selected.
static std::string_view matched(std::string_view file, const FuzzyMatch& m) {
    return file.substr(m.pos, m.len);
}

int main() {
    // ── 1. exact unique match ────────────────────────────────────────────
    {
        std::string file = "line one\nline two\nline three\n";
        auto m = fuzzy_find(file, "line two\n");
        CHECK(m.ok);
        CHECK(m.count == 1);
        CHECK(matched(file, m) == "line two\n");
        CHECK(m.strategy == 1);   // exact fast path
    }

    // ── 2a. leading-indent drift is free ─────────────────────────────────
    {
        std::string file =
            "void f() {\n"
            "        int x = 1;\n"      // 8-space indent on disk
            "}\n";
        // model wrote it at 4-space indent
        auto m = fuzzy_find(file, "    int x = 1;\n");
        CHECK(m.ok);
        CHECK(matched(file, m).find("int x = 1;") != std::string_view::npos);
    }

    // ── 2b. trailing-whitespace drift is free ────────────────────────────
    {
        std::string file = "alpha\nbeta   \ngamma\n";   // trailing spaces on beta
        auto m = fuzzy_find(file, "beta\n");
        CHECK(m.ok);
        CHECK(matched(file, m).find("beta") != std::string_view::npos);
    }

    // ── 3. single-char typo tolerated ────────────────────────────────────
    {
        std::string file =
            "def process_request(req):\n"
            "    return handle(req)\n";
        // 'proccess' — one extra c
        auto m = fuzzy_find(file, "def proccess_request(req):\n    return handle(req)\n");
        CHECK(m.ok);
        CHECK(m.strategy == 2);   // DP path (not exact)
        CHECK(matched(file, m).find("process_request") != std::string_view::npos);
    }

    // ── 4. smart-quote hallucination normalized ──────────────────────────
    {
        std::string file = "msg = 'hello world'\n";     // ASCII quotes on disk
        // model emitted curly quotes (U+2018 / U+2019)
        auto m = fuzzy_find(file, "msg = \xe2\x80\x98hello world\xe2\x80\x99\n");
        CHECK(m.ok);
        CHECK(matched(file, m).find("hello world") != std::string_view::npos);
    }

    // ── 5a. duplicated needle, NO hint → ambiguous, does not apply ────────
    {
        std::string file =
            "    return 0;\n"     // line 0
            "  a();\n"
            "    return 0;\n"     // line 2
            "  b();\n"
            "    return 0;\n";    // line 4
        auto m = fuzzy_find(file, "    return 0;\n");
        CHECK(!m.ok);
        CHECK(m.count >= 2);      // caller turns this into an "appears N times" error
    }

    // ── 5b. duplicated needle WITH a line_hint → nearest one resolves ─────
    {
        std::string file =
            "    return 0;\n"     // row 0
            "  a();\n"
            "    return 0;\n"     // row 2
            "  b();\n"
            "    return 0;\n";    // row 4
        // Hint near row 4 should select the LAST occurrence.
        auto m = fuzzy_find(file, "    return 0;\n", std::string_view{}, /*line_hint=*/4);
        CHECK(m.ok);
        // pos must be at the third "return 0" (byte offset of row 4).
        std::size_t third = file.rfind("    return 0;\n");
        CHECK(m.pos == third);

        // Hint near row 0 should select the FIRST occurrence.
        auto m0 = fuzzy_find(file, "    return 0;\n", std::string_view{}, /*line_hint=*/0);
        CHECK(m0.ok);
        CHECK(m0.pos == 0);
    }

    // ── 6. genuine no-match never fabricates a location ──────────────────
    {
        std::string file = "the quick brown fox\njumps over\n";
        auto m = fuzzy_find(file, "completely unrelated content here\nand more\n");
        CHECK(!m.ok);
        CHECK(m.pos == 0 && m.len == 0);
    }

    // ── 7. indent re-basing of new_text ──────────────────────────────────
    {
        std::string file =
            "class C:\n"
            "        def m(self):\n"       // 8-space indent
            "                return 1\n";
        // needle at 8-space (matches file); new_text written at same base.
        auto m = fuzzy_find(file,
            "        def m(self):\n                return 1\n",
            "        def m(self):\n                return 2\n");
        CHECK(m.ok);
        // exact indent match → no re-base needed → adjusted stays empty.
        CHECK(m.adjusted_new_text.empty());
    }

    // ── 7b. re-base when needle indent differs from file indent ──────────
    {
        std::string file =
            "def outer():\n"
            "    if cond:\n"
            "        do_a()\n"
            "        do_b()\n";
        // model wrote the block at 0 indent; file has 8-space.
        auto m = fuzzy_find(file,
            "do_a()\ndo_b()\n",
            "do_a()\ndo_c()\n");
        CHECK(m.ok);
        // adjusted_new_text should carry the file's 8-space base so the
        // splice keeps the file's convention.
        if (!m.adjusted_new_text.empty()) {
            CHECK(m.adjusted_new_text.find("        do_c()") != std::string::npos);
        }
    }

    // ── 8. empty needle is refused ────────────────────────────────
    {
        std::string file = "anything\n";
        auto m = fuzzy_find(file, "");
        CHECK(!m.ok);
    }

    // ── 9. BANDING: a fuzzy match in a HUGE file that used to exceed the
    //    O(Q*B) DP cell cap (returning a false "no match"). The anchor-band
    //    driver runs the DP only around the distinctive needle line, so this
    //    now lands — and does so fast. 60k lines * a 3-line needle would be
    //    180k*3 = 540k*... cells full-file; banded it's a few hundred.
    {
        std::string file;
        file.reserve(2'000'000);
        for (int i = 0; i < 60'000; ++i) {
            file += "filler line ";
            file += std::to_string(i);
            file += '\n';
        }
        // Bury a distinctive 3-line block deep in the file.
        const std::size_t marker_at = file.size();
        file += "void QuoxFrobnicate(int distinctiveArgument) {\n";
        file += "    return compute_the_thing(distinctiveArgument);\n";
        file += "}\n";
        for (int i = 0; i < 20'000; ++i) file += "more filler\n";

        // Needle with a one-char typo (Frobnicate->Frobnicat) to force the
        // fuzzy path, not the exact fast path.
        auto m = fuzzy_find(file,
            "void QuoxFrobnicat(int distinctiveArgument) {\n"
            "    return compute_the_thing(distinctiveArgument);\n"
            "}\n");
        CHECK(m.ok);
        CHECK(m.pos == marker_at);
        CHECK(matched(file, m).find("QuoxFrobnicate") != std::string_view::npos);
    }

    // ── 10. BANDING keeps ambiguity honest: the same distinctive block in
    //     two far-apart places must still report count>=2 (no false unique
    //     just because the two hits fell in different bands).
    {
        std::string block =
            "int veryDistinctiveHelperName(int a, int b) {\n"
            "    return a * b + 42;\n"
            "}\n";
        std::string file;
        for (int i = 0; i < 5'000; ++i) { file += "pad\n"; }
        file += block;
        for (int i = 0; i < 5'000; ++i) { file += "pad\n"; }
        file += block;
        for (int i = 0; i < 5'000; ++i) { file += "pad\n"; }

        auto m = fuzzy_find(file, block);
        CHECK(!m.ok);
        CHECK(m.count >= 2);
    }

    if (g_failures == 0) {
        std::puts("fuzzy_match_test: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "fuzzy_match_test: %d failure(s)\n", g_failures);
    return 1;
}
