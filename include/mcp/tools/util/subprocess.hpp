#pragma once
// Cross-platform subprocess runner.
//
// Why a class-with-Options instead of free functions: we already had three
// overlapping runners (shell / argv / legacy-string-wrapper) each with their
// own truncation / timeout / progress logic. Consolidating into one entry
// point lets every tool pick the knobs it cares about without duplicating
// 200 lines of Win32 pipe plumbing per call site.
//
// Platform specifics:
//   Windows → CreateProcessW with stdin redirected to NUL (prevents the
//             child from stealing keystrokes) and stdin's console mode saved
//             + restored (prevents a child resetting ENABLE_LINE_INPUT from
//             corrupting TUI input). Reader thread drains the pipe so a
//             grandchild that inherits stdout can't deadlock the wait.
//   POSIX   → posix_spawn + poll-based deadline. Shell form goes through
//             /bin/sh -c; argv form execs directly. Timeouts are enforced
//             in-process via SIGTERM (with a 2 s grace) → SIGKILL — no
//             dependency on GNU coreutils `timeout`, which isn't on stock
//             macOS. stdin redirected from /dev/null; stdout+stderr both
//             dup2'd onto a single pipe so callers see merged output.
//
// Both paths stream captured bytes through the thread-local progress sink
// (see agentty/tool/registry.hpp) at most every ~80 ms, so the UI reveals live
// output without flooding the event queue.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::tools::util {

struct SubprocessOptions {
    // Exactly one of `shell_command` / `argv` must be set. Shell form goes
    // through cmd.exe/sh and gets its quoting rules; argv form is exec'd
    // directly so paths, refs, commit messages, and format strings survive
    // intact.
    std::optional<std::string>              shell_command = std::nullopt;
    std::optional<std::vector<std::string>> argv = std::nullopt;

    // Working directory for the child. Empty ⇒ inherit the parent's cwd. Set
    // via a real chdir in the child (posix_spawn file-action / pre-exec, or
    // CreateProcess lpCurrentDirectory on Windows) rather than a `cd &&`
    // string prefix, so the path can't be re-parsed or mangled by the shell.
    std::string cwd;

    // Extra environment variables for the child, layered on top of the
    // parent env (later entries win). Used both for the sane non-interactive
    // defaults the tool injects (NO_COLOR, PAGER=cat, GIT_TERMINAL_PROMPT=0,
    // …) and for caller/model-supplied vars. Empty ⇒ inherit env unchanged.
    std::vector<std::pair<std::string, std::string>> env;

    // `timeout` is an IDLE watchdog: it fires only after this many seconds
    // of *silence*, so a chatty build/test that keeps printing progress is
    // never killed mid-flight. `hard_timeout` is an absolute wall-clock
    // ceiling from spawn that NEVER resets — it exists solely to reap a
    // runaway that stays chatty forever (`yes`, `tail -f`, an infinite
    // progress loop) which the idle watchdog alone can never catch. 0 ⇒
    // derive a generous default (see run_posix) so existing callers that
    // only set `timeout` still get a real ceiling.
    std::chrono::seconds timeout{120};
    std::chrono::seconds hard_timeout{0};
    // Unsigned because a negative cap makes no sense and every compare
    // site was already a `size_t` on the RHS; the old `int` caused
    // mixed-sign promotions and the occasional sign-compare warning.
    std::size_t          max_bytes = 30'000;

    // Called with the full accumulated buffer (not a delta) on a best-effort
    // throttle. Passing the whole buffer each time means multi-byte UTF-8
    // sequences that span pipe reads still render correctly on the next
    // flush — no need for delta-aware splitting on the caller side.
    std::function<void(std::string_view snapshot)> on_progress = nullptr;
    std::function<bool()> cancelled = nullptr;
};

struct SubprocessResult {
    std::string output;                // captured stdout+stderr, UTF-8 valid
    int  exit_code   = 0;
    bool timed_out   = false;
    bool cancelled   = false;
    bool truncated   = false;
    bool started     = true;           // false iff spawn itself failed
    std::string start_error;           // populated when started==false
};

struct Subprocess {
    [[nodiscard]] static SubprocessResult run(SubprocessOptions opts);
};

// ── Convenience wrappers around Subprocess::run ─────────────────────────
//
// `run_command_s` takes a shell string, `run_argv_s` takes a pre-built argv
// (no shell). The `_s` suffix is a throwback to the pre-refactor era where
// the non-`_s` versions returned the "legacy_format" suffixed string shape;
// kept for call-site grep-ability.

[[nodiscard]] SubprocessResult run_command_s(
    const std::string& cmd,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120},
    std::string_view     cwd       = {},
    const std::vector<std::pair<std::string, std::string>>& env = {});

[[nodiscard]] SubprocessResult run_argv_s(
    const std::vector<std::string>& argv,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

// Flatten a SubprocessResult into the legacy suffix-marker string shape:
//   <output>[\n[output truncated]][\n[timed out after Xs] | \n[exit code N]]
// Tools that parse exit codes out of their captured blob (e.g. git_commit's
// `out.find("[exit code")` guard) depend on this format — don't change it
// without auditing every caller.
[[nodiscard]] std::string legacy_format(const SubprocessResult& r,
                                        std::chrono::seconds timeout);

[[nodiscard]] std::string run_command(
    const std::string& cmd,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

[[nodiscard]] std::string run_argv(
    const std::vector<std::string>& argv,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

} // namespace mcp::tools::util
