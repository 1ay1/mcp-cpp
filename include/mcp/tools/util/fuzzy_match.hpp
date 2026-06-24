// SPDX-License-Identifier: Apache-2.0
#pragma once
// Line-DP fuzzy matching for the edit tool, ported from Zed's
// StreamingFuzzyMatcher. Treats `old_text` as a fuzzy line query and finds
// the file region with minimum edit distance under a line-level cost model
// (typo-tolerant; whitespace/indent drift is free).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcp::tools::util {

struct FuzzyMatch {
    bool        ok;      // true iff exactly one usable match
    std::size_t pos;     // byte offset into file (0 if !ok)
    std::size_t len;     // bytes of file that correspond to `needle`
    int         count;   // total matches seen (1 on ok; >1 means ambiguous)
    std::string adjusted_new_text;  // re-indented replacement, or empty
    int         strategy = 0;       // 0=none, 1=exact, 2=DP (diagnostics)
};

FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle);

FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text);

FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text,
                      std::uint32_t    line_hint);

} // namespace mcp::tools::util
