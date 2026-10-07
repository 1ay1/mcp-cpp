// SPDX-License-Identifier: Apache-2.0
// Long-running process sessions for dev servers, watchers, and log tails.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/cap/process.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/progress.hpp>   // cancellation::requested()
#include <mcp/tools/util/sandbox.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;

namespace {

constexpr std::size_t kRollingBytes = 128 * 1024;

struct Session {
    std::string id;
    std::string command;
    // The host's. It owns the process, the pipe and the thread that keeps
    // the pipe from filling; this layer owns the rolling buffer and the
    // bookkeeping of what a caller has already been shown.
    std::shared_ptr<::mcp::tools::Session> proc;
    std::mutex output_mu;
    std::mutex stop_mu;
    std::string output;
    std::size_t output_base = 0;
    std::size_t delivered = 0;
    std::size_t dropped_unseen = 0;   // bytes evicted before any poll saw them
    std::optional<int> cached_exit_;  // exit code captured before child.reset()
    std::chrono::steady_clock::time_point started_at =
        std::chrono::steady_clock::now();
    bool stopped = false;
    bool running_ = true;
    // Set by the reader thread when it consumes EOF on the output pipe.
    // Distinguishes "child exited AND every byte is in the buffer" from
    // "child exited but the pipe is still open" — either the reader hasn't
    // drained the tail yet (scheduling), or a grandchild inherited the write
    // end and keeps producing. Poll uses this to spend its wait budget on
    // the tail instead of falsely reporting "no further output".
    std::atomic<bool> reader_eof{false};

    [[nodiscard]] bool reader_finished() const noexcept {
        return reader_eof.load(std::memory_order_acquire);
    }

    void append(std::string_view text) {
        std::lock_guard<std::mutex> lock(output_mu);
        output.append(text);
        if (output.size() > kRollingBytes) {
            const auto erased = output.size() - kRollingBytes;
            // If the rolling buffer evicts bytes the caller never polled,
            // remember how many so the next poll can honestly say output was
            // dropped rather than silently losing the head of a burst.
            if (output_base + erased > delivered)
                dropped_unseen += (output_base + erased) - std::max(delivered, output_base);
            output.erase(0, erased);
            output_base += erased;
        }
    }

    // Returns freshly-produced output plus, via `dropped`, the count of bytes
    // that scrolled out of the rolling buffer before this poll could see them.
    std::string take_new(std::size_t max_chars, std::size_t* dropped = nullptr) {
        std::lock_guard<std::mutex> lock(output_mu);
        if (dropped) { *dropped = dropped_unseen; dropped_unseen = 0; }
        const auto end = output_base + output.size();
        auto begin = std::max(delivered, output_base);
        std::size_t clipped = 0;
        if (end - begin > max_chars) { clipped = (end - begin) - max_chars; begin = end - max_chars; }
        if (dropped) *dropped += clipped;   // over-budget bytes are also unseen
        std::string result = output.substr(begin - output_base, end - begin);
        delivered = end;
        return result;
    }

    bool running() {
        std::lock_guard<std::mutex> lock(stop_mu);
        return !stopped && running_;
    }

    // Exit code once the child has been reaped (running() observed false).
    // 128+N encodes death by signal N. nullopt while still running. Cached so
    // it survives stop() tearing the child down (child.reset()).
    std::optional<int> exit_code() {
        std::lock_guard<std::mutex> lock(stop_mu);
        return cached_exit_;
    }

    // Pull whatever the host has for us and fold it into the rolling
    // buffer. This is the only place the two layers meet.
    void ingest(std::chrono::milliseconds wait) {
        auto proc_handle = [&] {
            std::lock_guard<std::mutex> lock(stop_mu);
            return proc;
        }();
        if (!proc_handle) return;
        auto u = proc_handle->poll(wait);
        if (!u.output.empty()) append(u.output);
        std::lock_guard<std::mutex> lock(stop_mu);
        running_ = u.running;
        if (u.outcome && !cached_exit_) {
            std::visit([&]<class T>(const T& o) {
                if constexpr (std::is_same_v<T, Exited>)         cached_exit_ = o.code;
                else if constexpr (std::is_same_v<T, Signalled>) cached_exit_ = 128 + o.signal;
                else if constexpr (std::is_same_v<T, StartFailed>) cached_exit_ = 127;
                else cached_exit_ = 0;
            }, *u.outcome);
        }
        if (!u.running) reader_eof.store(true, std::memory_order_release);
    }

    std::chrono::seconds age() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_at);
    }

    // True if any produced output has not yet been handed to a poll caller.
    // Used to keep an exited-but-unread session alive so its tail isn't lost.
    bool has_pending_output() {
        std::lock_guard<std::mutex> lock(output_mu);
        return delivered < output_base + output.size();
    }

    void stop() noexcept {
        std::shared_ptr<::mcp::tools::Session> handle;
        {
            std::lock_guard<std::mutex> lock(stop_mu);
            if (stopped) return;
            stopped = true;
            running_ = false;
            handle = std::exchange(proc, nullptr);
        }
        // Outside the lock: the host's stop() waits for its drain thread,
        // and holding our mutex across that invites a deadlock with a
        // concurrent poll.
        if (handle) handle->stop();
    }

    ~Session() { stop(); }
};

struct ProcessManager {
    std::mutex mu;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    std::atomic<unsigned long long> sequence{1};

    static ProcessManager& instance() {
        static ProcessManager manager;
        return manager;
    }
};

std::shared_ptr<Session> find_session(const std::string& id) {
    auto& manager = ProcessManager::instance();
    std::lock_guard<std::mutex> lock(manager.mu);
    if (auto it = manager.sessions.find(id); it != manager.sessions.end()) return it->second;
    return {};
}

struct StartArgs { std::string command; std::string cwd; };

std::expected<StartArgs, ToolError> parse_start(const json& args) {
    util::ArgReader reader(args);
    auto command = reader.require_str("command");
    if (!command || command->empty())
        return std::unexpected(ToolError::invalid_args("command is required"));
    auto cwd = reader.str("cwd", ".");
    auto checked = util::make_workspace_path_checked(cwd, "process_start");
    if (!checked) return std::unexpected(checked.error());
    return StartArgs{*command, checked->string()};
}

ExecResult run_start(const StartArgs& args, Exec& exec) {
    auto& manager = ProcessManager::instance();
    {
        std::lock_guard<std::mutex> lock(manager.mu);
        // Garbage-collect sessions whose child already exited and whose
        // output has been fully drained by a prior poll — a model that
        // starts many short-lived processes and never calls process_stop
        // would otherwise wedge at the cap with a confusing error.
        for (auto it = manager.sessions.begin(); it != manager.sessions.end();) {
            if (!it->second->running() && !it->second->has_pending_output())
                it = manager.sessions.erase(it);
            else
                ++it;
        }
        if (manager.sessions.size() >= 32)
            return std::unexpected(ToolError::invalid_args(
                "process session limit reached (32 live sessions); call "
                "process_stop on one before starting another"));
    }

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

    auto session = std::make_shared<Session>();
    session->id = "proc-" + std::to_string(manager.sequence.fetch_add(1));
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

    {
        std::lock_guard<std::mutex> lock(manager.mu);
        manager.sessions.emplace(session->id, session);
        if (std::getenv("MCP_PROC_TRACE"))
            std::fprintf(stderr, "[proc] inserted %s, map=%zu mgr=%p\n",
                         session->id.c_str(), manager.sessions.size(),
                         (void*)&manager);
    }

    // Give the child a beat to either start producing output or crash on the
    // spot. A mistyped command, a missing binary, or a port-already-in-use
    // server otherwise leaves the model to "start" successfully and only
    // discover the failure on a later poll. Reporting it here — with the exit
    // code and whatever it printed — turns a two-call surprise into one clear
    // answer.
    constexpr auto kSettleWindow = std::chrono::milliseconds{300};
    session->ingest(kSettleWindow);

    const std::string head =
        "Started " + session->id + " ("
        + session->id + "): " + args.command;

    if (!session->running()) {
        // Exited within the settle window — almost always a failure. Stop()
        // first: it joins the reader thread so every last byte is appended
        // before we drain. Then drop the session (nothing to poll) but
        // surface the code.
        session->stop();
        auto early = session->take_new(4000);
        const auto code = session->exit_code();
        {
            std::lock_guard<std::mutex> lock(manager.mu);
            manager.sessions.erase(session->id);
        }
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
    std::string early = session->take_new(4000);
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
    // run_poll returns the instant output arrives OR the child exits, and the
    // loop honours cancellation so the user can always interrupt it.
    return PollArgs{*id, std::clamp(reader.integer("max_chars", 30000), 1000, 100000),
                    std::clamp(reader.integer("wait_ms", 250), 0, 300000)};
}

// Human-friendly list of live session ids for recovery hints. Caller must NOT
// already hold manager.mu.
std::string live_session_hint_locked(ProcessManager& manager);
std::string live_session_hint() {
    auto& manager = ProcessManager::instance();
    std::lock_guard<std::mutex> lock(manager.mu);
    return live_session_hint_locked(manager);
}

// Same, for callers that already hold manager.mu.
std::string live_session_hint_locked(ProcessManager& manager) {
    if (std::getenv("MCP_PROC_TRACE"))
        std::fprintf(stderr, "[proc] lookup: map=%zu mgr=%p\n",
                     manager.sessions.size(), (void*)&manager);
    if (manager.sessions.empty()) return " (no live sessions)";
    std::string s = " (live sessions:";
    for (const auto& [id, _] : manager.sessions) s += " " + id;
    s += ")";
    return s;
}

ExecResult run_poll(const PollArgs& args, Exec&) {
    auto session = find_session(args.id);
    if (!session)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint()));
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds{args.wait_ms};
    std::string output;
    std::size_t dropped = 0;
    bool running = false;
    // Adaptive backoff: poll at 10 ms while fresh (snappy first-byte + exit
    // latency), then ramp toward 50 ms once the process has been quiet for a
    // while, so a MULTI-MINUTE wait on a silent build/boot doesn't busy-spin.
    // The 50 ms ceiling bounds how long we can miss a cancellation request or
    // the first output byte.
    long sleep_ms = 10;
    do {
        running = session->running();
        session->ingest(std::chrono::milliseconds{0});
        output = session->take_new(static_cast<std::size_t>(args.max_chars), &dropped);
        if (!output.empty()) break;
        // Honour cooperative cancellation (user interrupt): a long wait_ms must
        // never wedge the agent. Return what we have and let the caller re-poll.
        if (util::cancellation::requested()) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        // Child exited AND the reader consumed EOF — every byte is in the
        // buffer and it's empty: truly nothing more, stop waiting. Without
        // the reader_finished() check we'd break the moment the child died,
        // racing the reader thread and reporting a crashed server's FINAL
        // LINES — the error the caller is polling for — as "no output".
        if (!running && session->reader_finished()) break;
        // Don't overshoot the deadline with the last sleep.
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{std::min<long>(sleep_ms, remain)});
        // Ramp the interval after the process has been quiet ~0.5 s.
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds{500} && sleep_ms < 50)
            sleep_ms = 50;
    } while (true);
    // Re-check liveness after draining: a process that printed its last line
    // and THEN exited during this same poll should report "exited (code)"
    // together with that final output, not "running" — saving the model an
    // extra poll just to learn it's done.
    if (running) running = session->running();

    std::string status = args.id;
    if (running) {
        status += " running (" + std::to_string(session->age().count()) + "s)";
    } else {
        status += " exited";
        if (auto code = session->exit_code())
            status += " (exit " + std::to_string(*code) + ")";
    }
    std::string text = std::move(status) + "\n";
    if (dropped)
        text += "[" + std::to_string(dropped) + " bytes of earlier output "
                "scrolled past the buffer before this poll]\n";
    if (!output.empty()) {
        text += output;
    } else if (running) {
        text += "(no new output yet — process still running; poll again)";
    } else if (!session->reader_finished()) {
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

ExecResult run_stop(const StopArgs& args, Exec&) {
    auto& manager = ProcessManager::instance();
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(manager.mu);
        auto it = manager.sessions.find(args.id);
        if (it == manager.sessions.end())
            return std::unexpected(ToolError::not_found(
                "unknown process session: " + args.id
                + live_session_hint_locked(manager)));
        session = it->second;
        manager.sessions.erase(it);
    }
    const bool was_running = session->running();
    session->stop();
    std::size_t dropped = 0;
    session->ingest(std::chrono::milliseconds{0});
    auto output = session->take_new(30000, &dropped);
    std::string text = (was_running ? "Stopped " : "Reaped ") + args.id;
    if (auto code = session->exit_code())
        text += " (exit " + std::to_string(*code) + ")";
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
        body_with<StartArgs>([exec](const StartArgs& a) { return run_start(a, *exec); }, parse_start), 4000);
    shells.add("process_poll",
        "Fetch output produced by a background session SINCE THE LAST POLL, plus "
        "its status (running + uptime, or exited + exit code). Blocks briefly for "
        "new output. Reports if any output scrolled past the rolling buffer.",
        poll_schema(), EffectSet{Effect::Exec},
        body_with<PollArgs>([exec](const PollArgs& a) { return run_poll(a, *exec); }, parse_poll), 30000);
    shells.add("process_stop",
        "Terminate (SIGTERM→SIGKILL) and reap a background session, returning its "
        "exit code and any final output not yet delivered by process_poll. Always "
        "call this to clean up a session you started.",
        stop_schema(), EffectSet{Effect::Exec},
        body_with<StopArgs>([exec](const StopArgs& a) { return run_stop(a, *exec); }, parse_stop), 30000);
}

} // namespace mcp::tools::detail
