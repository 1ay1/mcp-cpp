// SPDX-License-Identifier: Apache-2.0
// Long-running process sessions for dev servers, watchers, and log tails.

#include "tool_shell.hpp"
#include "call_state.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;

namespace {

constexpr std::size_t kRollingBytes = 128 * 1024;
constexpr std::size_t kMaxSessions  = 32;

// The sessions live in the tool state (ToolState::procs). The host's
// Session owns the process, the pipe and whatever keeps the pipe drained;
// a state::Proc is this layer's rolling buffer and the bookkeeping of what a
// caller has already been shown. Every touch of it is one short with(): the
// waiting (poll) happens on the host's Session, outside the state.

void append(state::Proc& p, std::string_view text) {
    p.output.append(text);
    if (p.output.size() > kRollingBytes) {
        const auto erased = p.output.size() - kRollingBytes;
        // Evicting bytes no poll saw: remember how many, so the next poll
        // can say output was dropped instead of silently losing it.
        if (p.output_base + erased > p.delivered)
            p.dropped_unseen += (p.output_base + erased) - std::max(p.delivered, p.output_base);
        p.output.erase(0, erased);
        p.output_base += erased;
    }
}

// Fresh output, and via `dropped` the bytes that scrolled away unseen.
std::string take_new(state::Proc& p, std::size_t max_chars, std::size_t* dropped = nullptr) {
    if (dropped) { *dropped = p.dropped_unseen; p.dropped_unseen = 0; }
    const auto end = p.output_base + p.output.size();
    auto begin = std::max(p.delivered, p.output_base);
    std::size_t clipped = 0;
    if (end - begin > max_chars) { clipped = (end - begin) - max_chars; begin = end - max_chars; }
    if (dropped) *dropped += clipped;   // over-budget bytes are also unseen
    std::string result = p.output.substr(begin - p.output_base, end - begin);
    p.delivered = end;
    return result;
}

// Fold one host update into the session.
void fold(state::Proc& p, const Session::Update& u) {
    if (!u.output.empty()) append(p, u.output);
    p.uptime  = u.uptime;
    p.running = u.running && !p.stopped;
    if (u.outcome && !p.exit_code) {
        std::visit([&]<class T>(const T& o) {
            if constexpr (std::is_same_v<T, Exited>)           p.exit_code = o.code;
            else if constexpr (std::is_same_v<T, Signalled>)   p.exit_code = 128 + o.signal;
            else if constexpr (std::is_same_v<T, StartFailed>) p.exit_code = 127;
            else p.exit_code = 0;
        }, *u.outcome);
    }
    if (!u.running) p.eof = true;
}

// Ask the host for news (waiting up to `wait`), then fold it in.
void ingest(const Call& call, const std::shared_ptr<state::Proc>& p, std::chrono::milliseconds wait) {
    auto handle = call.with([&](ToolState&) { return p->stopped ? nullptr : p->proc; });
    if (!handle) return;
    auto u = handle->poll(wait);
    call.with([&](ToolState&) { fold(*p, u); });
}

std::shared_ptr<state::Proc> find_proc(const Call& call, const std::string& id) {
    return call.with([&](ToolState& s) -> std::shared_ptr<state::Proc> {
        auto it = s.procs.find(id);
        return it == s.procs.end() ? nullptr : it->second;
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

// Stop the host's process (once) and mark the session stopped.
void stop_proc(const Call& call, const std::shared_ptr<state::Proc>& p) {
    auto handle = call.with([&](ToolState&) {
        if (p->stopped) return std::shared_ptr<Session>{};
        p->stopped = true;
        p->running = false;
        return p->proc;
    });
    if (handle) handle->stop();   // outside the state: the host may wait
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

ExecResult run_start(const Call& call, const StartArgs& args, Exec& exec) {
    // Drop sessions whose child exited and whose output a poll has fully
    // drained — a model that starts many short-lived processes and never
    // calls process_stop would otherwise hit the cap with a confusing error.
    const bool full = call.with([](ToolState& s) {
        std::erase_if(s.procs, [](const auto& kv) {
            const auto& p = *kv.second;
            return !p.running && p.delivered >= p.output_base + p.output.size();
        });
        return s.procs.size() >= kMaxSessions;
    });
    if (full)
        return std::unexpected(ToolError::invalid_args(
            "process session limit reached (32 live sessions); call "
            "process_stop on one before starting another"));

    // The cwd is DATA, not a shell prefix.
    //
    // This used to build `cd -- '<cwd>' && exec /bin/sh -c '<cmd>'`, plus a
    // cmd.exe equivalent whose quoting had already caused one "volume label
    // syntax is incorrect" bug. Under a sandbox the prefix is also a path
    // the boundary must permit, and when it did not the session died with
    // "cd: Operation not permitted" before the command ran at all.
    //
    // As req.cwd it is a real chdir in the child: nothing to re-parse, and
    // the sandbox applies it as a workdir rather than as an access to allow.
    const std::string& command = args.command;
    // Name the shell; the host decides what to wrap it in.
#ifdef _WIN32
    const std::vector<std::string> argv{"cmd.exe", "/c", command};
#else
    const std::vector<std::string> argv{"/bin/sh", "-c", command};
#endif

    auto session = std::make_shared<state::Proc>();
    session->command = args.command;

    // The host starts it, inside whatever boundary it applies to everything
    // else it runs. This used to spawn directly through cap::ChildProcess
    // and wrap the argv itself, which meant a background process was
    // confined by a different code path from a foreground one -- and after
    // the bwrap removal, by nothing at all on linux.
    ExecRequest req;
    req.program          = {argv.front(), {argv.begin() + 1, argv.end()}};
    if (!args.cwd.empty()) req.cwd = args.cwd;
    req.max_output_bytes = kRollingBytes;
    auto started = exec.start(req);
    if (!started) return std::unexpected(ToolError::spawn(started.error()));
    session->proc = std::move(*started);
    call.with([&](ToolState& s) {
        session->id = "proc-" + std::to_string(s.next_proc++);
        s.procs.emplace(session->id, session);
    });

    // Give the child a beat to either start producing output or crash on the
    // spot. A mistyped command, a missing binary, or a port-already-in-use
    // server otherwise leaves the model to "start" successfully and only
    // discover the failure on a later poll. Reporting it here — with the exit
    // code and whatever it printed — turns a two-call surprise into one clear
    // answer.
    constexpr auto kSettleWindow = std::chrono::milliseconds{300};
    ingest(call, session, kSettleWindow);

    const std::string head =
        "Started " + session->id + " ("
        + session->id + "): " + args.command;

    if (!call.with([&](ToolState&) { return session->running; })) {
        // Exited within the settle window — almost always a failure. Drain
        // what's left, drop the session (nothing to poll), surface the code.
        ingest(call, session, std::chrono::milliseconds{0});
        stop_proc(call, session);
        auto [early, code] = call.with([&](ToolState& s) {
            s.procs.erase(session->id);
            return std::pair{take_new(*session, 4000), session->exit_code};
        });
        std::string text = session->id + " exited immediately";
        if (code) text += " (exit " + std::to_string(*code) + ")";
        text += ": " + args.command;
        if (!early.empty()) text += "\n" + early;
        else text += "\n(no output)";
        // Exit 0 is SUCCESS — a fast one-shot command that finished cleanly is
        // not something to "fix". Only a nonzero (or signal) exit is a failure
        // the model should act on. Tailor the guidance so a clean quick run
        // isn't mislabeled as broken. (No exit code captured — e.g. killed
        // before reap — is treated as the failure case.)
        const bool clean = code && *code == 0;
        if (clean)
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

    // Still alive: report any banner it already printed so the first poll
    // isn't wasted on the startup line.
    std::string early = call.with([&](ToolState&) { return take_new(*session, 4000); });
    std::string text = head + "\nStatus: running. Poll with process_poll \""
        + session->id + "\", stop with process_stop.";
    if (!early.empty()) text += "\n\n" + early;
    return ToolOutput{std::move(text), std::nullopt};
}

struct PollArgs { std::string id; int max_chars = 30000; int wait_ms = 250; };
std::expected<PollArgs, ToolError> parse_poll(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    // wait_ms up to 300 s (matches the bash tool's max). A long block is safe:
    // the host's poll returns the instant output arrives OR the child exits,
    // and the wait is sliced so a cancel is noticed.
    return PollArgs{*id, std::clamp(reader.integer("max_chars", 30000), 1000, 100000),
                    std::clamp(reader.integer("wait_ms", 250), 0, 300000)};
}

ExecResult run_poll(const Call& call, const PollArgs& args, Exec&) {
    auto session = find_proc(call, args.id);
    if (!session)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint(call)));

    // Wait on the host's session for news, in slices so a cancel is
    // noticed. The host's poll returns as soon as there is output or the
    // child ends; the library keeps no clock, so the budget is counted in
    // slices of the wait the caller asked for.
    constexpr std::chrono::milliseconds kSlice{250};
    std::string output;
    std::size_t dropped = 0;
    auto left = std::chrono::milliseconds{args.wait_ms};
    do {
        const auto wait = std::min(left, kSlice);
        ingest(call, session, wait);
        left -= wait;
        auto [out, done] = call.with([&](ToolState&) {
            std::size_t d = 0;
            auto o = take_new(*session, static_cast<std::size_t>(args.max_chars), &d);
            dropped += d;
            return std::pair{std::move(o), !session->running && session->eof};
        });
        output = std::move(out);
        if (!output.empty() || done || call.cancel_requested()) break;
    } while (left.count() > 0);

    auto [running, uptime, code, eof] = call.with([&](ToolState&) {
        return std::tuple{session->running, session->uptime, session->exit_code, session->eof};
    });
    std::string status = args.id;
    if (running) {
        status += " running (" + std::to_string(uptime.count()) + "s)";
    } else {
        status += " exited";
        if (code) status += " (exit " + std::to_string(*code) + ")";
    }
    std::string text = std::move(status) + "\n";
    if (dropped)
        text += "[" + std::to_string(dropped) + " bytes of earlier output "
                "scrolled past the buffer before this poll]\n";
    if (!output.empty()) {
        text += output;
    } else if (running) {
        text += "(no new output yet — process still running; poll again)";
    } else if (!eof) {
        // Exited but the pipe never hit EOF within the wait budget: a
        // backgrounded descendant inherited the write end. More output may
        // still arrive from it.
        text += "(process exited but its output pipe is still open — a "
                "background child likely inherited it; poll again for more, "
                "or process_stop to force-close and reap)";
    } else {
        text += "(process has exited; no further output — call process_stop to reap it)";
    }
    return ToolOutput{std::move(text), std::nullopt};
}

struct StopArgs { std::string id; };
std::expected<StopArgs, ToolError> parse_stop(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    return StopArgs{*id};
}

ExecResult run_stop(const Call& call, const StopArgs& args, Exec&) {
    auto session = call.with([&](ToolState& s) -> std::shared_ptr<state::Proc> {
        auto it = s.procs.find(args.id);
        if (it == s.procs.end()) return nullptr;
        auto p = it->second;
        s.procs.erase(it);
        return p;
    });
    if (!session)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint(call)));
    const bool was_running = call.with([&](ToolState&) { return session->running; });
    auto handle = call.with([&](ToolState&) { return session->proc; });
    stop_proc(call, session);
    // One last look for the final output and exit code.
    if (handle) {
        auto u = handle->poll(std::chrono::milliseconds{0});
        call.with([&](ToolState&) { fold(*session, u); });
    }
    std::size_t dropped = 0;
    auto [output, code] = call.with([&](ToolState&) {
        return std::pair{take_new(*session, 30000, &dropped), session->exit_code};
    });
    std::string text = (was_running ? "Stopped " : "Reaped ") + args.id;
    if (code) text += " (exit " + std::to_string(*code) + ")";
    if (dropped)
        text += "\n[" + std::to_string(dropped) + " bytes of earlier output were dropped]";
    if (!output.empty()) text += "\n" + output;
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
        body_with<StartArgs>([exec](const Call& c, const StartArgs& a) { return run_start(c, a, *exec); }, parse_start), 4000);
    shells.add("process_poll",
        "Fetch output produced by a background session SINCE THE LAST POLL, plus "
        "its status (running + uptime, or exited + exit code). Blocks briefly for "
        "new output. Reports if any output scrolled past the rolling buffer.",
        poll_schema(), EffectSet{Effect::Exec},
        body_with<PollArgs>([exec](const Call& c, const PollArgs& a) { return run_poll(c, a, *exec); }, parse_poll), 30000);
    shells.add("process_stop",
        "Terminate (SIGTERM→SIGKILL) and reap a background session, returning its "
        "exit code and any final output not yet delivered by process_poll. Always "
        "call this to clean up a session you started.",
        stop_schema(), EffectSet{Effect::Exec},
        body_with<StopArgs>([exec](const Call& c, const StopArgs& a) { return run_stop(c, a, *exec); }, parse_stop), 30000);
}

} // namespace mcp::tools::detail
