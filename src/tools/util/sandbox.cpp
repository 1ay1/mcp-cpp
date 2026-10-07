#include <mcp/tools/util/sandbox.hpp>

#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/progress.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace mcp::tools::util::sandbox {

namespace fs = std::filesystem;

namespace {

// Single-process state. Set by init() at startup, read by every bash
// call afterwards. Atomics aren't strictly needed (set once, never
// flipped after main has handed off to maya), but they cost nothing
// and document the read-many lifecycle.
std::atomic<Mode>    g_mode{Mode::Auto};
std::atomic<Backend> g_backend{Backend::None};

// Host-installed sandbox (see sandbox.hpp). Plain globals: set once at
// startup, read on every tool run, never mutated concurrently.
HostSandbox& host_sandbox() { static HostSandbox hs; return hs; }

// No backend of its own, on any platform. The embedding application
// supplies the boundary (HostServices::exec) and tells us about it
// (set_host_sandbox), which is why everything that used to live here --
// three platform arms, a bwrap argv builder, a seatbelt profile generator,
// a denial annotator and two run paths -- is gone rather than ported.
[[nodiscard]] Backend probe() { return Backend::None; }

}  // namespace

bool init(Mode requested) {
    g_mode.store(requested, std::memory_order_release);
    auto found = (requested == Mode::Off) ? Backend::None : probe();
    g_backend.store(found, std::memory_order_release);
    if (requested == Mode::On && found == Backend::None) {
        // Strict mode + no backend = init failure. Caller decides
        // whether to abort startup or downgrade silently.
        return false;
    }
    return true;
}

void set_host_sandbox(HostSandbox hs) { host_sandbox() = std::move(hs); }
bool has_host_sandbox() noexcept { return host_sandbox().active; }

Mode    requested_mode()   noexcept { return g_mode.load(std::memory_order_acquire); }
Backend detected_backend() noexcept { return g_backend.load(std::memory_order_acquire); }

bool is_active() noexcept {
    if (requested_mode() == Mode::Off) return false;
    // A host sandbox IS a sandbox, even when our own probe found no backend:
    // the host may have an engine this library cannot reach on its own.
    return has_host_sandbox() || detected_backend() != Backend::None;
}

std::string describe_state() {
    auto m = requested_mode();
    auto b = detected_backend();
    if (m == Mode::Off) return "sandbox: off";
    // Name what will actually run the command.
    if (has_host_sandbox() && !host_sandbox().label.empty())
        return "sandbox: active (" + host_sandbox().label + ")";
    const char* tag = nullptr;
    switch (b) {
        case Backend::SandboxExec: tag = "sandbox-exec"; break;
        case Backend::None:        tag = nullptr;        break;
    }
    if (tag) {
        // --workspace / rw-binds the whole filesystem: still wrapped,
        // but no filesystem containment. Be honest about it.
        std::error_code wec;
        auto ws = fs::weakly_canonical(workspace_root(), wec);
        if (wec) ws = workspace_root();
        if (ws == ws.root_path())
            return std::string{"sandbox: degraded ("} + tag
                 + ", --workspace / gives no filesystem containment)";
        return std::string{"sandbox: active ("} + tag + ")";
    }
    if (m == Mode::On)
        return "sandbox: requested but no backend "
#if defined(__linux__)
               "(no host sandbox was installed \xe2\x80\x94 the embedding "
               "application supplies the boundary on this platform)";
#elif defined(__APPLE__)
               "(sandbox-exec missing \xe2\x80\x94 system integrity issue)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if defined(__linux__)
           "(no host sandbox was installed)";
#elif defined(__APPLE__)
           "(sandbox-exec missing)";
#else
           "(no backend on this platform)";
#endif
}

} // namespace mcp::tools::util::sandbox
