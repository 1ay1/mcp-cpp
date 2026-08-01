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
    bool stopped = false;

    void append(std::string_view text) {
        std::lock_guard<std::mutex> lock(output_mu);
        output.append(text);
        if (output.size() > kRollingBytes) {
            const auto erased = output.size() - kRollingBytes;
            output.erase(0, erased);
            output_base += erased;
        }
    }

    std::string take_new(std::size_t max_chars) {
        std::lock_guard<std::mutex> lock(output_mu);
        const auto end = output_base + output.size();
        auto begin = std::max(delivered, output_base);
        if (end - begin > max_chars) begin = end - max_chars;
        std::string result = output.substr(begin - output_base, end - begin);
        delivered = end;
        return result;
    }

    bool running() {
        std::lock_guard<std::mutex> lock(stop_mu);
        return !stopped && child && child->alive();
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lock(stop_mu);
        if (stopped) return;
        stopped = true;
        if (child) child->terminate();
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
        if (manager.sessions.size() >= 32)
            return std::unexpected(ToolError::invalid_args(
                "process session limit reached (32); stop an existing session"));
    }

#ifdef _WIN32
    std::string escaped_cwd = args.cwd;
    std::size_t quote_pos = 0;
    while ((quote_pos = escaped_cwd.find('"', quote_pos)) != std::string::npos) {
        escaped_cwd.insert(quote_pos, 1, '"');
        quote_pos += 2;
    }
    const std::string command = "cd /d \"" + escaped_cwd + "\" && " + args.command;
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
    return ToolOutput{"Started " + session->id + " (pid "
        + std::to_string(session->child->pid()) + "): " + args.command, std::nullopt};
}

struct PollArgs { std::string id; int max_chars = 30000; int wait_ms = 100; };
std::expected<PollArgs, ToolError> parse_poll(const json& args) {
    util::ArgReader reader(args);
    auto id = reader.require_str("id");
    if (!id || id->empty()) return std::unexpected(ToolError::invalid_args("id is required"));
    return PollArgs{*id, std::clamp(reader.integer("max_chars", 30000), 1000, 100000),
                    std::clamp(reader.integer("wait_ms", 100), 0, 5000)};
}

ExecResult run_poll(const PollArgs& args) {
    auto session = find_session(args.id);
    if (!session) return std::unexpected(ToolError::not_found("unknown process session: " + args.id));
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{args.wait_ms};
    std::string output;
    bool running = false;
    do {
        running = session->running();
        output = session->take_new(static_cast<std::size_t>(args.max_chars));
        if (!output.empty() || !running || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    } while (true);
    std::string text = args.id + (running ? " running" : " exited") + "\n";
    text += output.empty() ? "(no output yet)" : output;
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
            return std::unexpected(ToolError::not_found("unknown process session: " + args.id));
        session = it->second;
        manager.sessions.erase(it);
    }
    session->stop();
    auto output = session->take_new(30000);
    std::string text = "Stopped " + args.id;
    if (!output.empty()) text += "\n" + output;
    return ToolOutput{std::move(text), std::nullopt};
}

json start_schema() {
    return json{{"type","object"}, {"required", {"command"}}, {"properties", {
        {"command", {{"type","string"}, {"description","Shell command for a long-running process."}}},
        {"cwd", {{"type","string"}, {"description","Workspace directory (default workspace root)."}}}
    }}};
}
json poll_schema() {
    return json{{"type","object"}, {"required", {"id"}}, {"properties", {
        {"id", {{"type","string"}, {"description","Process session id returned by process_start."}}},
        {"max_chars", {{"type","integer"}, {"minimum",1000}, {"maximum",100000}, {"default",30000}}},
        {"wait_ms", {{"type","integer"}, {"minimum",0}, {"maximum",5000}, {"default",100},
                     {"description","Wait this long for new/initial output before returning."}}}
    }}};
}
json stop_schema() {
    return json{{"type","object"}, {"required", {"id"}}, {"properties", {
        {"id", {{"type","string"}, {"description","Process session id to terminate."}}}
    }}};
}

} // namespace

void register_process_tools(Shells& shells) {
    shells.add("process_start",
        "Start a long-running workspace process such as a dev server or watcher. Returns a session id; use process_poll for rolling output and process_stop for cleanup.",
        start_schema(), EffectSet{Effect::Exec},
        body<StartArgs>(run_start, parse_start), 4000);
    shells.add("process_poll",
        "Poll a long-running process session and return its status plus only stdout/stderr produced since the previous poll.",
        poll_schema(), EffectSet{Effect::Exec},
        body<PollArgs>(run_poll, parse_poll), 30000);
    shells.add("process_stop",
        "Terminate and reap a long-running process session, returning only final output not delivered by earlier polls.",
        stop_schema(), EffectSet{Effect::Exec},
        body<StopArgs>(run_stop, parse_stop), 30000);
}

} // namespace mcp::tools::detail
