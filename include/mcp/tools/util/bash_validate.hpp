// SPDX-License-Identifier: Apache-2.0
#pragma once
// The text-level bash gate: refuses interactive commands (vim, a bare python
// REPL, ...) and flagrantly destructive ones (rm -rf /, curl | sh). Returns
// "" when the command is fine, else the reason. shell.cpp runs it when the
// shellx parse isn't clean, so a script it can't read still gets checked.
//
// Detour advice (cat -> read, etc.) is in shell_detour.hpp.

#include <string>
#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] std::string validate_bash_command(std::string_view cmd);

} // namespace mcp::tools::util
