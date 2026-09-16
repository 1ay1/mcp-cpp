// A spawned tool must not inherit the host's file descriptors.
//
// Access rights attach to the open file DESCRIPTION, not to the path — true
// of Landlock and Seatbelt alike. So a descriptor the parent opened before
// confinement keeps working after it and survives exec, and ONE leaked fd
// voids the whole filesystem policy: the child can read a file it was denied,
// and (measured below) even one that no longer has a path at all.
//
// agentty is exactly the program this hurts. It holds credential stores,
// session files and logs open while spawning tools it does not trust.
// Per-callsite O_CLOEXEC is not an answer — it is a convention that holds
// until one caller forgets, and a sandbox that is only safe when every caller
// is careful is not a boundary. (bastion DESIGN.md §6.1 measures the same
// hole from the sandbox side; this asserts the host side.)
//
// The unlinked case is the sharp one: no path exists, no sandbox rule can
// name it, and the child still reads the bytes if the descriptor survives.

#include <mcp/tools/util/subprocess.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using mcp::tools::util::run_command_s;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    const auto dir = fs::temp_directory_path()
                   / ("fd_leak_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto secret = dir / "secret.txt";
    {
        std::FILE* f = std::fopen(secret.c_str(), "w");
        if (!f) { std::puts("setup failed"); return 2; }
        std::fputs("FLAG{fd-inherited}\n", f);
        std::fclose(f);
    }

    // Opened BEFORE the spawn and deliberately WITHOUT O_CLOEXEC — standing
    // in for a long-lived host that holds its credential store open, and for
    // any library fd we do not control.
    const int fd = ::open(secret.c_str(), O_RDONLY);
    CHECK(fd >= 0);

    // ── 1. The descriptor must not be readable by number ────────────────
    {
        const std::string cmd =
            "cat /proc/self/fd/" + std::to_string(fd) + " 2>/dev/null; true";
        auto r = run_command_s(cmd, 64u * 1024, std::chrono::seconds{10});
        CHECK(r.started);
        const bool leaked = r.output.find("FLAG{fd-inherited}") != std::string::npos;
        if (leaked)
            std::fprintf(stderr, "FD_LEAK: child read the inherited fd\n");
        CHECK(!leaked);
    }

    // ── 2. …not even when the file has no path at all ───────────────────
    // The sharp case: after unlink() there is nothing for a path-based
    // policy to deny, so the descriptor IS the entire access grant.
    {
        fs::remove(secret);
        const std::string cmd =
            "cat /proc/self/fd/" + std::to_string(fd) + " 2>/dev/null; true";
        auto r = run_command_s(cmd, 64u * 1024, std::chrono::seconds{10});
        CHECK(r.started);
        const bool leaked = r.output.find("FLAG{fd-inherited}") != std::string::npos;
        if (leaked)
            std::fprintf(stderr, "FD_LEAK: child read an UNLINKED file via fd\n");
        CHECK(!leaked);
    }

    // ── 3. The child's own three descriptors still work ─────────────────
    // A sweep that also closed 0/1/2 would "pass" the checks above by
    // breaking every tool. stdout must still reach us, and stdin must still
    // be the /dev/null the spawn wired up (reading it yields EOF, not a
    // hang and not the terminal).
    {
        auto r = run_command_s("echo alive; cat; echo done", 64u * 1024, std::chrono::seconds{10});
        CHECK(r.started);
        CHECK(r.output.find("alive") != std::string::npos);
        CHECK(r.output.find("done")  != std::string::npos);
    }

    if (fd >= 0) ::close(fd);
    std::error_code ec;
    fs::remove_all(dir, ec);

    if (g_failures == 0) std::puts("fd_leak_test: OK");
    return g_failures == 0 ? 0 : 1;
}
