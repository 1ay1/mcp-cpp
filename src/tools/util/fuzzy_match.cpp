#include <mcp/tools/util/fuzzy_match.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::util {

// ─────────────────────────────────────────────────────────────────────────
// Cost model. Matches Zed's StreamingFuzzyMatcher byte-for-byte.
//
//   REPLACEMENT_COST = 1   line align, content differs by a typo-tolerant
//                          fuzzy_eq (cheap — most "drift" lives here).
//   INSERTION_COST   = 3   buffer has an extra line the query skips —
//                          cheap, so the model can omit blank/comment lines.
//   DELETION_COST    = 10  query has an extra line the buffer doesn't —
//                          expensive, biases the search toward windows
//                          that contain ALL the query lines.
//
// Two lines are "fuzzy equal" when normalized Levenshtein (1 − d/max_len)
// is ≥ 0.8 — i.e. ~20% of characters can differ before the line stops
// counting as a match. That covers single-char typos, smart quotes,
// inserted/removed punctuation, NBSP, etc., without needing a separate
// unicode-normalization pass.
// ─────────────────────────────────────────────────────────────────────────
namespace {

constexpr std::uint32_t REPLACEMENT_COST = 1;
constexpr std::uint32_t INSERTION_COST   = 3;
constexpr std::uint32_t DELETION_COST    = 10;
constexpr double        FUZZY_EQ_THRESHOLD = 0.8;   // line-level
constexpr double        MATCH_RATIO        = 0.8;   // accepted match
constexpr std::uint32_t LINE_HINT_TOLERANCE = 200;  // lines
// Beyond this, the O(n*m) DP gets expensive. The exact-match fast path
// handles every case below the cap; above it we conservatively bail to
// "no match" rather than burn the watchdog. Real files vs. real needles
// are well under this in practice.
constexpr std::size_t   MAX_DP_CELLS = 2'000'000;   // ~16 MiB of state

enum class Dir : std::uint8_t { Up, Left, Diag };

struct Cell {
    std::uint32_t cost;
    Dir           dir;
};

// ─────────────────────────────────────────────────────────────────────────
// Levenshtein distance (classic two-row Wagner-Fischer). We only need the
// normalized score, but the distance itself is what we threshold.
// O(|a| * |b|) time, O(min(|a|, |b|)) space. Lines are typically <120 cols
// so this is cheap.
// ─────────────────────────────────────────────────────────────────────────
std::size_t levenshtein(std::string_view a, std::string_view b) noexcept {
    if (a.size() < b.size()) std::swap(a, b);
    if (b.empty()) return a.size();

    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> curr(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;

    for (std::size_t i = 1; i <= a.size(); ++i) {
        curr[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            std::size_t ins = curr[j - 1] + 1;
            std::size_t del = prev[j]     + 1;
            curr[j] = std::min({sub, ins, del});
        }
        std::swap(prev, curr);
    }
    return prev[b.size()];
}

// ───────────────────────────────────────────────────────────────────────────
// Smart-quote / dash normalization. LLMs often hallucinate curly quotes
// (U+2018/2019/201C/201D) and em-dashes (U+2014) when the file has plain
// ASCII. Normalize these BEFORE Levenshtein so they don't inflate the
// edit distance. Zero allocation when no smart chars present.
//
// U+2018 ‘ (left single quote)  -> '
// U+2019 ’ (right single quote) -> '
// U+201C “ (left double quote)  -> "
// U+201D ” (right double quote) -> "
// U+2014 — (em dash)            -> --
// U+2013 – (en dash)            -> -
// ───────────────────────────────────────────────────────────────────────────

bool has_smart_chars(std::string_view s) noexcept {
    // Quick scan for the UTF-8 lead byte 0xE2 (covers U+2000–U+2FFF).
    for (char c : s)
        if (static_cast<unsigned char>(c) == 0xE2) return true;
    return false;
}

std::string normalize_smart_chars(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        // Check for 3-byte UTF-8 starting with 0xE2.
        if (c0 == 0xE2 && i + 2 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            // U+2018/2019: E2 80 98 / E2 80 99 -> '
            if (c1 == 0x80 && (c2 == 0x98 || c2 == 0x99)) {
                out.push_back('\'');
                i += 3;
                continue;
            }
            // U+201C/201D: E2 80 9C / E2 80 9D -> "
            if (c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) {
                out.push_back('"');
                i += 3;
                continue;
            }
            // U+2014 (em dash): E2 80 94 -> --
            if (c1 == 0x80 && c2 == 0x94) {
                out.append("--");
                i += 3;
                continue;
            }
            // U+2013 (en dash): E2 80 93 -> -
            if (c1 == 0x80 && c2 == 0x93) {
                out.push_back('-');
                i += 3;
                continue;
            }
        }
        out.push_back(static_cast<char>(c0));
        ++i;
    }
    return out;
}

// Cheap pre-filter: if the length difference alone forces normalized
// distance below the threshold, skip the full Levenshtein computation.
// Lines that are wildly different sizes can't be fuzzy-equal.
// Also normalizes smart-quotes/dashes before comparing.
bool fuzzy_eq(std::string_view a, std::string_view b) noexcept {
    if (a.empty() && b.empty()) return true;

    // Normalize smart-quotes/dashes if present. This handles the common
    // case where the LLM hallucinates curly quotes but the file has ASCII.
    // Only allocates when smart chars are actually present.
    std::string a_norm, b_norm;
    if (has_smart_chars(a)) {
        a_norm = normalize_smart_chars(a);
        a = a_norm;
    }
    if (has_smart_chars(b)) {
        b_norm = normalize_smart_chars(b);
        b = b_norm;
    }

    auto max_len = std::max(a.size(), b.size());
    if (max_len == 0) return true;
    auto min_len_diff = (a.size() > b.size()) ? (a.size() - b.size())
                                              : (b.size() - a.size());
    // Lower bound on Levenshtein is |len(a) - len(b)|.
    double min_norm = 1.0 - static_cast<double>(min_len_diff) / static_cast<double>(max_len);
    if (min_norm < FUZZY_EQ_THRESHOLD) return false;
    auto d = levenshtein(a, b);
    double norm = 1.0 - static_cast<double>(d) / static_cast<double>(max_len);
    return norm >= FUZZY_EQ_THRESHOLD;
}

// ─────────────────────────────────────────────────────────────────────────
// Line index. We track three offsets per line:
//   start         — byte offset of first char.
//   end           — byte offset one past the trailing '\n' (or file end).
//   indent_end    — first non-whitespace byte (== trimmed_end on a blank line).
//   trimmed_end   — one past the last non-whitespace byte. trimmed view is
//                   `[indent_end, trimmed_end)` and is what the DP compares.
//
// `trim(s)` returns the trimmed view of a string_view in line-oriented
// callers (the needle is also split via lines and trimmed the same way).
// ─────────────────────────────────────────────────────────────────────────

struct Line {
    std::size_t start;
    std::size_t end;
    std::size_t indent_end;
    std::size_t trimmed_end;
};

constexpr bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

std::vector<Line> scan_lines(std::string_view s) {
    std::vector<Line> out;
    out.reserve(s.size() / 40 + 1);
    auto push = [&](std::size_t start, std::size_t end) {
        std::size_t te = end;
        if (te > start && s[te - 1] == '\n') --te;
        while (te > start && is_ws(s[te - 1])) --te;
        std::size_t ie = start;
        while (ie < te && (s[ie] == ' ' || s[ie] == '\t')) ++ie;
        out.push_back({start, end, ie, te});
    };
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') { push(start, i + 1); start = i + 1; }
    }
    if (start < s.size()) push(start, s.size());
    return out;
}

inline std::string_view trimmed_of(std::string_view s, const Line& l) noexcept {
    return s.substr(l.indent_end, l.trimmed_end - l.indent_end);
}

// ─────────────────────────────────────────────────────────────────────────
// Indent adjustment for `new_text`. When the matched buffer region's
// indentation differs from the needle's, the model's new_text — which
// was written at the needle's level — must be shifted to the buffer's
// level so the splice preserves the file's convention.
//
// We capture both sides as a STRUCTURAL prefix:
//   needle_base = longest common whitespace prefix across non-blank needle lines
//   file_base   = same, for the matched buffer lines
// On apply, we strip `needle_base` from each non-blank new_text line and
// prepend `file_base`. Blank lines stay verbatim.
//
// Compared to Zed's per-character `IndentDelta { Spaces(±n) | Tabs(±n) }`,
// this byte-prefix approach handles mixed tabs+spaces consistently and
// degrades to a no-op when the bases are identical.
// ─────────────────────────────────────────────────────────────────────────

struct IndentDelta {
    bool        have = false;
    std::string needle_base;
    std::string file_base;
};

std::size_t common_prefix_len(std::string_view a, std::string_view b) noexcept {
    std::size_t n = std::min(a.size(), b.size());
    std::size_t k = 0;
    while (k < n && a[k] == b[k]) ++k;
    return k;
}

IndentDelta detect_indent_delta(std::string_view file_text,
                                std::string_view needle_text,
                                const std::vector<Line>& fl,
                                std::size_t fl_lo,
                                std::size_t fl_hi,
                                const std::vector<Line>& nl) {
    std::string_view needle_base;
    bool first = true;
    for (const auto& N : nl) {
        if (N.indent_end == N.trimmed_end) continue;
        std::string_view ind{needle_text.data() + N.start, N.indent_end - N.start};
        if (first) { needle_base = ind; first = false; }
        else needle_base = needle_base.substr(0, common_prefix_len(needle_base, ind));
    }
    if (first) return {};

    std::string_view file_base;
    first = true;
    for (std::size_t i = fl_lo; i < fl_hi; ++i) {
        const auto& F = fl[i];
        if (F.indent_end == F.trimmed_end) continue;
        std::string_view ind{file_text.data() + F.start, F.indent_end - F.start};
        if (first) { file_base = ind; first = false; }
        else file_base = file_base.substr(0, common_prefix_len(file_base, ind));
    }
    if (first) return {};

    IndentDelta d;
    d.have = true;
    d.needle_base.assign(needle_base);
    d.file_base.assign(file_base);
    return d;
}

std::string apply_indent_delta(std::string_view text, const IndentDelta& d) {
    if (!d.have || d.needle_base == d.file_base) return std::string{text};
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t line_start = i;
        while (i < text.size() && text[i] != '\n') ++i;
        std::size_t line_end = i;
        if (i < text.size()) ++i;
        bool blank = true;
        for (std::size_t k = line_start; k < line_end; ++k)
            if (!is_ws(text[k])) { blank = false; break; }
        if (blank) {
            out.append(text.data() + line_start, line_end - line_start);
        } else {
            std::size_t strip = 0;
            if (!d.needle_base.empty()
                && line_end - line_start >= d.needle_base.size()
                && std::string_view{text.data() + line_start,
                                    d.needle_base.size()} == d.needle_base) {
                strip = d.needle_base.size();
            }
            out.append(d.file_base);
            out.append(text.data() + line_start + strip,
                       line_end - line_start - strip);
        }
        if (line_end < text.size()) out.push_back('\n');
    }
    return out;
}

// Count exact byte occurrences of `needle` in `file`.
int count_occurrences(std::string_view file, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > file.size()) return 0;
    int n = 0;
    std::size_t p = 0;
    while ((p = file.find(needle, p)) != std::string_view::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────────
// The DP itself. Returns every (buffer_row_start, buffer_row_end_exclusive)
// pair that ties for the minimum cost in the final row AND passes the
// match-ratio quality gate. Caller picks one using line_hint or reports
// ambiguity.
// ─────────────────────────────────────────────────────────────────────────

struct DPMatch {
    std::size_t row_start;     // inclusive
    std::size_t row_end;       // inclusive
    std::uint32_t cost;
};

std::vector<DPMatch> run_line_dp(std::string_view file,
                                 std::string_view needle,
                                 const std::vector<Line>& fl,
                                 const std::vector<Line>& nl,
                                 std::size_t b_lo,
                                 std::size_t b_hi) {
    if (nl.empty() || fl.empty() || b_lo >= b_hi) return {};

    const std::size_t Q = nl.size();          // query rows
    const std::size_t B = b_hi - b_lo;         // buffer rows IN THIS BAND
    const std::size_t cols = B + 1;
    const std::size_t rows = Q + 1;

    if (rows * cols > MAX_DP_CELLS) return {};

    // Pre-compute trimmed needle lines (cheap, lets fuzzy_eq skip work).
    std::vector<std::string_view> needle_tr;
    needle_tr.reserve(Q);
    for (const auto& N : nl) needle_tr.push_back(trimmed_of(needle, N));

    std::vector<Cell> dp(rows * cols, Cell{0, Dir::Diag});

    // Top row is the "empty query" — cost 0 anywhere in the buffer (we can
    // start matching at any column for free).
    for (std::size_t c = 0; c <= B; ++c) dp[0 * cols + c] = {0, Dir::Diag};

    // Left column: matching i query lines against zero buffer lines costs
    // i * DELETION_COST. (Skipping query lines is expensive — biases toward
    // complete-query matches.)
    for (std::size_t r = 1; r <= Q; ++r)
        dp[r * cols + 0] = {static_cast<std::uint32_t>(r) * DELETION_COST, Dir::Up};

    for (std::size_t r = 1; r <= Q; ++r) {
        std::string_view qline = needle_tr[r - 1];
        for (std::size_t c = 1; c <= B; ++c) {
            std::string_view bline = trimmed_of(file, fl[b_lo + c - 1]);

            std::uint32_t up = dp[(r - 1) * cols + c].cost;
            up = (up > std::numeric_limits<std::uint32_t>::max() - DELETION_COST)
               ?  std::numeric_limits<std::uint32_t>::max() : up + DELETION_COST;

            std::uint32_t left = dp[r * cols + (c - 1)].cost;
            left = (left > std::numeric_limits<std::uint32_t>::max() - INSERTION_COST)
                 ?  std::numeric_limits<std::uint32_t>::max() : left + INSERTION_COST;

            std::uint32_t diag_base = dp[(r - 1) * cols + (c - 1)].cost;
            std::uint32_t diag;
            if (qline == bline) {
                diag = diag_base;
            } else if (fuzzy_eq(qline, bline)) {
                diag = (diag_base > std::numeric_limits<std::uint32_t>::max() - REPLACEMENT_COST)
                     ?  std::numeric_limits<std::uint32_t>::max() : diag_base + REPLACEMENT_COST;
            } else {
                constexpr std::uint32_t mismatch = DELETION_COST + INSERTION_COST;
                diag = (diag_base > std::numeric_limits<std::uint32_t>::max() - mismatch)
                     ?  std::numeric_limits<std::uint32_t>::max() : diag_base + mismatch;
            }

            Cell best{up, Dir::Up};
            if (left < best.cost) best = {left, Dir::Left};
            if (diag < best.cost) best = {diag, Dir::Diag};
            dp[r * cols + c] = best;
        }
    }

    // Find all columns in the final row that tie for the minimum cost.
    std::uint32_t best_cost = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::size_t> best_cols;
    for (std::size_t c = 1; c <= B; ++c) {
        auto cost = dp[Q * cols + c].cost;
        if (cost < best_cost) { best_cost = cost; best_cols.clear(); best_cols.push_back(c); }
        else if (cost == best_cost)               { best_cols.push_back(c); }
    }
    if (best_cols.empty()) return {};

    // Trace back from each best column. We need the start row of the match
    // and how many query lines actually aligned (diagonal moves).
    std::vector<DPMatch> matches;
    matches.reserve(best_cols.size());
    for (std::size_t end_col : best_cols) {
        std::size_t r = Q;
        std::size_t c = end_col;
        std::size_t matched = 0;
        while (r > 0 && c > 0) {
            auto d = dp[r * cols + c].dir;
            if (d == Dir::Diag) {
                // Only count this as a "matched line" when the trimmed
                // pair is actually equal-or-fuzzy-equal — a Diag move
                // with full mismatch cost is the DP's way of saying
                // "we had to align this pair but they're not really the
                // same line". Counting those inflates the match ratio
                // and makes the quality gate accept garbage.
                std::string_view qline = needle_tr[r - 1];
                std::string_view bline = trimmed_of(file, fl[b_lo + c - 1]);
                if (qline == bline || fuzzy_eq(qline, bline)) ++matched;
                --r; --c;
            } else if (d == Dir::Up) {
                --r;
            } else {
                --c;
            }
        }
        std::size_t row_start = b_lo + c;            // first buffer row used
        std::size_t row_end   = b_lo + end_col - 1;  // inclusive last buffer row
        std::size_t buf_rows  = end_col - c;         // band-local span (rows used)
        double ratio = static_cast<double>(matched)
                     / static_cast<double>(std::max(buf_rows, Q));
        if (ratio >= MATCH_RATIO)
            matches.push_back({row_start, row_end, best_cost});
    }
    return matches;
}

// ── Candidate banding ────────────────────────────────────────────────────
// The DP is O(Q * B). Running it over the WHOLE file is wasteful (and hits
// MAX_DP_CELLS on large files, returning a false "no match"). Instead we
// find ANCHOR rows — buffer lines that exact- or fuzzy-match the needle's
// most distinctive line — and run the DP only in a small window around each.
// This turns the common case into O(Q * band) and lets huge files match.
//
// Falls back to a single full-file band when no anchor is found (e.g. a
// needle whose every line is blank/duplicated) AND the file fits the cap.
struct Band { std::size_t lo, hi; };

std::vector<Band> candidate_bands(std::string_view file, std::string_view needle,
                                  const std::vector<Line>& fl,
                                  const std::vector<Line>& nl) {
    const std::size_t B = fl.size();
    const std::size_t Q = nl.size();
    // Slack lets the DP absorb inserted/deleted lines around the anchor.
    const std::size_t slack = std::max<std::size_t>(Q, 8);

    // Pick the most DISTINCTIVE needle line as the anchor: the longest
    // non-blank trimmed line is least likely to occur spuriously. Cheap and
    // far more selective than always using line 0 (often `{` or blank).
    std::size_t anchor_ni = SIZE_MAX;
    std::size_t anchor_len = 0;
    std::size_t anchor_off = 0;  // needle-row index of the anchor (for windowing)
    for (std::size_t i = 0; i < Q; ++i) {
        std::string_view t = trimmed_of(needle, nl[i]);
        if (t.size() > anchor_len) { anchor_len = t.size(); anchor_ni = i; anchor_off = i; }
    }

    std::vector<Band> bands;
    if (anchor_ni != SIZE_MAX && anchor_len >= 3) {
        std::string_view atext = trimmed_of(needle, nl[anchor_ni]);
        const std::size_t alen = atext.size();
        for (std::size_t r = 0; r < B; ++r) {
            std::string_view btext = trimmed_of(file, fl[r]);
            // Cheap length gate first: fuzzy_eq can't pass the 0.8 threshold
            // when the lengths differ by >20%, so skip the O(n*m) Levenshtein
            // for the overwhelming majority of non-matching lines. Exact hits
            // still take the fast equality path.
            bool hit = (btext == atext);
            if (!hit && btext.size() >= 3) {
                const std::size_t blen = btext.size();
                const std::size_t lo = std::min(alen, blen), hi = std::max(alen, blen);
                if (hi != 0 && static_cast<double>(lo) / static_cast<double>(hi) >= 0.8)
                    hit = fuzzy_eq(atext, btext);
            }
            if (hit) {
                // Window: anchor may be `anchor_off` rows into the needle, so
                // reach back that far (plus slack) and forward for the rest.
                std::size_t back = anchor_off + slack;
                std::size_t fwd  = (Q - anchor_off) + slack;
                std::size_t lo = (r > back) ? r - back : 0;
                std::size_t hi = std::min(B, r + fwd);
                bands.push_back({lo, hi});
            }
        }
        // Coalesce overlapping/adjacent windows so the DP isn't re-run over
        // the same rows (and duplicate matches don't inflate the count).
        if (!bands.empty()) {
            std::sort(bands.begin(), bands.end(),
                      [](const Band& a, const Band& b){ return a.lo < b.lo; });
            std::vector<Band> merged;
            merged.push_back(bands.front());
            for (std::size_t i = 1; i < bands.size(); ++i) {
                if (bands[i].lo <= merged.back().hi)
                    merged.back().hi = std::max(merged.back().hi, bands[i].hi);
                else
                    merged.push_back(bands[i]);
            }
            return merged;
        }
    }

    // No anchor hit. One full-file band — the DP's own cap guards cost.
    return {{0, B}};
}

// Run the DP across every candidate band and union the results.
std::vector<DPMatch> run_banded_dp(std::string_view file, std::string_view needle,
                                   const std::vector<Line>& fl,
                                   const std::vector<Line>& nl) {
    auto bands = candidate_bands(file, needle, fl, nl);
    std::vector<DPMatch> all;
    for (const auto& b : bands) {
        auto part = run_line_dp(file, needle, fl, nl, b.lo, b.hi);
        for (auto& m : part) all.push_back(m);
    }
    // A match can surface from two overlapping bands post-coalesce only if
    // coalescing missed it; dedup by (row_start,row_end) defensively so the
    // ambiguity count stays honest.
    std::sort(all.begin(), all.end(), [](const DPMatch& a, const DPMatch& b){
        if (a.row_start != b.row_start) return a.row_start < b.row_start;
        return a.row_end < b.row_end;
    });
    all.erase(std::unique(all.begin(), all.end(), [](const DPMatch& a, const DPMatch& b){
        return a.row_start == b.row_start && a.row_end == b.row_end;
    }), all.end());
    // Keep only the globally-minimum-cost matches (each band reported its own
    // best; across bands the true best cost may be lower in one of them).
    if (!all.empty()) {
        std::uint32_t best = all.front().cost;
        for (const auto& m : all) best = std::min(best, m.cost);
        std::vector<DPMatch> keep;
        for (auto& m : all) if (m.cost == best) keep.push_back(m);
        return keep;
    }
    return all;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────

FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle) {
    return fuzzy_find(file, needle, {},
                      std::numeric_limits<std::uint32_t>::max());
}

FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text) {
    return fuzzy_find(file, needle, new_text,
                      std::numeric_limits<std::uint32_t>::max());
}

FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text,
                      std::uint32_t    line_hint) {
    if (needle.empty()) return {false, 0, 0, 0, {}, 0};

    // ── Exact-match fast path ────────────────────────────────────────────
    // Zero allocations, no DP. Single match wins outright; ambiguous cases
    // still need the DP because a `line_hint` can break the tie.
    {
        int n = count_occurrences(file, needle);
        if (n == 1) {
            auto pos = file.find(needle);
            return {true, pos, needle.size(), 1, {}, 1};
        }
        // n == 0 → keep going (DP may still find a fuzzy hit).
        // n >= 2 → keep going too; line_hint may disambiguate. Below we
        // return that count so the error message is honest if neither
        // path lands a unique match.
    }

    // ── Line index ───────────────────────────────────────────────────────
    auto fl = scan_lines(file);
    auto nl = scan_lines(needle);

    auto matches = run_banded_dp(file, needle, fl, nl);
    if (matches.empty()) {
        // Re-check exact-count for the caller's diagnostic — if exact was
        // ambiguous and DP found nothing better, surface the exact count.
        int n = count_occurrences(file, needle);
        if (n >= 2) return {false, 0, 0, n, {}, 0};
        return {false, 0, 0, 0, {}, 0};
    }

    // Pick a single match. Prefer line_hint when supplied AND we have
    // multiple candidates within tolerance.
    const DPMatch* pick = nullptr;
    if (matches.size() == 1) {
        pick = &matches[0];
    } else if (line_hint != std::numeric_limits<std::uint32_t>::max()) {
        std::uint32_t best_dist = std::numeric_limits<std::uint32_t>::max();
        for (const auto& m : matches) {
            // start row of buffer match in 0-based file coordinates.
            auto row = static_cast<std::uint32_t>(m.row_start);
            std::uint32_t dist = (row > line_hint) ? (row - line_hint)
                                                   : (line_hint - row);
            if (dist <= LINE_HINT_TOLERANCE && dist < best_dist) {
                best_dist = dist;
                pick = &m;
            }
        }
    }

    if (!pick) {
        // Ambiguous and no usable hint. Report match count so the caller
        // can emit a precise "appears N times at lines …" error.
        return {false, 0, 0, static_cast<int>(matches.size()), {}, 0};
    }

    // Compute the byte range. The match spans buffer rows [row_start, row_end].
    // If the needle ended without a trailing newline, drop the trailing '\n'
    // from the file range so the splice length stays consistent.
    const auto& start_line = fl[pick->row_start];
    const auto& end_line   = fl[pick->row_end];
    std::size_t pos = start_line.start;
    std::size_t end = end_line.end;
    bool needle_had_trailing_nl = !needle.empty() && needle.back() == '\n';
    if (!needle_had_trailing_nl && end > pos && file[end - 1] == '\n')
        --end;

    FuzzyMatch out{};
    out.ok = true;
    out.pos = pos;
    out.len = end - pos;
    out.count = 1;
    out.strategy = (pick->cost == 0) ? 1 : 2;

    // Indent fix-up. Only when caller actually supplied a replacement.
    if (!new_text.empty()) {
        auto d = detect_indent_delta(file, needle, fl,
                                     pick->row_start, pick->row_end + 1, nl);
        if (d.have && d.needle_base != d.file_base)
            out.adjusted_new_text = apply_indent_delta(new_text, d);
    }
    return out;
}

} // namespace mcp::tools::util
