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

// A long-lived child process with bidirectional stdio is available on every
// platform agentty targets: POSIX (fork/exec/pipe) and Windows
// (CreateProcess/CreatePipe). Both expose the SAME ChildProcess interface so
// StdioServerProvider — and any client that spawns its own MCP servers —
// works identically everywhere.
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
#  define MCP_CAP_HAVE_PROCESS 1
#else
#  define MCP_CAP_HAVE_PROCESS 0
#endif

#if MCP_CAP_HAVE_PROCESS

#if defined(_WIN32)
//
// ── Windows backend (CreateProcess + CreatePipe) ────────────────────────────
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace mcp::cap {

// Move-only owner of a single Windows pipe HANDLE. Closes on scope exit unless
// release()'d — the CreatePipe/CreateProcess prologue's leak-ladder becomes
// automatic: every pipe end is parked in a HandleGuard the instant CreatePipe
// hands it back, any throw before the commit point unwinds them all, and at the
// commit point the surviving ends are release()'d into the streambufs (which
// own them thereafter). Zero runtime cost: one HANDLE, no vtable, no heap.
class HandleGuard {
public:
    HandleGuard() noexcept = default;
    explicit HandleGuard(HANDLE h) noexcept : h_(h) {}
    HandleGuard(HandleGuard&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    HandleGuard& operator=(HandleGuard&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    HandleGuard(const HandleGuard&)            = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] HANDLE release() noexcept { HANDLE h = h_; h_ = nullptr; return h; }
    void reset() noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) { ::CloseHandle(h_); }
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// A std::streambuf backed by a Windows pipe HANDLE. Replaces libstdc++'s
// __gnu_cxx::stdio_filebuf (which is unavailable on MSVC) so ChildProcess can
// expose std::istream/std::ostream over the child's stdio pipes. Blocking
// ReadFile/WriteFile, single-byte put-back area — the StdioTransport reads
// line-buffered JSON-RPC frames, so a small buffer is fine.
class handle_streambuf final : public std::streambuf {
public:
    explicit handle_streambuf(HANDLE h) : h_(h) {
        setg(in_, in_ + 1, in_ + 1);   // empty get area (force underflow)
        setp(out_, out_ + sizeof(out_));
    }
    ~handle_streambuf() override { sync(); close(); }

    handle_streambuf(const handle_streambuf&)            = delete;
    handle_streambuf& operator=(const handle_streambuf&) = delete;

    void close() noexcept {
        if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr) {
            ::CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    [[nodiscard]] bool valid() const noexcept {
        return h_ != INVALID_HANDLE_VALUE && h_ != nullptr;
    }

protected:
    // ── reading: fill the 1-byte get area from the pipe ──────────────────
    int_type underflow() override {
        if (!valid()) return traits_type::eof();
        DWORD got = 0;
        // ReadFile blocks until ≥1 byte or the write end closes (→ got==0 /
        // ERROR_BROKEN_PIPE), which we surface as EOF — mirroring POSIX read().
        if (!::ReadFile(h_, in_, 1, &got, nullptr) || got == 0)
            return traits_type::eof();
        setg(in_, in_, in_ + 1);
        return traits_type::to_int_type(in_[0]);
    }

    // ── writing: flush the put area to the pipe ──────────────────────────
    int_type overflow(int_type ch) override {
        if (sync() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            char c = traits_type::to_char_type(ch);
            *pptr() = c;
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override {
        std::ptrdiff_t n = pptr() - pbase();
        if (n <= 0) return 0;
        if (!valid()) return -1;
        const char* p = pbase();
        std::ptrdiff_t left = n;
        while (left > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(h_, p, static_cast<DWORD>(left), &wrote, nullptr) ||
                wrote == 0)
                return -1;
            p    += wrote;
            left -= wrote;
        }
        setp(out_, out_ + sizeof(out_));
        return 0;
    }

private:
    HANDLE h_;
    char   in_[1]{};
    char   out_[4096]{};
};

// A spawned child process whose stdin/stdout are wired to iostreams. stderr is
// inherited from the parent (MCP servers log there). Construction throws
// std::runtime_error on spawn failure. Windows CreateProcess backend; same
// interface as the POSIX version below.
class ChildProcess {
public:
    struct Spawn {
        std::string              command;   // executable (PATH-resolved)
        std::vector<std::string> args;      // NOT including argv[0]
        std::vector<std::string> env_kv;    // extra "KEY=VALUE" entries
    };

    explicit ChildProcess(const Spawn& s) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;          // pipe ends are inheritable

        // Each pipe end is owned by a HandleGuard the moment CreatePipe yields
        // it, so any throw before the commit point closes every open handle
        // automatically — no hand-rolled CloseHandle ladder to drift out of
        // sync with the open set.
        HANDLE rd = nullptr, wr = nullptr;
        if (!::CreatePipe(&rd, &wr, &sa, 0))
            throw std::runtime_error("mcp::cap: CreatePipe(stdin) failed");
        HandleGuard child_stdin_rd{rd}, child_stdin_wr{wr};
        if (!::CreatePipe(&rd, &wr, &sa, 0))
            throw std::runtime_error("mcp::cap: CreatePipe(stdout) failed");
        HandleGuard child_stdout_rd{rd}, child_stdout_wr{wr};

        // The PARENT ends must NOT be inherited by the child, else they never
        // close and we'd never see EOF.
        ::SetHandleInformation(child_stdin_wr.get(),  HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(child_stdout_rd.get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = child_stdin_rd.get();
        si.hStdOutput = child_stdout_wr.get();
        si.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);   // inherit parent stderr

        std::string cmdline = build_command_line_(s);
        std::string env_block;
        const bool have_env = build_env_block_(s, env_block);

        PROCESS_INFORMATION pi{};
        BOOL ok = ::CreateProcessA(
            /*lpApplicationName=*/nullptr,
            /*lpCommandLine=*/cmdline.data(),
            /*procAttrs=*/nullptr, /*threadAttrs=*/nullptr,
            /*bInheritHandles=*/TRUE,
            /*creationFlags=*/0,
            /*lpEnvironment=*/have_env ? env_block.data() : nullptr,
            /*lpCurrentDirectory=*/nullptr,
            &si, &pi);

        // The child owns its ends now; drop ours regardless of success.
        child_stdin_rd.reset();
        child_stdout_wr.reset();
        if (!ok)
            // child_stdin_wr / child_stdout_rd close here via guard unwind.
            throw std::runtime_error("mcp::cap: CreateProcess('" + s.command +
                                     "') failed (err " +
                                     std::to_string(::GetLastError()) + ")");
        ::CloseHandle(pi.hThread);
        proc_   = pi.hProcess;
        pid_    = static_cast<int>(pi.dwProcessId);

        // Commit point: release the surviving parent ends into the streambufs,
        // which own the handles thereafter and close them via shutdown().
        in_buf_  = std::make_unique<handle_streambuf>(child_stdout_rd.release());  // child stdout
        out_buf_ = std::make_unique<handle_streambuf>(child_stdin_wr.release());   // child stdin
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
        if (!proc_) return false;
        return ::WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
    }

    // Close ONLY the child's stdin (our write end) → the server sees EOF and
    // begins exiting, which closes its stdout and unblocks a reader parked in
    // ReadFile. Does NOT touch the read stream or reap the process.
    void close_stdin() noexcept {
        if (out_stream_) out_stream_->flush();
        out_stream_.reset();
        out_buf_.reset();   // closes child's stdin HANDLE
    }

    // Close child stdin (EOF → graceful exit), wait briefly, then terminate.
    // Idempotent; also called by the destructor.
    void shutdown() noexcept {
        if (out_stream_) out_stream_->flush();
        out_stream_.reset();
        out_buf_.reset();             // EOF to the child
        if (proc_) {
            if (::WaitForSingleObject(proc_, 500) == WAIT_TIMEOUT)
                ::TerminateProcess(proc_, 1);
            ::WaitForSingleObject(proc_, 2000);
            ::CloseHandle(proc_);
            proc_ = nullptr;
            pid_  = -1;
        }
        in_stream_.reset();
        in_buf_.reset();
    }

private:
    // Quote one argv token per the CommandLineToArgvW rules MSVCRT uses, so a
    // child parsing its command line recovers the exact arguments.
    static std::string quote_arg_(const std::string& a) {
        if (!a.empty() &&
            a.find_first_of(" \t\n\v\"") == std::string::npos)
            return a;                         // no quoting needed
        std::string out = "\"";
        for (auto it = a.begin();; ++it) {
            std::size_t backslashes = 0;
            while (it != a.end() && *it == '\\') { ++it; ++backslashes; }
            if (it == a.end()) {
                out.append(backslashes * 2, '\\');   // escape trailing run
                break;
            } else if (*it == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
            } else {
                out.append(backslashes, '\\');
                out.push_back(*it);
            }
        }
        out.push_back('"');
        return out;
    }

    static std::string build_command_line_(const Spawn& s) {
        std::string cl = quote_arg_(s.command);
        for (const auto& a : s.args) { cl.push_back(' '); cl += quote_arg_(a); }
        return cl;
    }

    // Build a merged environment block (parent env + s.env_kv overrides) in the
    // double-NUL-terminated form CreateProcess wants. Returns false (and leaves
    // `out` empty) when there are no overrides, so the caller passes nullptr to
    // simply inherit the parent environment.
    static bool build_env_block_(const Spawn& s, std::string& out) {
        if (s.env_kv.empty()) return false;
        // Start from the parent environment.
        std::vector<std::string> entries;
        if (LPCH env = ::GetEnvironmentStringsA()) {
            for (const char* p = env; *p; ) {
                std::string e = p;
                p += e.size() + 1;
                if (!e.empty() && e[0] != '=') entries.push_back(std::move(e));
            }
            ::FreeEnvironmentStringsA(env);
        }
        auto key_of = [](const std::string& kv) {
            auto eq = kv.find('=');
            return eq == std::string::npos ? kv : kv.substr(0, eq);
        };
        auto ci_eq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::toupper((unsigned char)a[i]) !=
                    std::toupper((unsigned char)b[i])) return false;
            return true;
        };
        for (const auto& kv : s.env_kv) {
            if (kv.find('=') == std::string::npos) continue;
            std::string k = key_of(kv);
            for (auto& e : entries)
                if (ci_eq(key_of(e), k)) { e.clear(); break; }  // drop old
            entries.push_back(kv);
        }
        for (const auto& e : entries) {
            if (e.empty()) continue;
            out += e;
            out.push_back('\0');
        }
        out.push_back('\0');   // final terminator
        return true;
    }

    HANDLE proc_ = nullptr;
    int    pid_  = -1;
    std::unique_ptr<handle_streambuf> in_buf_;
    std::unique_ptr<handle_streambuf> out_buf_;
    std::unique_ptr<std::istream>     in_stream_;
    std::unique_ptr<std::ostream>     out_stream_;
};

} // namespace mcp::cap

#else  // !_WIN32 — POSIX backend (fork/exec/pipe)

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

// Move-only owner of a single pipe-end FD. Closes on scope exit unless
// release()'d, so the spawn prologue's leak-ladder (close every FD opened so
// far on any early throw) becomes automatic and impossible to get wrong: each
// ::pipe() end is parked in an FdGuard the instant it exists, a throw between
// here and the commit point unwinds them in reverse, and at the commit point
// the surviving ends are release()'d — into the child (via dup2) or into the
// parent's streambufs. Zero runtime cost: one int, no vtable, no heap.
class FdGuard {
public:
    FdGuard() noexcept = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(FdGuard&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FdGuard& operator=(FdGuard&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    FdGuard(const FdGuard&)            = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { int f = fd_; fd_ = -1; return f; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    int fd_ = -1;
};

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
        // parent[1] → child stdin[0]; child stdout[1] → parent[0]. Each end is
        // owned by an FdGuard the moment ::pipe() hands it back, so any throw
        // before the commit point unwinds every open FD automatically — no
        // hand-rolled close-ladder to drift out of sync with the open set.
        int in_pipe[2]  = {-1, -1};
        int out_pipe[2] = {-1, -1};
        if (::pipe(in_pipe) != 0)
            throw std::runtime_error("mcp::cap: pipe() failed (stdin)");
        FdGuard in_rd{in_pipe[0]}, in_wr{in_pipe[1]};
        if (::pipe(out_pipe) != 0)
            throw std::runtime_error("mcp::cap: pipe() failed (stdout)");
        FdGuard out_rd{out_pipe[0]}, out_wr{out_pipe[1]};

        pid_ = ::fork();
        if (pid_ < 0)
            throw std::runtime_error("mcp::cap: fork() failed");
            // in_rd/in_wr/out_rd/out_wr all close here via guard unwind.

        if (pid_ == 0) {
            // ── child ────────────────────────────────────────────────────
            // Raw FDs: this is the forked child; the parent's guards are
            // copies in a separate address space and irrelevant here.
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
        // Drop the child-side ends (guards close them on destruction); the
        // parent-side ends are release()'d into the stdio_filebufs, which now
        // own the FDs and close them via shutdown(). Commit point: from here
        // on the streams hold the only references.
        in_rd.reset();    // child's stdin read end — parent doesn't use it
        out_wr.reset();   // child's stdout write end — parent doesn't use it
        in_buf_  = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(out_rd.release(), std::ios::in);
        out_buf_ = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(in_wr.release(),  std::ios::out);
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

    // Close ONLY the child's stdin (our write end). A well-behaved server sees
    // EOF and begins exiting, which in turn closes its stdout — unblocking a
    // reader thread parked in getline on our read end. Does NOT reap or touch
    // the read stream, so a reader can drain remaining output + the EOF
    // cleanly. Safe to call before joining the reader, then shutdown() after.
    void close_stdin() noexcept {
        out_stream_.reset();
        out_buf_.reset();   // closes child's stdin FD
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

#endif // _WIN32 vs POSIX backend

#endif // MCP_CAP_HAVE_PROCESS
