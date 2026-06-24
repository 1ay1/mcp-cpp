// SPDX-License-Identifier: Apache-2.0
#pragma once
// fnmatch-style glob matcher. Supports `*` (any run, incl. empty),
// `?` (any single char), `[abc]` / `[a-z]` / `[!abc]` character classes.
// Case-insensitive on Windows, case-sensitive elsewhere. `**` collapses to
// `*` (no path-spanning); callers match against just the filename segment.

#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view name) noexcept;

} // namespace mcp::tools::util
