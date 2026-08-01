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

#include <mcp/cap/process.hpp>
#include <mcp/tools/util/subprocess.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <unistd.h>

using mcp::tools::util::Subprocess;
using mcp::tools::util::SubprocessOptions;
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

    // Cancellation must target the session, not just its shell leader. Both
    // processes ignore TERM, forcing the 2-second KILL escalation. The
    // descendant inherits stdout, reproducing the pipe-holder hang.
    SubprocessOptions cancelled;
    cancelled.shell_command =
        "trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & "
        "echo DESCENDANT:$!; wait";
    cancelled.timeout = std::chrono::seconds{30};
    const auto cancel_start = std::chrono::steady_clock::now();
    cancelled.cancelled = [cancel_start] {
        return std::chrono::steady_clock::now() - cancel_start >
               std::chrono::milliseconds{100};
    };
    auto killed = Subprocess::run(std::move(cancelled));
    const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_start;
    CHECK(killed.cancelled, "cancellation reported");
    CHECK(cancel_elapsed < std::chrono::seconds{5},
          "cancellation is bounded despite TERM-resistant descendant");
    const auto marker = killed.output.find("DESCENDANT:");
    CHECK(marker != std::string::npos, "descendant pid captured");
    if (marker != std::string::npos) {
        const pid_t descendant = static_cast<pid_t>(std::strtol(
            killed.output.c_str() + marker + 11, nullptr, 10));
        errno = 0;
        CHECK(descendant > 0 && ::kill(descendant, 0) < 0 && errno == ESRCH,
              "cancellation killed descendant process");
    }

    // Exercise the long-running process primitive used by process_stop.
    // terminate() must escalate for a TERM-resistant tree and
    // interrupt_output() must safely wake the blocked iostream reader.
    mcp::cap::ChildProcess::Spawn spawn;
    spawn.command = "/bin/sh";
    spawn.args = {"-c", "trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & "
                         "echo $!; wait"};
    spawn.merge_stderr = true;
    mcp::cap::ChildProcess child{spawn};
    pid_t held_descendant = -1;
    child.out() >> held_descendant;
    std::thread reader([&] {
        char byte = 0;
        child.out().get(byte); // blocks: descendant deliberately retains stdout
    });
    const auto stop_start = std::chrono::steady_clock::now();
    child.terminate();
    child.interrupt_output();
    reader.join();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
    CHECK(stop_elapsed < std::chrono::seconds{5},
          "process stop primitive is bounded");
    errno = 0;
    CHECK(held_descendant > 0 && ::kill(held_descendant, 0) < 0 && errno == ESRCH,
          "process stop killed stdout-holding descendant");

    if (failures == 0) std::puts("all subprocess-tty tests passed");
    return failures ? 1 : 0;
}
