// SPDX-License-Identifier: Apache-2.0
//
// tool_body.cpp — make_change: build a FileChange from before/after using the
// internal Myers diff engine for accurate added/removed counts.

#include "tool_body.hpp"
#include "diff.hpp"

#include <string>
#include <utility>

namespace mcp::tools::detail {

FileChange make_change(std::string path, std::string before, std::string after) {
    auto d = diff::compute(path, before, after);
    FileChange fc;
    fc.path    = std::move(path);
    fc.added   = d.added;
    fc.removed = d.removed;
    fc.before  = std::move(before);
    fc.after   = std::move(after);
    return fc;
}

} // namespace mcp::tools::detail
