// SPDX-License-Identifier: Apache-2.0
#pragma once
// Bash guards: reject interactive commands (vim, bare python REPL, etc.)
// and a handful of flagrantly destructive patterns. Returns empty string
// when acceptable, otherwise a human-readable rejection reason.

#include <string>
#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] std::string validate_bash_command(std::string_view cmd);

// Soft, NON-blocking advisory: when a command is a bare file-inspection
// shell-out (cat / sed / head / tail / grep / find / ls / wc) that a smart
// native tool does better, return a one-line nudge toward that tool (read /
// read symbol= / grep / glob / list_dir). Returns "" when the command is fine
// as-is — including any command with a pipe/redirect, where the shell tool is
// legitimately the right call. The bash tool prepends this to its output; it
// NEVER blocks execution.
[[nodiscard]] std::string bash_tool_suggestion(std::string_view cmd);

} // namespace mcp::tools::util
