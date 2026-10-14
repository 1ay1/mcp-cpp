// SPDX-License-Identifier: Apache-2.0
//
// error.cpp / progress.cpp — out-of-line bits of the util support layer.

#include <mcp/tools/util/error.hpp>

#include <format>

namespace mcp::tools::util {

std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::InvalidArgs:    return "invalid args";
        case ErrorKind::NotFound:       return "not found";
        case ErrorKind::NotAFile:       return "not a file";
        case ErrorKind::NotADirectory:  return "not a directory";
        case ErrorKind::TooLarge:       return "too large";
        case ErrorKind::Binary:         return "binary";
        case ErrorKind::Ambiguous:      return "ambiguous";
        case ErrorKind::NoMatch:        return "no match";
        case ErrorKind::InvalidRegex:   return "invalid regex";
        case ErrorKind::Network:        return "network";
        case ErrorKind::Spawn:          return "spawn failed";
        case ErrorKind::Subprocess:     return "subprocess failed";
        case ErrorKind::Io:             return "io";
        case ErrorKind::OutOfWorkspace: return "out of workspace";
        case ErrorKind::Unknown:        return "unknown";
    }
    return "unknown";
}

std::string ToolError::render() const {
    // An error usually quotes an argument back ("file not found: <path>").
    // A 200 KB argument made a 200 KB error. Keep head and tail.
    constexpr std::size_t kMax = 4096, kKeep = 1500;
    if (detail.size() <= kMax) return std::format("[{}] {}", to_string(kind), detail);
    std::size_t head = kKeep, tail = detail.size() - kKeep;
    // don't cut inside a UTF-8 sequence
    while (head > 0 && (static_cast<unsigned char>(detail[head]) & 0xC0) == 0x80) --head;
    while (tail < detail.size() && (static_cast<unsigned char>(detail[tail]) & 0xC0) == 0x80) ++tail;
    return std::format("[{}] {}\n[... {} bytes elided ...]\n{}", to_string(kind),
                       std::string_view{detail}.substr(0, head), tail - head,
                       std::string_view{detail}.substr(tail));
}

} // namespace mcp::tools::util
