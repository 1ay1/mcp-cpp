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

// Standard base64 (RFC 4648 alphabet, '+' '/', '=' padding). Used to carry raw
// image bytes through the structured-meta JSON channel to the host. Distinct
// from mcp::b64url_* (URL-safe alphabet) elsewhere — image data wants the
// canonical alphabet the host's own decoders expect.
inline std::string b64_encode(std::string_view in) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(in[i]) << 16)
                         | (static_cast<unsigned char>(in[i + 1]) << 8)
                         |  static_cast<unsigned char>(in[i + 2]);
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);
        out.push_back(kAlpha[n & 63]);
    }
    if (const std::size_t rem = in.size() - i; rem == 1) {
        const unsigned n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const unsigned n = (static_cast<unsigned char>(in[i]) << 16)
                         | (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// Inverse of b64_encode. Ignores whitespace; returns empty on malformed input.
inline std::string b64_decode(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) return {};
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

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

// An image a tool surfaces to a vision-capable model (e.g. `read` on a PNG).
// mcp-cpp has no dependency on the host's Message model, so this is a minimal
// self-contained carrier the host maps onto its own ImageContent. Bytes are
// RAW (not base64); the host/wire encodes at the JSON boundary.
struct ToolImage {
    std::string media_type;   // "image/png", "image/jpeg", "image/webp", "image/gif"
    std::string bytes;        // raw image bytes
};

// The output a tool body produces: text plus an optional file mutation a
// write/edit emitted (carried back to the host's diff-review UI via meta).
struct ToolOutput {
    std::string                            text;
    std::optional<mcp::tools::FileChange>  change;
    // Multi-file tools (replace) emit ONE FileChange per written file here;
    // single-file tools use `change` above. Both flow to the host diff-review.
    std::vector<mcp::tools::FileChange>    changes;
    // Images a tool wants a vision model to actually SEE (read on an image
    // file). The host maps these onto the turn's tool_result image blocks.
    std::vector<ToolImage>                 images;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

} // namespace mcp::tools::util
