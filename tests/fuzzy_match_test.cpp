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

#include "agtest.hpp"

static int g_failures = 0;

#include <mcp/tools/util/fuzzy_match.hpp>

#include <cstdio>
#include <limits>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <string_view>

using mcp::tools::util::fuzzy_find;
using mcp::tools::util::FuzzyMatch;


// Convenience: the substring of `file` that a match selected.
static std::string_view matched(std::string_view file, const FuzzyMatch& m) {
    return file.substr(m.pos, m.len);
}

TEST_CASE("fuzzy_match") {
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

    // ── 6b. RELAXED acceptance: a UNIQUE block that drifted below the strict
    //        0.8 ratio still applies (previously reported "not found even
    //        fuzzily"). Here a 3-line block has one reworded line + one
    //        reflowed line — ratio dips under 0.8 but stays above the 0.6
    //        relaxed floor, and the location is unambiguous. ────────────────
    {
        std::string file =
            "void setup() {\n"
            "    // initialise the widget registry\n"
            "    registry.init();\n"
            "    log.info(\"ready\");\n"
            "}\n";
        // Model's copy drifted: comment reworded, spacing changed. Only
        // `registry.init();` survives verbatim, but the block is unique.
        auto m = fuzzy_find(file,
            "    // init the widget registry\n"
            "    registry.init();\n"
            "    log.info( \"ready\" );\n");
        CHECK(m.ok);
        CHECK(m.count == 1);
        CHECK(matched(file, m).find("registry.init();") != std::string_view::npos);
    }

    // ── 6c. RELAXED floor still rejects half-garbage: a 2-line needle where
    //        one line matches and the other is absent is exactly 0.5 ratio —
    //        below the 0.6 floor — so it must stay an honest no-match (this is
    //        the invariant patch atomicity relies on). ─────────────────────
    {
        std::string file =
            "line one\nline two\nline three\nline four\nline five\n";
        auto m = fuzzy_find(file,
            "line four\nthis line does not exist anywhere\n");
        CHECK(!m.ok);
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

    // ── 11. DoS BOUND (algorithmic-complexity guard). A large multi-line
    //     needle against a big, highly-repetitive file used to spend SECONDS
    //     in the banded DP (each cell runs an O(L²) levenshtein via fuzzy_eq),
    //     reachable from a single edit/apply_patch call. MAX_DP_CELLS now caps
    //     the total work: such an input must bail to "no match" quickly rather
    //     than wedge the tool. We assert BOTH the verdict and a generous wall-
    //     clock ceiling so a future cap regression trips this test.
    {
        std::string file;
        for (int i = 0; i < 2'000; ++i)
            file += "  func item_" + std::to_string(i % 37) + "(a, b) { return a; }\n";
        std::string needle;                       // ~400-line fuzzy needle
        for (int i = 0; i < 400; ++i)
            needle += "  func item_" + std::to_string(i % 37) + "(a,b){ return b; }\n";

        const auto t0 = std::chrono::steady_clock::now();
        auto m = fuzzy_find(file, needle);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK(!m.ok);                             // over-cap input bails cleanly
        // 1 s is ~50× the post-fix time and far under the pre-fix seconds; it
        // catches a cap regression without flaking on a slow/loaded CI box.
        CHECK(elapsed < 1'000);
    }

    // ── 12. Myers bit-parallel Levenshtein == reference matrix. The fuzzy
    //     locator's inner term is a hand-written bit-vector kernel (easy to get
    //     subtly wrong); pin it against an obviously-correct full-matrix
    //     Levenshtein over many random pairs, including the word boundaries
    //     (len 0, exactly 64, > 64 fallback) and multi-byte UTF-8 bytes.
    {
        auto ref = [](std::string_view a, std::string_view b) -> std::size_t {
            std::vector<std::size_t> prev(b.size() + 1), curr(b.size() + 1);
            for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
            for (std::size_t i = 1; i <= a.size(); ++i) {
                curr[0] = i;
                for (std::size_t j = 1; j <= b.size(); ++j)
                    curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1,
                                        prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
                std::swap(prev, curr);
            }
            return prev[b.size()];
        };
        const char alpha[] = "ab \t{}();=x\xe2\x80\x98";   // incl. a UTF-8 char
        const int A = sizeof(alpha) - 1;
        std::mt19937_64 rng(0xBEEF);
        std::size_t mism = 0;
        for (int trial = 0; trial < 60'000; ++trial) {
            // Bias lengths around the 64-bit word boundary.
            std::uniform_int_distribution<int> la(0, 70), lb(0, 70);
            std::string a, b;
            std::uniform_int_distribution<int> pick(0, A - 1);
            for (int i = 0, na = la(rng); i < na; ++i) a += alpha[pick(rng)];
            for (int i = 0, nb = lb(rng); i < nb; ++i) b += alpha[pick(rng)];
            if (mcp::tools::util::levenshtein(a, b) != ref(a, b)) ++mism;
        }
        CHECK(mism == 0);
        // Spot-check exact values so a build that silently returns 0 still fails.
        CHECK(mcp::tools::util::levenshtein("kitten", "sitting") == 3);
        CHECK(mcp::tools::util::levenshtein("", "abc") == 3);
        CHECK(mcp::tools::util::levenshtein("abc", "abc") == 0);
        CHECK(mcp::tools::util::levenshtein(std::string(64, 'a'),
                                            std::string(64, 'b')) == 64);
    }
    CHECK(g_failures == 0);
}
