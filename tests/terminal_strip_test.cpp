// SPDX-License-Identifier: Apache-2.0
//
// terminal_strip_test.cpp — proves strip_terminal_controls applies real
// terminal line-discipline to captured subprocess output, and that the
// bash/subprocess capture boundary delivers CLEAN bytes on both the live
// progress path and the final output.
//
// This is the regression test for the reported UI corruption: a bash
// child that thinks it owns a tty (cmake/ctest progress, ls --color,
// top -b) emits CSI/OSC sequences; the LIVE progress snapshots used to
// reach the tool card raw, so parameter bytes painted as literal glyphs
// ("\x1b[1;24r" → stray "r" cells) and were committed to native
// scrollback. Every path out of the runners must now be escape-free.

#include "agtest.hpp"

#include <mcp/tools/util/subprocess.hpp>
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

#ifndef _WIN32
TEST_CASE("live progress path is clean") {
    // A child that emits SGR + CR progress + a DECSTBM probe. Both the
    // live snapshots and the final output must be control-free.
    SubprocessOptions opts;
    opts.shell_command =
        "printf 'step 1\\r'; printf 'step 2\\n'; "
        "printf '\\033[1;32mgreen\\033[0m\\n'; "
        "printf '\\033[3;24r'; printf 'after-region\\n'";
    opts.timeout   = std::chrono::seconds{10};
    opts.max_bytes = 1 << 20;
    std::vector<std::string> snaps;
    opts.on_progress = [&](std::string_view s) { snaps.emplace_back(s); };

    auto r = Subprocess::run(std::move(opts));
    CHECK(r.started, "child started");
    CHECK(r.exit_code == 0, "child exited 0");
    CHECK(!has_controls(r.output), "final output is control-free");
    CHECK(r.output.find("green") != std::string::npos, "SGR text kept");
    CHECK(r.output.find("after-region") != std::string::npos,
          "post-DECSTBM text kept");
    CHECK(r.output.find('r') == std::string::npos
              || r.output.find("region") != std::string::npos,
          "no stray 'r' outside real words");
    CHECK(r.output.find("step 2") != std::string::npos, "CR overwrite kept final");
    bool all_snaps_clean = true;
    for (const auto& s : snaps)
        if (has_controls(s)) all_snaps_clean = false;
    CHECK(all_snaps_clean, "every live progress snapshot is control-free");
}
#endif

