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

// Apply terminal line-discipline to captured subprocess output so raw
// control bytes can never reach a UI cell grid or the model:
//   • ESC sequences are consumed whole — CSI through its final byte
//     (0x40–0x7E), OSC/DCS/SOS/PM/APC through BEL or ST, two-byte ESC+X
//     pairs. A sequence left INCOMPLETE at the end of the buffer (a
//     progress snapshot can cut mid-CSI) is dropped, not passed through
//     as literal parameter bytes — "\x1b[1;24" must not surface as
//     "[1;24" (or, ESC-swallowed, as a stray "r" once the final byte
//     arrives in the next snapshot).
//   • lone \r applies OVERWRITE semantics: the current output line is
//     erased back to its start, exactly what a terminal does with
//     progress bars ("12%\r13%\r…\r100%" collapses to "100%").
//     \r\n normalises to \n.
//   • \b erases the previous codepoint on the current line.
//   • every other C0 byte and DEL is dropped; \n and \t are kept.
// UTF-8 is passed through untouched (the function is byte-transparent
// outside control sequences); run to_valid_utf8 after this for the
// encoding guarantee.
[[nodiscard]] std::string strip_terminal_controls(std::string_view in);

} // namespace mcp::tools::util
