// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/process.hpp — a long-lived child process with bidirectional stdio,
// exposed as std::iostream so StdioTransport can drive it.
//
//   This is the missing piece for a *client that spawns its own servers*:
//   StdioTransport wants std::istream&/std::ostream&, but a spawned MCP
//   server's stdio are raw OS pipe FDs. We bridge them with libstdc++'s
//   __gnu_cxx::stdio_filebuf — zero extra buffering, no new dependency.
//
//   POSIX-only (Linux/macOS). Header-only, like the rest of mcp-cpp.
//
#pragma once

#if defined(__unix__) || defined(__APPLE__)
#  define MCP_CAP_HAVE_PROCESS 1
#else
#  define MCP_CAP_HAVE_PROCESS 0
#endif

#if MCP_CAP_HAVE_PROCESS

#include <ext/stdio_filebuf.h>   // __gnu_cxx::stdio_filebuf (libstdc++)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mcp::cap {

// A spawned child process whose stdin/stdout are wired to iostreams. stderr is
// left attached to the parent's (MCP servers log there; the spec keeps stderr
// free for logging). Construction throws std::runtime_error on spawn failure.
class ChildProcess {
public:
    struct Spawn {
        std::string              command;   // executable (PATH-resolved via execvp)
        std::vector<std::string> args;      // NOT including argv[0]
        std::vector<std::string> env_kv;    // extra "KEY=VALUE" entries
    };

    explicit ChildProcess(const Spawn& s) {
        int in_pipe[2]  = {-1, -1};   // parent[1] → child stdin[0]
        int out_pipe[2] = {-1, -1};   // child stdout[1] → parent[0]
        if (::pipe(in_pipe) != 0)
            throw std::runtime_error("mcp::cap: pipe() failed (stdin)");
        if (::pipe(out_pipe) != 0) {
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            throw std::runtime_error("mcp::cap: pipe() failed (stdout)");
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(in_pipe[0]);  ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            throw std::runtime_error("mcp::cap: fork() failed");
        }

        if (pid_ == 0) {
            // ── child ────────────────────────────────────────────────────
            ::dup2(in_pipe[0],  STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(in_pipe[0]);  ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            for (const auto& kv : s.env_kv) {
                if (kv.find('=') == std::string::npos) continue;
                ::putenv(::strdup(kv.c_str()));   // intentionally leaked pre-exec
            }
            std::vector<std::string> store;
            store.reserve(s.args.size() + 1);
            store.push_back(s.command);
            for (const auto& a : s.args) store.push_back(a);
            std::vector<char*> argv;
            argv.reserve(store.size() + 1);
            for (auto& v : store) argv.push_back(v.data());
            argv.push_back(nullptr);
            ::execvp(s.command.c_str(), argv.data());
            std::fprintf(stderr, "mcp::cap: exec '%s' failed: %s\n",
                         s.command.c_str(), std::strerror(errno));
            ::_exit(127);
        }

        // ── parent ──────────────────────────────────────────────────────
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        in_buf_  = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(out_pipe[0], std::ios::in);
        out_buf_ = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(in_pipe[1],  std::ios::out);
        in_stream_  = std::make_unique<std::istream>(in_buf_.get());
        out_stream_ = std::make_unique<std::ostream>(out_buf_.get());
    }

    ~ChildProcess() { shutdown(); }

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] std::istream& out() noexcept { return *in_stream_; }   // child stdout
    [[nodiscard]] std::ostream& in()  noexcept { return *out_stream_; }  // child stdin
    [[nodiscard]] int pid() const noexcept { return pid_; }

    [[nodiscard]] bool alive() const noexcept {
        if (pid_ <= 0) return false;
        int status = 0;
        return ::waitpid(pid_, &status, WNOHANG) == 0;
    }

    // Close child stdin (EOF → graceful exit), poll briefly, then SIGTERM.
    // Idempotent; also called by the destructor.
    void shutdown() noexcept {
        out_stream_.reset();
        out_buf_.reset();             // closes child's stdin → EOF
        if (pid_ > 0) {
            int status = 0;
            for (int i = 0; i < 50; ++i) {        // ~500ms grace
                pid_t r = ::waitpid(pid_, &status, WNOHANG);
                if (r == pid_ || r < 0) { pid_ = -1; break; }
                ::usleep(10'000);
            }
            if (pid_ > 0) {
                ::kill(pid_, SIGTERM);
                ::waitpid(pid_, &status, 0);
                pid_ = -1;
            }
        }
        in_stream_.reset();
        in_buf_.reset();
    }

private:
    int pid_ = -1;
    std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> in_buf_;
    std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> out_buf_;
    std::unique_ptr<std::istream> in_stream_;
    std::unique_ptr<std::ostream> out_stream_;
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
