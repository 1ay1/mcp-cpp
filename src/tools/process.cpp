// SPDX-License-Identifier: Apache-2.0
// Long-running process sessions for dev servers, watchers, and log tails.
//
// Ownership: the host's Session owns the process, its output and the read
// cursor, and is thread-safe by contract (host.hpp). The tool state holds
// only a map from id to an immutable entry. So every with() body here is a
// map operation, and nothing about a process is ever touched under the
// state's lock: a tool takes the entry out and talks to its Session after.
// Two calls on one id can't interleave into a bad state, because there is
// no shared mutable state for them to interleave on.

#include "tool_shell.hpp"
#include "call_state.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;

namespace {

constexpr std::size_t kRollingBytes = 128 * 1024;
constexpr std::size_t kMaxSessions  = 32;

using Entry = std::shared_ptr<const state::Proc>;

std::optional<int> exit_code_of(const std::optional<ExecOutcome>& o) {
    if (!o) return std::nullopt;
    return std::visit([]<class T>(const T& v) -> std::optional<int> {
        if constexpr (std::is_same_v<T, Exited>)           return v.code;
        else if constexpr (std::is_same_v<T, Signalled>)   return 128 + v.signal;
        else if constexpr (std::is_same_v<T, StartFailed>) return 127;
        else                                               return std::nullopt;
    }, *o);
}

Entry find_entry(const Call& call, const std::string& id) {
    return call.with([&](ToolState& s) -> Entry {
        auto it = s.procs.find(id);
        return it == s.procs.end() ? nullptr : it->second;
    });
}

Entry take_entry(const Call& call, const std::string& id) {
    return call.with([&](ToolState& s) -> Entry {
        auto it = s.procs.find(id);
        if (it == s.procs.end()) return nullptr;
        auto e = std::move(it->second);
        s.procs.erase(it);
        return e;
    });
}

// " (live sessions: …)" for recovery hints.
std::string live_session_hint(const Call& call) {
    return call.with([](ToolState& s) {
        if (s.procs.empty()) return std::string{" (no live sessions)"};
        std::string out = " (live sessions:";
        for (const auto& [id, _] : s.procs) out += " " + id;
        return out + ")";
    });
}

// Read for up to `wait`, in slices so a cancelled call stops waiting.
//   * stops on the end of the program;
//   * with `until_end`, keeps reading until then (stop, settle window);
//   * otherwise returns on output, after a short grace: a program that
//     prints its last words and exits should report both in one reply.
// Output from several slices is joined; `max` caps the total (newest kept).
Session::Read read_for(const Call& call, Session& s, std::chrono::milliseconds wait,
                       std::size_t max, bool until_end = false) {
    constexpr std::chrono::milliseconds kSlice{250};
    constexpr std::chrono::milliseconds kExitGrace{100};
    Session::Read acc;
    bool graced = false;
    for (;;) {
        const auto slice = std::min(wait, graced ? kExitGrace : kSlice);
        auto r = s.read(slice, max);
        acc.dropped += r.dropped;
        acc.output  += r.output;
        acc.running  = r.running;
        acc.eof      = r.eof;
        acc.outcome  = r.outcome;
        acc.uptime   = r.uptime;
        wait -= slice;
        if (!acc.running || wait.count() <= 0 || call.cancel_requested() || graced)
            break;
        if (!acc.output.empty() && !until_end) graced = true;   // one short look more
    }
    if (acc.output.size() > max) {
        acc.dropped += acc.output.size() - max;
        acc.output.erase(0, acc.output.size() - max);
    }
    return acc;
}

}  // namespace

namespace {

struct StartArgs { std::string command; std::string cwd; };

std::expected<StartArgs, ToolError> parse_start(const json& args, const util::Bounds& b) {
    util::ArgReader reader(args);
    auto command = reader.require_str("command");
    if (!command || command->empty())
        return std::unexpected(ToolError::invalid_args("command is required"));
    auto cwd = reader.str("cwd", ".");
    auto checked = util::make_workspace_path_checked(cwd, "process_start", b);
    if (!checked) return std::unexpected(checked.error());
    return StartArgs{*command, checked->string()};
}

ExecResult run_start(const Call& call, const StartArgs& args, CallExec& exec) {
    // Make room by dropping sessions whose program has ended. A model that
    // starts many short-lived processes and never calls process_stop would
    // otherwise hit the cap. Ended means reaped: there's nothing left to
    // read, the program can't produce more.
    auto [count, entries] = call.with([](ToolState& s) {
        std::vector<Entry> all;
        all.reserve(s.procs.size());
        for (const auto& [_, e] : s.procs) all.push_back(e);
        return std::pair{s.procs.size(), std::move(all)};
    });
    if (count >= kMaxSessions) {
        std::vector<std::string> ended;
        for (const auto& e : entries) {
            // A zero-wait, zero-byte read only asks "are you done?".
            if (!e->session->read(std::chrono::milliseconds{0}, 0).running)
                ended.push_back(e->id);
        }
        // The removed entries die here, after the state is released, so a
        // host Session destructor never runs under the state's lock.
        std::vector<Entry> gone;
        call.with([&](ToolState& s) {
            for (const auto& id : ended)
                if (auto it = s.procs.find(id); it != s.procs.end()) {
                    gone.push_back(std::move(it->second));
                    s.procs.erase(it);
                }
            count = s.procs.size();
        });
    }
    entries.clear();
    if (count >= kMaxSessions)
        return std::unexpected(ToolError::invalid_args(
            "process session limit reached (32 live sessions); call "
            "process_stop on one before starting another"));

    // The cwd is DATA, not a shell prefix: a real chdir in the child, which a
    // sandbox applies as a workdir rather than as an access to allow.
#ifdef _WIN32
    const std::vector<std::string> argv{"cmd.exe", "/c", args.command};
#else
    const std::vector<std::string> argv{"/bin/sh", "-c", args.command};
#endif
    ExecRequest req;
    req.program          = {argv.front(), {argv.begin() + 1, argv.end()}};
    if (!args.cwd.empty()) req.cwd = args.cwd;
    req.max_output_bytes = kRollingBytes;
    auto started = exec.start(req);
    if (!started) return std::unexpected(ToolError::spawn(started.error()));
    std::shared_ptr<Session> session = std::move(*started);
    const std::string id = call.with([](ToolState& s) {
        return "proc-" + std::to_string(s.next_proc++);
    });

    // Give the child a beat to start producing output or crash on the spot.
    // A mistyped command or a busy port otherwise "starts" fine and fails on
    // a later poll; reporting it here is one clear answer instead of two.
    constexpr auto kSettleWindow = std::chrono::milliseconds{300};
    constexpr std::size_t kEarlyBytes = 4000;
    auto first = read_for(call, *session, kSettleWindow, kEarlyBytes, /*until_end=*/true);

    if (!first.running) {
        // Ended within the settle window: no session is kept.
        auto rest = session->read(std::chrono::milliseconds{0}, kEarlyBytes);
        first.output += rest.output;
        if (rest.outcome) first.outcome = rest.outcome;
        const auto code = exit_code_of(first.outcome);
        std::string text = id + " exited immediately";
        if (code) text += " (exit " + std::to_string(*code) + ")";
        text += ": " + args.command;
        text += first.output.empty() ? std::string{"\n(no output)"} : "\n" + first.output;
        // Exit 0 is success: a fast one-shot that finished cleanly isn't
        // something to fix. Only a non-zero or missing code is a failure.
        if (code && *code == 0)
            text += "\n\nThe command finished cleanly (exit 0) before the "
                    "background settle window — its full output is above and no "
                    "session was kept. For commands that finish on their own, "
                    "call `bash` instead of process_start; it's built for "
                    "one-shot runs and returns the output directly.";
        else
            text += "\n\nThe process is no longer running — no session was kept. "
                    "Fix the command and call process_start again, or run a "
                    "one-shot command with bash instead.";
        return ToolOutput{std::move(text), std::nullopt};
    }

    call.with([&](ToolState& s) {
        auto e = std::make_shared<state::Proc>();
        e->id      = id;
        e->command = args.command;
        e->session = session;
        s.procs.emplace(id, std::move(e));
    });

    std::string text = "Started " + id + " (" + id + "): " + args.command
        + "\nStatus: running. Poll with process_poll \"" + id
        + "\", stop with process_stop.";
    if (!first.output.empty()) text += "\n\n" + first.output;
    return ToolOutput{std::move(text), std::nullopt};
}

struct PollArgs { std::string id; int max_chars = 30000; int wait_ms = 250; };
std::expected<PollArgs, ToolError> parse_poll(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    // wait_ms up to 300 s (matches the bash tool's max). A long block is safe:
    // the host returns the instant output arrives OR the child exits, and
    // the wait is sliced so a cancel is noticed.
    return PollArgs{*id, std::clamp(reader.integer("max_chars", 30000), 1000, 100000),
                    std::clamp(reader.integer("wait_ms", 250), 0, 300000)};
}

ExecResult run_poll(const Call& call, const PollArgs& args, CallExec&) {
    const auto entry = find_entry(call, args.id);
    if (!entry)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint(call)));

    const auto r = read_for(call, *entry->session, std::chrono::milliseconds{args.wait_ms},
                            static_cast<std::size_t>(args.max_chars));

    std::string text = args.id;
    if (r.running) {
        text += " running (" + std::to_string(r.uptime.count()) + "s)";
    } else {
        text += " exited";
        if (const auto code = exit_code_of(r.outcome)) text += " (exit " + std::to_string(*code) + ")";
    }
    text += "\n";
    if (r.dropped)
        text += "[" + std::to_string(r.dropped) + " bytes of earlier output "
                "scrolled past the buffer before this poll]\n";
    if (!r.output.empty())
        text += r.output;
    else if (r.running)
        text += "(no new output yet — process still running; poll again)";
    else if (!r.eof)
        // Exited but the pipe is still open: a background child inherited
        // the write end, and more output may come from it.
        text += "(process exited but its output pipe is still open — a "
                "background child likely inherited it; poll again for more, "
                "or process_stop to force-close and reap)";
    else
        text += "(process has exited; no further output — call process_stop to reap it)";
    return ToolOutput{std::move(text), std::nullopt};
}

struct StopArgs { std::string id; };
std::expected<StopArgs, ToolError> parse_stop(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    return StopArgs{*id};
}

ExecResult run_stop(const Call& call, const StopArgs& args, CallExec&) {
    // Out of the map first: from here this call is the only one that can
    // reach the session through the tools.
    const auto entry = take_entry(call, args.id);
    if (!entry)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint(call)));
    auto& s = *entry->session;
    const bool was_running = s.read(std::chrono::milliseconds{0}, 0).running;
    s.stop();
    // Give it the TERM->KILL grace to finish, so the reply carries the exit
    // code and the last output. Bounded: a call never hangs on a stop.
    constexpr auto kReapWait = std::chrono::milliseconds{3000};
    const auto r = read_for(call, s, kReapWait, 30000, /*until_end=*/true);
    std::string text = (was_running ? "Stopped " : "Reaped ") + args.id;
    if (const auto code = exit_code_of(r.outcome)) text += " (exit " + std::to_string(*code) + ")";
    if (r.dropped)
        text += "\n[" + std::to_string(r.dropped) + " bytes of earlier output were dropped]";
    if (!r.output.empty()) text += "\n" + r.output;
    return ToolOutput{std::move(text), std::nullopt};
}

json start_schema() {
    return json{{"type","object"}, {"required", {"command"}}, {"properties", {
        {"command", {{"type","string"}, {"description",
            "Shell command to run as a persistent background process (dev "
            "server, file watcher, log tail). For a command that finishes on "
            "its own, use bash instead."}}},
        {"cwd", {{"type","string"}, {"description",
            "Working directory, relative to the workspace root (default: "
            "workspace root)."}}}
    }}};
}
json poll_schema() {
    return json{{"type","object"}, {"required", {"id"}}, {"properties", {
        {"id", {{"type","string"}, {"description","Session id returned by process_start."}}},
        {"max_chars", {{"type","integer"}, {"minimum",1000}, {"maximum",100000}, {"default",30000},
                       {"description","Cap on bytes of new output returned this poll (most recent kept)."}}},
        {"wait_ms", {{"type","integer"}, {"minimum",0}, {"maximum",300000}, {"default",250},
                     {"description","Block up to this long (ms, max 300000 = 5 min) waiting for "
                                    "new output before returning — returns THE INSTANT output arrives "
                                    "or the process exits, so a big value is free for a quiet process. "
                                    "Raise it (e.g. 60000) to wait on a slow build/boot instead of "
                                    "busy-polling; the wait stays interruptible."}}}
    }}};
}
json stop_schema() {
    return json{{"type","object"}, {"required", {"id"}}, {"properties", {
        {"id", {{"type","string"}, {"description","Session id to terminate and reap."}}}
    }}};
}

} // namespace

void register_process_tools(Shells& shells, const std::shared_ptr<Exec>& exec) {
    if (!exec) return;

    shells.add("process_start",
        "Start a long-running background process (dev server, watcher, log tail) "
        "and return a session id. Waits ~300ms so an immediate crash is reported "
        "right away with its exit code and output; otherwise it keeps running for "
        "process_poll (incremental output) and process_stop (cleanup). Use bash "
        "for commands that finish on their own.",
        start_schema(), EffectSet{Effect::Exec},
        body_with<StartArgs>([exec](const Call& c, const StartArgs& a) { CallExec ce{*exec, c}; return run_start(c, a, ce); }, parse_start), 4000);
    shells.add("process_poll",
        "Fetch output produced by a background session SINCE THE LAST POLL, plus "
        "its status (running + uptime, or exited + exit code). Blocks briefly for "
        "new output. Reports if any output scrolled past the rolling buffer.",
        poll_schema(), EffectSet{Effect::Exec},
        body_with<PollArgs>([exec](const Call& c, const PollArgs& a) { CallExec ce{*exec, c}; return run_poll(c, a, ce); }, parse_poll), 30000);
    shells.add("process_stop",
        "Terminate (SIGTERM→SIGKILL) and reap a background session, returning its "
        "exit code and any final output not yet delivered by process_poll. Always "
        "call this to clean up a session you started.",
        stop_schema(), EffectSet{Effect::Exec},
        body_with<StopArgs>([exec](const Call& c, const StopArgs& a) { CallExec ce{*exec, c}; return run_stop(c, a, ce); }, parse_stop), 30000);
}

} // namespace mcp::tools::detail
