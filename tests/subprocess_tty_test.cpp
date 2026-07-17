// SPDX-License-Identifier: Apache-2.0
//
// subprocess_tty_test.cpp — proves the bash/subprocess runner detaches the
// child from the controlling terminal, so a tool that reaches PAST its std
// fds by opening /dev/tty directly (git progress meters, pager/credential
// prompts, coloured advice) can NOT scribble raw escapes onto the real
// terminal outside agentty's alt-screen. That off-band write never passed
// through clean_capture and was the reported "stray r r glyphs beside git
// add cards" leak.
//
// The mechanism under test: setsid (POSIX_SPAWN_SETSID on the posix_spawn
// path, ::setsid() in the fork/exec child). With no controlling tty in the
// child's fresh session, open("/dev/tty") fails ENXIO — verified here by
// having the child attempt exactly that and report the outcome on its
// (piped, captured) stdout.

#include <mcp/tools/util/subprocess.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using mcp::tools::util::run_command_s;

static int failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s\n", msg);                         \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

int main() {
    // The child tries to grab the controlling terminal for WRITE via an
    // exec redirect. In a session WITH a controlling tty this succeeds and
    // any bytes written land on the real terminal — the leak. In our
    // detached session open("/dev/tty") fails ENXIO, so the redirect fails
    // and we take the `||` branch. A stable sentinel is printed to the
    // (piped, captured) stdout either way; we never assert on exit code.
    const char* probe =
        "(exec 9>/dev/tty) 2>/dev/null && echo TTY_OPEN || echo TTY_ENXIO";

    auto r = run_command_s(probe, 64 * 1024, std::chrono::seconds{10});

    CHECK(r.started, "probe command started");
    CHECK(r.output.find("TTY_ENXIO") != std::string::npos,
          "child has NO controlling tty — /dev/tty write refused");
    CHECK(r.output.find("TTY_OPEN") == std::string::npos,
          "child did NOT reach the real terminal via /dev/tty");

    // Sanity: ordinary stdout still flows through the pipe + clean_capture.
    auto s = run_command_s("printf 'hello-stdout\\n'", 4096,
                           std::chrono::seconds{10});
    CHECK(s.output.find("hello-stdout") != std::string::npos,
          "normal stdout capture still works");

    if (failures == 0) std::puts("all subprocess-tty tests passed");
    return failures ? 1 : 0;
}
