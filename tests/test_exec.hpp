// tests/test_exec.hpp — the smallest tools::Exec a test needs: run a program
// to completion with popen (merged output), or start one in the background
// for process_start (fork/exec, a reader thread). No budgets or sandbox. A
// real host (agentty) implements Exec with its own clocks and process tree.
// Tests may use threads; the library may not.
#pragma once

#if !defined(_WIN32)

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mcp::test {

// A background child for tests. A reader thread drains the pipe into one
// rolling buffer; read() hands out what's new past the caller-visible
// cursor. stop() only signals; the reader and a reaper finish on their own,
// and the destructor joins them (tests own their threads; it's bounded by
// the SIGKILL).
class BgSession final : public tools::Session {
public:
    BgSession(pid_t pid, int fd) : pid_(pid), fd_(fd), started_(std::chrono::steady_clock::now()) {
        reader_ = std::thread([this] {
            char buf[4096];
            for (;;) {
                const auto n = ::read(fd_, buf, sizeof buf);
                std::lock_guard lk(mu_);
                if (n <= 0) { eof_ = true; cv_.notify_all(); return; }
                buf_.append(buf, static_cast<std::size_t>(n));
                if (buf_.size() > kWindow) {
                    const auto cut = buf_.size() - kWindow;
                    buf_.erase(0, cut);
                    base_ += cut;
                }
                cv_.notify_all();
            }
        });
        reaper_ = std::thread([this] {
            int st = 0;
            ::waitpid(pid_, &st, 0);
            std::lock_guard lk(mu_);
            exited_ = true;
            if (WIFEXITED(st)) outcome_ = tools::Exited{WEXITSTATUS(st)};
            else               outcome_ = tools::Signalled{WIFSIGNALED(st) ? WTERMSIG(st) : 0};
            cv_.notify_all();
        });
    }
    ~BgSession() override {
        stop();
        if (reaper_.joinable()) reaper_.join();
        if (reader_.joinable()) reader_.join();
        ::close(fd_);
    }
    Read read(std::chrono::milliseconds wait, std::size_t max) override {
        std::unique_lock lk(mu_);
        const auto fresh = [&] { return base_ + buf_.size() > seen_; };
        cv_.wait_for(lk, wait, [&] { return fresh() || exited_; });
        // After the exit, give the reader a beat to drain what was written
        // just before it (the dying-breath tail).
        if (exited_ && !eof_) cv_.wait_for(lk, std::chrono::milliseconds{20}, [&] { return eof_; });
        Read r;
        const auto end = base_ + buf_.size();
        auto from = std::max(seen_, base_);
        r.dropped = from - seen_;
        if (end - from > max) { r.dropped += (end - from) - max; from = end - max; }
        r.output  = buf_.substr(from - base_, end - from);
        seen_     = end;
        r.running = !exited_;
        r.eof     = eof_;
        if (exited_) r.outcome = outcome_;
        r.uptime  = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_);
        return r;
    }
    void stop() noexcept override {
        std::lock_guard lk(mu_);
        if (!exited_) ::kill(-pid_, SIGKILL);
    }
private:
    static constexpr std::size_t kWindow = 128 * 1024;
    pid_t pid_;
    int   fd_;
    std::chrono::steady_clock::time_point started_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::string             buf_;
    std::size_t base_ = 0, seen_ = 0;
    bool eof_ = false, exited_ = false;
    tools::ExecOutcome outcome_ = tools::Exited{0};
    std::thread reader_, reaper_;
};

struct PopenExec final : tools::Exec {
    tools::ExecResult run(const tools::Call&, const tools::ExecRequest& r) override {
        auto q = [](const std::string& s) {
            std::string o = "'";
            for (char c : s) o += (c == '\'') ? std::string("'\\''") : std::string(1, c);
            return o + "'";
        };
        std::string cmd;
        if (r.cwd) cmd += "cd " + q(*r.cwd) + " && ";
        for (const auto& [k, v] : r.env) cmd += k + "=" + q(v) + " ";
        cmd += q(r.program.exe);
        for (const auto& a : r.program.args) cmd += " " + q(a);
        cmd += " 2>&1";
        tools::ExecResult out;
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) { out.outcome = tools::StartFailed{"popen failed"}; return out; }
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
            out.output.append(buf, n);
            if (r.max_output_bytes && out.output.size() > *r.max_output_bytes) {
                out.output.resize(*r.max_output_bytes);
                out.truncated = true;
            }
        }
        const int st = ::pclose(p);
        if (WIFEXITED(st)) {
            const int code = WEXITSTATUS(st);
            if (code == 127) out.outcome = tools::StartFailed{"not found: " + r.program.exe};
            else             out.outcome = tools::Exited{code};
        } else {
            out.outcome = tools::Signalled{WIFSIGNALED(st) ? WTERMSIG(st) : 0};
        }
        return out;
    }
    std::expected<std::shared_ptr<tools::Session>, std::string> start(const tools::ExecRequest& r) override {
        int fds[2];
        if (::pipe(fds) != 0) return std::unexpected(std::string{"pipe failed"});
        const pid_t pid = ::fork();
        if (pid < 0) return std::unexpected(std::string{"fork failed"});
        if (pid == 0) {
            ::setpgid(0, 0);
            int devnull = ::open("/dev/null", O_RDONLY);
            ::dup2(devnull, 0);
            ::dup2(fds[1], 1);
            ::dup2(fds[1], 2);
            ::close(fds[0]); ::close(fds[1]);
            if (r.cwd && ::chdir(r.cwd->c_str()) != 0) ::_exit(127);
            std::vector<char*> argv;
            std::string exe = r.program.exe;
            argv.push_back(exe.data());
            std::vector<std::string> args = r.program.args;
            for (auto& a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            ::execvp(exe.c_str(), argv.data());
            ::_exit(127);
        }
        ::close(fds[1]);
        return std::make_shared<BgSession>(pid, fds[0]);
    }
    bool stops_whole_tree() const noexcept override { return false; }
};

}  // namespace mcp::test

#endif
