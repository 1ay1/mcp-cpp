// SPDX-License-Identifier: Apache-2.0
//
// diff.cpp — Myers-style LCS unified-diff engine, ported verbatim from
// agentty's src/diff/diff.cpp (compute + render_unified only; apply_accepted
// is a host-UI concern and stays in the host).

#include "diff.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace mcp::tools::detail::diff {

namespace {
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

struct Edit { enum K { Keep, Del, Ins } k; int a_idx, b_idx; };

// Cap on the LCS DP matrix. compute_edits builds an (n+1)*(m+1) int matrix,
// so cost is O(n*m) ints. Above this product we skip the LCS entirely and
// emit a degenerate "delete everything, then insert everything" edit script:
// the diff is still correct, just not minimal. This bounds memory at ~64 MiB
// (16M ints) and keeps a multi-MB paste from OOM-ing or hanging the host.
constexpr std::size_t kMaxLcsCells = 16u * 1024u * 1024u;

std::vector<Edit> full_replace_edits(std::size_t n, std::size_t m) {
    std::vector<Edit> edits;
    edits.reserve(n + m);
    for (std::size_t i = 0; i < n; ++i)
        edits.push_back({Edit::Del, static_cast<int>(i), -1});
    for (std::size_t j = 0; j < m; ++j)
        edits.push_back({Edit::Ins, -1, static_cast<int>(j)});
    return edits;
}

std::vector<Edit> compute_edits(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
    const std::size_t n = a.size(), m = b.size();
    if (n != 0 && m > kMaxLcsCells / n)
        return full_replace_edits(n, m);

    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j)
            dp[i][j] = (a[i-1] == b[j-1]) ? dp[i-1][j-1] + 1
                                          : std::max(dp[i-1][j], dp[i][j-1]);
    std::vector<Edit> edits;
    std::size_t i = n, j = m;
    while (i > 0 && j > 0) {
        if (a[i-1] == b[j-1]) { edits.push_back({Edit::Keep, static_cast<int>(i-1), static_cast<int>(j-1)}); --i; --j; }
        else if (dp[i-1][j] >= dp[i][j-1]) { edits.push_back({Edit::Del, static_cast<int>(i-1), -1}); --i; }
        else { edits.push_back({Edit::Ins, -1, static_cast<int>(j-1)}); --j; }
    }
    while (i > 0) { --i; edits.push_back({Edit::Del, static_cast<int>(i), -1}); }
    while (j > 0) { --j; edits.push_back({Edit::Ins, -1, static_cast<int>(j)}); }
    std::reverse(edits.begin(), edits.end());
    return edits;
}
} // namespace

Diff compute(const std::string& path,
             const std::string& before,
             const std::string& after) {
    Diff c;
    c.path   = path;
    c.before = before;
    c.after  = after;

    auto a = split_lines(before);
    auto b = split_lines(after);
    auto edits = compute_edits(a, b);

    const int ctx = 3;
    int added = 0, removed = 0;
    std::vector<bool> is_change(edits.size(), false);
    for (size_t k = 0; k < edits.size(); ++k)
        if (edits[k].k != Edit::Keep) is_change[k] = true;

    size_t k = 0;
    while (k < edits.size()) {
        while (k < edits.size() && !is_change[k]) ++k;
        if (k >= edits.size()) break;
        size_t start = (k > (size_t)ctx) ? k - ctx : 0;
        size_t end = k;
        while (end < edits.size()) {
            size_t last_change = end;
            size_t probe = end;
            size_t gap = 0;
            while (probe < edits.size() && gap <= (size_t)(2 * ctx)) {
                if (is_change[probe]) { last_change = probe; gap = 0; }
                else gap++;
                probe++;
            }
            if (last_change == end) break;
            end = last_change;
        }
        end = std::min(edits.size() - 1, end + ctx);

        Hunk h;
        int old_start = -1, new_start = -1;
        int old_len = 0, new_len = 0;
        std::ostringstream patch;
        // Emit deletions before insertions within each contiguous change run
        // (git convention). The LCS backtrace can group inserts ahead of
        // deletes; rendered through a two-column diff gutter that ordering
        // makes the old/new line numbers read out of sequence (new line 2
        // appearing above old line 2). Buffer each run and flush "-" before
        // "+" on the next context line so both gutter columns stay monotonic
        // and the changed lines line up.
        std::string del_buf, ins_buf;
        auto flush_run = [&] {
            if (!del_buf.empty()) patch << del_buf;
            if (!ins_buf.empty()) patch << ins_buf;
            del_buf.clear();
            ins_buf.clear();
        };
        for (size_t i2 = start; i2 <= end; ++i2) {
            const auto& e = edits[i2];
            if (e.k == Edit::Keep) {
                flush_run();
                if (old_start < 0) old_start = e.a_idx + 1;
                if (new_start < 0) new_start = e.b_idx + 1;
                old_len++; new_len++;
                patch << " " << a[static_cast<std::size_t>(e.a_idx)] << "\n";
            } else if (e.k == Edit::Del) {
                if (old_start < 0) old_start = e.a_idx + 1;
                old_len++;
                del_buf += "-"; del_buf += a[static_cast<std::size_t>(e.a_idx)]; del_buf += "\n";
                removed++;
            } else {
                if (new_start < 0) new_start = e.b_idx + 1;
                new_len++;
                ins_buf += "+"; ins_buf += b[static_cast<std::size_t>(e.b_idx)]; ins_buf += "\n";
                added++;
            }
        }
        flush_run();
        h.old_start = std::max(1, old_start);
        h.new_start = std::max(1, new_start);
        h.old_len = old_len;
        h.new_len = new_len;
        h.patch = patch.str();
        c.hunks.push_back(std::move(h));
        k = end + 1;
    }

    c.added = added;
    c.removed = removed;
    return c;
}

std::string render_unified(const Diff& c) {
    std::ostringstream oss;
    oss << "--- a/" << c.path << "\n";
    oss << "+++ b/" << c.path << "\n";
    for (const auto& h : c.hunks) {
        oss << "@@ -" << h.old_start << "," << h.old_len
            << " +" << h.new_start << "," << h.new_len << " @@\n";
        oss << h.patch;
    }
    return oss.str();
}

} // namespace mcp::tools::detail::diff
