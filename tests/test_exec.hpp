// tests/test_exec.hpp — the smallest tools::Exec a test needs: run a program
// to completion with popen (merged output), or start one in the background
// for process_start (fork/exec, a reader thread). No budgets or sandbox. A
// real host (agentty) implements Exec with its own clocks and process tree.
// Tests may use threads; the library may not.
#pragma once

#if !defined(_WIN32)

#include <mcp/tools/host.hpp>

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

// A background child: a thread reads its merged output until EOF; poll()
// hands out what arrived and waits up to `wait` for more.
class BgSession final : public tools::Session {
public:
    BgSession(pid_t pid, int fd) : pid_(pid), fd_(fd), started_(std::chrono::steady_clock::now()) {
        reader_ = std::thread([this] {
            char buf[4096];
            for (;;) {
                const auto n = ::read(fd_, buf, sizeof buf);
                std::lock_guard lk(mu_);
                if (n <= 0) { eof_ = true; cv_.notify_all(); return; }
                pending_.append(buf, static_cast<std::size_t>(n));
                cv_.notify_all();
            }
        });
    }
    ~BgSession() override {
        stop();
        if (reader_.joinable()) reader_.join();
        ::close(fd_);
    }
    Update poll(std::chrono::milliseconds wait) override {
        std::unique_lock lk(mu_);
        cv_.wait_for(lk, wait, [&] { return !pending_.empty() || eof_; });
        Update u;
        u.output = std::exchange(pending_, {});
        reap_locked();
        u.running = !exited_;
        if (exited_ && eof_ && !reported_) { u.outcome = outcome_; reported_ = true; }
        u.uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_);
        return u;
    }
    void stop() override {
        std::lock_guard lk(mu_);
        reap_locked();
        if (!exited_) {
            ::kill(-pid_, SIGKILL);
            int st = 0;
            ::waitpid(pid_, &st, 0);
            exited_ = true;
            outcome_ = tools::Signalled{SIGKILL};
        }
    }
private:
    void reap_locked() {
        if (exited_) return;
        int st = 0;
        if (::waitpid(pid_, &st, WNOHANG) == pid_) {
            exited_ = true;
            if (WIFEXITED(st)) outcome_ = tools::Exited{WEXITSTATUS(st)};
            else               outcome_ = tools::Signalled{WIFSIGNALED(st) ? WTERMSIG(st) : 0};
        }
    }
    pid_t pid_;
    int   fd_;
    std::chrono::steady_clock::time_point started_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::string             pending_;
    bool eof_ = false, exited_ = false, reported_ = false;
    tools::ExecOutcome outcome_ = tools::Exited{0};
    std::thread reader_;
};

struct PopenExec final : tools::Exec {
    tools::ExecResult run(const tools::ExecRequest& r) override {
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
