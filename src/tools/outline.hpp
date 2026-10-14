// SPDX-License-Identifier: Apache-2.0
//
// outline.hpp — a source file's shape, from its text alone: the definition
// lines (functions, classes, headings) and the line range of one symbol.
// Shared by `read` (auto outline for big files, symbol=) and `outline`.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp::tools::detail::outline {

// Flat outline of a big file: "L12: void foo()" per definition.
[[nodiscard]] std::string render(std::string_view content);

// Nested outline: entries indented by their leading-whitespace depth.
// `total_defs` gets the count before the max_entries cut.
[[nodiscard]] std::string render_nested(std::string_view content,
                                        std::size_t max_entries, int& total_defs);

// The 1-based [start, end] lines of `symbol`'s definition and body. Sets
// *decl_only when only a declaration (no body) was found.
[[nodiscard]] std::optional<std::pair<int, int>>
symbol_range(std::string_view content, std::string_view symbol, bool* decl_only = nullptr);

}  // namespace mcp::tools::detail::outline
