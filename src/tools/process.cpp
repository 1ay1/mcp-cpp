// SPDX-License-Identifier: Apache-2.0
// Long-running process sessions for dev servers, watchers, and log tails.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/cap/process.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/sandbox.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
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
    std::unique_ptr<mcp::cap::ChildProcess> child;
    std::thread reader;
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
        return !stopped && child && child->alive();
    }

    // Exit code once the child has been reaped (running() observed false).
    // 128+N encodes death by signal N. nullopt while still running. Cached so
    // it survives stop() tearing the child down (child.reset()).
    std::optional<int> exit_code() {
        std::lock_guard<std::mutex> lock(stop_mu);
        if (!cached_exit_ && child) cached_exit_ = child->exit_code();
        return cached_exit_;
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
        std::lock_guard<std::mutex> lock(stop_mu);
        if (stopped) return;
        stopped = true;
        if (child) {
            child->terminate();
            if (!cached_exit_) cached_exit_ = child->exit_code();
            // A failed session/group setup or unrelated inherited descriptor
            // must not leave process_stop blocked forever in stream.get().
            // POSIX fd_streambuf uses a wake pipe, so this does not close an
            // FD underneath the reader thread.
            child->interrupt_output();
        }
        if (reader.joinable()) reader.join();
        child.reset();
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

ExecResult run_start(const StartArgs& args) {
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

#ifdef _WIN32
    // Build the payload for `cmd.exe /d /s /c "<payload>"`.
    //
    // cmd.exe does NOT understand backslash-escaped quotes: the previous
    // `cd /d \"<cwd>\"` produced a literal backslash in the path and every
    // process_start died with "The filename, directory name, or volume
    // label syntax is incorrect." With /s, cmd strips exactly the first
    // and last quote of the payload and runs the rest VERBATIM, so the
    // inner quotes around the path must be plain, unescaped quotes.
    //
    // A cwd containing a quote can't be expressed this way at all (cmd has
    // no escape for it) — but such a path also can't exist on Windows,
    // where " is an illegal filename character. Reject it explicitly
    // instead of emitting a command line that would re-parse into
    // something else.
    if (args.cwd.find('"') != std::string::npos)
        return std::unexpected(ToolError::invalid_args(
            "cwd contains a quote character, which cmd.exe cannot escape"));
    const std::string command = "cd /d \"" + args.cwd + "\" && " + args.command;
#else
    auto quote = [](std::string_view value) {
        std::string out{"'"};
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };
    const std::string command = "cd -- " + quote(args.cwd)
        + " && exec /bin/sh -c " + quote(args.command);
#endif
    auto argv = util::sandbox::prepare_shell_argv(command);
    if (argv.empty())
        return std::unexpected(ToolError::spawn("sandbox produced an empty process argv"));

    mcp::cap::ChildProcess::Spawn spawn;
    spawn.command = argv.front();
    spawn.args.assign(argv.begin() + 1, argv.end());
    spawn.cwd = args.cwd;
    spawn.merge_stderr = true;

    auto session = std::make_shared<Session>();
    session->id = "proc-" + std::to_string(manager.sequence.fetch_add(1));
    session->command = args.command;
    try {
        session->child = std::make_unique<mcp::cap::ChildProcess>(spawn);
    } catch (const std::exception& error) {
        return std::unexpected(ToolError::spawn(error.what()));
    }

    Session* raw = session.get();
    raw->reader = std::thread([raw] {
        auto& stream = raw->child->out();
        char buffer[4096];
        while (stream.good()) {
            const int first = stream.get(); // blocks until one byte or EOF
            if (first == std::char_traits<char>::eof()) break;
            const char byte = static_cast<char>(first);
            raw->append(std::string_view{&byte, 1});
            const auto available = stream.rdbuf()->in_avail();
            if (available > 0) {
                const auto count = stream.rdbuf()->sgetn(
                    buffer, std::min<std::streamsize>(available, sizeof(buffer)));
                if (count > 0) raw->append(std::string_view{buffer, static_cast<std::size_t>(count)});
            }
        }
    });
    {
        std::lock_guard<std::mutex> lock(manager.mu);
        manager.sessions.emplace(session->id, session);
    }

    // Give the child a beat to either start producing output or crash on the
    // spot. A mistyped command, a missing binary, or a port-already-in-use
    // server otherwise leaves the model to "start" successfully and only
    // discover the failure on a later poll. Reporting it here — with the exit
    // code and whatever it printed — turns a two-call surprise into one clear
    // answer.
    constexpr auto kSettleWindow = std::chrono::milliseconds{300};
    const auto settle_deadline = std::chrono::steady_clock::now() + kSettleWindow;
    while (std::chrono::steady_clock::now() < settle_deadline) {
        if (!session->running()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    const std::string head =
        "Started " + session->id + " (pid "
        + std::to_string(session->child->pid()) + "): " + args.command;

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

struct PollArgs {
    std::string id;
    int max_chars = 30000;
    int wait_ms = 250;
    bool wait_for_exit = false;
};
std::expected<PollArgs, ToolError> parse_poll(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    const bool wait_for_exit = reader.boolean("wait_for_exit", false);
    // In wait_for_exit mode the caller is parking on a long silent phase (a
    // build link, a slow test) so allow a much longer block; otherwise keep
    // the responsive 5s ceiling for incremental log tailing.
    const int wait_cap = wait_for_exit ? 600000 : 5000;
    const int wait_default = wait_for_exit ? 60000 : 250;
    return PollArgs{*id,
                    std::clamp(reader.integer("max_chars", 30000), 1000, 100000),
                    std::clamp(reader.integer("wait_ms", wait_default), 0, wait_cap),
                    wait_for_exit};
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
    if (manager.sessions.empty()) return " (no live sessions)";
    std::string s = " (live sessions:";
    for (const auto& [id, _] : manager.sessions) s += " " + id;
    s += ")";
    return s;
}

ExecResult run_poll(const PollArgs& args) {
    auto session = find_session(args.id);
    if (!session)
        return std::unexpected(ToolError::not_found(
            "unknown process session: " + args.id + live_session_hint()));
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds{args.wait_ms};
    std::string output;
    std::size_t dropped = 0;
    bool running = false;
    do {
        running = session->running();
        output = session->take_new(static_cast<std::size_t>(args.max_chars), &dropped);
        // Normal mode: return as soon as ANY output arrives. wait_for_exit
        // mode: ignore incremental output and keep parking until the process
        // actually exits (or the deadline hits) — one call to await a long
        // silent phase instead of many empty polls.
        const bool have_output_break = !output.empty() && !args.wait_for_exit;
        if (have_output_break || !running || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{args.wait_for_exit ? 100 : 10});
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
        const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (args.wait_for_exit) {
            text += "(still running after waiting " + std::to_string(waited)
                  + "s with no exit — poll again with wait_for_exit to keep "
                    "parking, or process_stop to give up)";
        } else {
            text += "(no new output for " + std::to_string(waited)
                  + "s; process still running. If you're waiting for it to "
                    "FINISH, poll with wait_for_exit=true instead of repeating "
                    "short polls)";
        }
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

ExecResult run_stop(const StopArgs& args) {
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
        {"wait_for_exit", {{"type","boolean"}, {"default",false},
            {"description", "Block until the process EXITS (or wait_ms elapses) instead of returning on the first output. Use this to await a long silent phase (a build link, a slow test) in ONE call rather than many empty polls. In this mode wait_ms defaults to 60000 and may go up to 600000."}}},
        {"wait_ms", {{"type","integer"}, {"minimum",0}, {"maximum",600000}, {"default",250},
                     {"description","Block up to this long waiting for new output before returning "
                                    "(returns early as soon as any arrives). Raise it when waiting on "
                                    "a slow-to-boot server. Capped at 5000 normally; up to 600000 with "
                                    "wait_for_exit=true."}}}
    }}};
}
json stop_schema() {
    return json{{"type","object"}, {"required", {"id"}}, {"properties", {
        {"id", {{"type","string"}, {"description","Session id to terminate and reap."}}}
    }}};
}

} // namespace

void register_process_tools(Shells& shells) {
    shells.add("process_start",
        "Start a long-running background process (dev server, watcher, log tail) "
        "and return a session id. Waits ~300ms so an immediate crash is reported "
        "right away with its exit code and output; otherwise it keeps running for "
        "process_poll (incremental output) and process_stop (cleanup). Use bash "
        "for commands that finish on their own.",
        start_schema(), EffectSet{Effect::Exec},
        body<StartArgs>(run_start, parse_start), 4000);
    shells.add("process_poll",
        "Fetch output produced by a background session SINCE THE LAST POLL, plus "
        "its status (running + uptime, or exited + exit code). Blocks briefly for "
        "new output. Reports if any output scrolled past the rolling buffer. To "
        "await a long silent phase (a build link, a slow test) in ONE call "
        "instead of many empty polls, pass wait_for_exit=true - it blocks until "
        "the process exits.",
        poll_schema(), EffectSet{Effect::Exec},
        body<PollArgs>(run_poll, parse_poll), 30000);
    shells.add("process_stop",
        "Terminate (SIGTERM→SIGKILL) and reap a background session, returning its "
        "exit code and any final output not yet delivered by process_poll. Always "
        "call this to clean up a session you started.",
        stop_schema(), EffectSet{Effect::Exec},
        body<StopArgs>(run_stop, parse_stop), 30000);
}

} // namespace mcp::tools::detail
