// SPDX-License-Identifier: Apache-2.0
//
// diff.hpp — INTERNAL. A small self-contained unified-diff engine for the
// edit/write tools' inline diff output and FileChange line counts. Ported
// from agentty's diff domain (Myers-style LCS hunking). No I/O.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcp::tools::detail::diff {

struct Hunk {
    int old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string patch;
};

struct Diff {
    std::string path;
    int added   = 0;
    int removed = 0;
    std::vector<Hunk> hunks;
    std::string before;
    std::string after;
};

[[nodiscard]] Diff compute(const std::string& path,
                           const std::string& before,
                           const std::string& after);

[[nodiscard]] std::string render_unified(const Diff& d);

} // namespace mcp::tools::detail::diff
