// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/util/error.hpp — self-contained tool error type for the
// batteries-included toolset's util layer. Mirrors the shape agentty's
// tool layer used (ErrorKind + ToolError + factories + render) so the
// ported util/tool bodies transcribe verbatim, but lives entirely inside
// mcp-cpp so the util layer has NO dependency on any host's types.

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <mcp/tools/toolset.hpp>   // mcp::tools::FileChange

namespace mcp::tools::util {

// Typed error kind. Lets a host color / retry / suggest based on category
// rather than string-matching `detail`.
enum class ErrorKind : std::uint8_t {
    InvalidArgs,    // schema/validation failure (missing field, empty string, out of range)
    NotFound,       // file/dir/symbol doesn't exist
    NotAFile,       // exists but isn't a regular file
    NotADirectory,  // exists but isn't a directory
    TooLarge,       // input exceeded a size cap (read's 1 MiB, etc.)
    Binary,         // refused to treat a binary file as text
    Ambiguous,      // multiple matches where one was required (edit's old_string)
    NoMatch,        // pattern matched nothing (edit's old_string, grep)
    InvalidRegex,   // regex didn't compile
    Network,        // curl / HTTP transport failure
    Spawn,          // child process failed to start
    Subprocess,     // subprocess returned non-zero
    Io,             // generic I/O (write_file failed, etc.)
    OutOfWorkspace, // path is outside the configured workspace root
    Unknown,        // uncaught exception / unknown tool
};

[[nodiscard]] std::string_view to_string(ErrorKind k) noexcept;

struct ToolError {
    ErrorKind   kind = ErrorKind::Unknown;
    std::string detail;

    [[nodiscard]] static ToolError invalid_args(std::string d)     noexcept { return {ErrorKind::InvalidArgs,    std::move(d)}; }
    [[nodiscard]] static ToolError not_found(std::string d)        noexcept { return {ErrorKind::NotFound,       std::move(d)}; }
    [[nodiscard]] static ToolError not_a_file(std::string d)       noexcept { return {ErrorKind::NotAFile,       std::move(d)}; }
    [[nodiscard]] static ToolError not_a_directory(std::string d)  noexcept { return {ErrorKind::NotADirectory,  std::move(d)}; }
    [[nodiscard]] static ToolError too_large(std::string d)        noexcept { return {ErrorKind::TooLarge,       std::move(d)}; }
    [[nodiscard]] static ToolError binary(std::string d)           noexcept { return {ErrorKind::Binary,         std::move(d)}; }
    [[nodiscard]] static ToolError ambiguous(std::string d)        noexcept { return {ErrorKind::Ambiguous,      std::move(d)}; }
    [[nodiscard]] static ToolError no_match(std::string d)         noexcept { return {ErrorKind::NoMatch,        std::move(d)}; }
    [[nodiscard]] static ToolError invalid_regex(std::string d)    noexcept { return {ErrorKind::InvalidRegex,   std::move(d)}; }
    [[nodiscard]] static ToolError network(std::string d)          noexcept { return {ErrorKind::Network,        std::move(d)}; }
    [[nodiscard]] static ToolError spawn(std::string d)            noexcept { return {ErrorKind::Spawn,          std::move(d)}; }
    [[nodiscard]] static ToolError subprocess(std::string d)       noexcept { return {ErrorKind::Subprocess,     std::move(d)}; }
    [[nodiscard]] static ToolError io(std::string d)               noexcept { return {ErrorKind::Io,             std::move(d)}; }
    [[nodiscard]] static ToolError out_of_workspace(std::string d) noexcept { return {ErrorKind::OutOfWorkspace, std::move(d)}; }
    [[nodiscard]] static ToolError unknown(std::string d)          noexcept { return {ErrorKind::Unknown,        std::move(d)}; }

    // "[not found] path/to/file" — the default stringification.
    [[nodiscard]] std::string render() const;
};

// The output a tool body produces: text plus an optional file mutation a
// write/edit emitted (carried back to the host's diff-review UI via meta).
struct ToolOutput {
    std::string                            text;
    std::optional<mcp::tools::FileChange>  change;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

} // namespace mcp::tools::util
