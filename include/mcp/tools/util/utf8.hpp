// SPDX-License-Identifier: Apache-2.0
#pragma once
// UTF-8 validation + repair. Subprocess output on Windows is whatever code
// page cmd.exe/PowerShell picked (usually OEM/CP1252), but nlohmann::json
// throws type_error.316 on any non-UTF-8 byte. Every string we hand the API
// must pass through to_valid_utf8 at the capture boundary.

#include <string>
#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] bool is_valid_utf8(std::string_view s) noexcept;
[[nodiscard]] std::string sanitize_utf8(std::string_view s);
[[nodiscard]] std::string to_valid_utf8(std::string s);
[[nodiscard]] std::size_t safe_utf8_cut(std::string_view s, std::size_t max_bytes) noexcept;

} // namespace mcp::tools::util
