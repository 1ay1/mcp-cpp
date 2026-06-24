// SPDX-License-Identifier: Apache-2.0
#pragma once
// Bash guards: reject interactive commands (vim, bare python REPL, etc.)
// and a handful of flagrantly destructive patterns. Returns empty string
// when acceptable, otherwise a human-readable rejection reason.

#include <string>
#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] std::string validate_bash_command(std::string_view cmd);

} // namespace mcp::tools::util
