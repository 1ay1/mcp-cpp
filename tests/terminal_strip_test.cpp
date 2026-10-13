// SPDX-License-Identifier: Apache-2.0
//
// terminal_strip_test.cpp — proves strip_terminal_controls applies real
// terminal line-discipline to captured subprocess output. (Running the
// child is the host's Exec; this is the library's half.)
//
// This is the regression test for the reported UI corruption: a bash
// child that thinks it owns a tty (cmake/ctest progress, ls --color,
// top -b) emits CSI/OSC sequences; the LIVE progress snapshots used to
// reach the tool card raw, so parameter bytes painted as literal glyphs
// ("\x1b[1;24r" → stray "r" cells) and were committed to native
// scrollback.

#include "agtest.hpp"

#include <mcp/tools/util/utf8.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace mcp::tools::util;

static int g_checks   = 0;


static bool has_controls(std::string_view s) {
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0x1b || c == '\r' || c == '\b'
            || (c < 0x20 && c != '\n' && c != '\t') || c == 0x7f)
            return true;
    }
    return false;
}

TEST_CASE("csi and osc removed") {
    CHECK(strip_terminal_controls("\x1b[1;31mred\x1b[0m plain") == "red plain",
          "SGR pair stripped");
    CHECK(strip_terminal_controls("\x1b[3;24r") == "",
          "DECSTBM stripped whole — no stray 'r'");
    CHECK(strip_terminal_controls("\x1b]0;title\x07text") == "text",
          "OSC-BEL stripped");
    CHECK(strip_terminal_controls("\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\") == "link",
          "OSC-ST hyperlink stripped");
    CHECK(strip_terminal_controls("\x1b[?1049h\x1b[2J\x1b[Hhome") == "home",
          "altscreen + clear + home stripped");
    CHECK(strip_terminal_controls("\x1bMup") == "up",
          "two-byte ESC pair stripped");
}

TEST_CASE("incomplete sequences dropped") {
    // The snapshot cadence can cut mid-CSI. The unfinished tail must be
    // DROPPED — passing "[1;24" through (or consuming only the ESC) is
    // exactly the stray-glyph bug.
    CHECK(strip_terminal_controls("ok\x1b[1;24") == "ok",
          "mid-CSI cut drops the partial sequence");
    CHECK(strip_terminal_controls("ok\x1b") == "ok",
          "dangling ESC dropped");
    CHECK(strip_terminal_controls("ok\x1b]0;half-open") == "ok",
          "unterminated OSC dropped");
}

TEST_CASE("cr overwrite semantics") {
    CHECK(strip_terminal_controls("12%\r34%\r100%\ndone") == "100%\ndone",
          "progress bar collapses to final state");
    CHECK(strip_terminal_controls("line\r\nnext") == "line\nnext",
          "CRLF normalises to LF");
    CHECK(strip_terminal_controls("tail\r") == "tail",
          "trailing CR is a no-op (next snapshot may continue the line)");
}

TEST_CASE("backspace and c0") {
    CHECK(strip_terminal_controls("abcd\b\bXY") == "abXY",
          "backspace erases previous chars");
    CHECK(strip_terminal_controls("caf\xc3\xa9\bX") == "cafX",
          "backspace erases a whole UTF-8 codepoint");
    CHECK(strip_terminal_controls("a\x01\x02\x03z") == "az",
          "other C0 bytes dropped");
    CHECK(strip_terminal_controls("keep\ttabs\nand newlines") ==
          "keep\ttabs\nand newlines", "tab + newline preserved");
}



