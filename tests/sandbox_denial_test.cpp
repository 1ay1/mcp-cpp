// A sandboxed denial must explain itself — and stay quiet otherwise.
//
// When the sandbox blocks something, the tool sees whatever the child
// printed. Under bwrap that is NOT "Permission denied": bwrap declines to
// bind the path, so it is simply absent and the child says
//
//     cat: /etc/shadow: No such file or directory
//
// which a model reads as "this file does not exist on this machine" — and
// then stops, or tries to create it. The sandbox knew the real answer and was
// throwing it away, so the model burned turns against a boundary it could not
// see.
//
// The annotation is only useful if it is nearly always right. A note on every
// failure teaches the model to skip it, so the two halves of this test matter
// equally: it fires on a real denial, and it stays silent on an ordinary typo.

#include <mcp/tools/util/sandbox.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace sb = mcp::tools::util::sandbox;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {
bool annotated(std::string_view cmd) {
    auto r = sb::run_shell_command(cmd, 32768, std::chrono::seconds{20});
    return r.output.find("[sandbox]") != std::string::npos;
}
}  // namespace

int main() {
    auto root = fs::temp_directory_path()
              / ("sandbox_denial_" + std::to_string(::getpid()));
    fs::create_directories(root);
    mcp::tools::util::set_workspace_root(root);
    (void)sb::init(sb::Mode::Auto);

    if (!sb::is_active()) {
        // No backend on this host (CI container without user namespaces).
        // Skipping is correct — asserting sandbox behaviour with no sandbox
        // would be asserting nothing.
        std::puts("sandbox_denial_test: SKIP (no backend available)");
        std::error_code ec; fs::remove_all(root, ec);
        return 0;
    }

    // ── fires on a real denial ──────────────────────────────────────────
    // An absolute path outside the grant, which the sandbox made invisible.
    CHECK(annotated("cat /etc/shadow"));

    // ── silent on everything that is NOT us ─────────────────────────────
    // A typo in the workspace. An earlier version scanned for any '/' and
    // annotated these, because `./missing.txt` contains one — the precise
    // noise that would train a model to ignore the note.
    CHECK(!annotated("cat ./definitely-not-here.txt"));
    CHECK(!annotated("cat nope/inner.txt"));
    // A failure with no path in it at all.
    CHECK(!annotated("exit 3"));
    // A search that simply found nothing.
    CHECK(!annotated("grep -r zzzzzz . ; exit 1"));

    // ── silent on success, however noisy ────────────────────────────────
    // Exit 0 is never annotated: a command that worked has nothing to
    // explain, even if it printed the word "denied" along the way.
    CHECK(!annotated("echo 'Permission denied' ; true"));
    CHECK(!annotated("touch ./ok && echo wrote"));

    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::puts("sandbox_denial_test: OK");
    return g_failures == 0 ? 0 : 1;
}
