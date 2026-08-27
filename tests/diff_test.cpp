// diff_test.cpp — the internal unified-diff engine (edit/write tool output +
// FileChange line counts). Pins the property that matters most: a small edit
// to a LARGE file produces a MINIMAL diff, not a whole-file delete+insert.
//
// Regression guard for the "every line shown as '-'" bug: the LCS had an
// O(n*m) cap (kMaxLcsCells) that tripped on any file past ~4k lines, degrading
// a one-line edit into a full-file replacement. compute_edits now trims the
// common prefix/suffix first, so the DP only spans the changed window.

#include <doctest/doctest.h>

// diff.hpp is INTERNAL to mcp::tools (src/tools, a PRIVATE include dir), so
// reach it by relative path from tests/ rather than the public include tree.
#include "../src/tools/diff.hpp"

#include <string>

using namespace mcp::tools::detail::diff;

namespace {
std::string lines(int lo, int hi) {   // "line lo\n" .. "line hi-1\n"
    std::string s;
    for (int i = lo; i < hi; ++i) s += "line " + std::to_string(i) + "\n";
    return s;
}
} // namespace

TEST_CASE("diff: one-line edit in a large file is minimal") {
    // 5000-line file; change exactly one line in the middle.
    const std::string before = lines(0, 5000);
    std::string after = before;
    const std::string old_line = "line 2500\n";
    after.replace(after.find(old_line), old_line.size(), "CHANGED 2500\n");

    const Diff d = compute("big.c", before, after);
    // The whole point: NOT 5000/5000. One replaced line = 1 removed + 1 added.
    CHECK(d.added == 1);
    CHECK(d.removed == 1);
    CHECK(d.hunks.size() == 1);
}

TEST_CASE("diff: correctness on the small cases") {
    // Pure insert.
    {
        const Diff d = compute("x", "a\nb\nc\n", "a\nb\nX\nc\n");
        CHECK(d.added == 1);
        CHECK(d.removed == 0);
    }
    // Pure delete.
    {
        const Diff d = compute("x", "a\nb\nc\n", "a\nc\n");
        CHECK(d.added == 0);
        CHECK(d.removed == 1);
    }
    // Identical → no hunks.
    {
        const Diff d = compute("x", "a\nb\n", "a\nb\n");
        CHECK(d.added == 0);
        CHECK(d.removed == 0);
        CHECK(d.hunks.empty());
    }
    // Whole-content change.
    {
        const Diff d = compute("x", "a\nb\nc\n", "x\ny\nz\n");
        CHECK(d.added == 3);
        CHECK(d.removed == 3);
    }
    // Change at the very FIRST line (no shared prefix) of a big file.
    {
        std::string before = lines(0, 3000);
        std::string after = before;
        after.replace(0, std::string("line 0\n").size(), "FIRST\n");
        const Diff d = compute("x", before, after);
        CHECK(d.added == 1);
        CHECK(d.removed == 1);
    }
    // Change at the very LAST line (no shared suffix) of a big file.
    {
        std::string before = lines(0, 3000);
        std::string after = before;
        const std::string last = "line 2999\n";
        after.replace(after.rfind(last), last.size(), "LAST\n");
        const Diff d = compute("x", before, after);
        CHECK(d.added == 1);
        CHECK(d.removed == 1);
    }
}
