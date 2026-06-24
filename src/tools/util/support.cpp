// SPDX-License-Identifier: Apache-2.0
//
// error.cpp / progress.cpp — out-of-line bits of the util support layer.

#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/progress.hpp>

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
    return std::format("[{}] {}", to_string(kind), detail);
}

} // namespace mcp::tools::util

namespace mcp::tools::util::progress {
namespace {
    thread_local Sink g_sink;
}
void set(Sink s)                     { g_sink = std::move(s); }
void clear()                         { g_sink = nullptr; }
void emit(std::string_view snapshot) { if (g_sink) g_sink(snapshot); }
} // namespace mcp::tools::util::progress
