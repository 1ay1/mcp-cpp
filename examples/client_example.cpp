// SPDX-License-Identifier: Apache-2.0
//
// client_example.cpp — spawn the example server and drive it over stdio,
// written in straight-line async style with coroutines (mcp/coro.hpp).
//
//   Usage:  mcp_client_example /path/to/mcp_server_example
//
//   It forks the server, connects its stdio, and runs:
//     initialize → list_tools → call_tool(add) → read_resource → get_prompt.
//
#include <mcp/mcp.hpp>
#include <mcp/coro.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

using namespace mcp;

// A minimal std::streambuf over a raw file descriptor, using POSIX read/write.
// Replaces __gnu_cxx::stdio_filebuf (a libstdc++-only GNU extension that is
// absent under libc++, e.g. Termux/Android) so the example builds on every
// standard library. Unbuffered on the write side (each overflow flushes) —
// fine for a line-oriented JSON-RPC demo.
class FdStreambuf final : public std::streambuf {
public:
    explicit FdStreambuf(int fd) : fd_(fd) {}
    ~FdStreambuf() override { sync(); }

protected:
    // ── output ──
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            char c = static_cast<char>(ch);
            if (::write(fd_, &c, 1) != 1) return traits_type::eof();
        }
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize written = 0;
        while (written < n) {
            ssize_t w = ::write(fd_, s + written, static_cast<size_t>(n - written));
            if (w <= 0) break;
            written += w;
        }
        return written;
    }
    int sync() override { return 0; }

    // ── input ── (single-byte buffer; the transport reads line by line)
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        ssize_t n = ::read(fd_, &in_ch_, 1);
        if (n <= 0) return traits_type::eof();
        setg(&in_ch_, &in_ch_, &in_ch_ + 1);
        return traits_type::to_int_type(in_ch_);
    }

private:
    int  fd_;
    char in_ch_ = 0;
};

// Spawn `argv[0]`, wiring our pipes to its stdio. Returns child pid and two
// FILE* (write to child stdin, read from child stdout).
struct Child { pid_t pid; int to_child; int from_child; };

static Child spawn(const char* path) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) { perror("pipe"); std::exit(1); }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(path, path, (char*)nullptr);
        perror("execl"); std::_Exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    return {pid, in_pipe[1], out_pipe[0]};
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-mcp_server_example>\n";
        return 2;
    }
    Child child = spawn(argv[1]);

    // Build C++ streams over the pipe fds via a portable fd streambuf.
    FdStreambuf in_buf(child.from_child);
    FdStreambuf out_buf(child.to_child);
    std::istream child_out(&in_buf);
    std::ostream child_in(&out_buf);

    StdioTransport transport(child_out, child_in);
    Client client(transport.sink());
    transport.start(client.engine());

    using mcp::co::Task;
    using mcp::co::operator co_await;

    auto drive = [&]() -> Task<int> {
        auto init = co_await client.initialize(
            Implementation{"mcp-cpp-client", std::string(kLibraryVersion), Nothing, Nothing, Nothing, Nothing});
        std::cout << "✓ initialized with " << init.serverInfo.name
                  << " (protocol " << init.protocolVersion << ")\n";
        if (init.instructions) std::cout << "  instructions: " << *init.instructions << "\n";
        client.initialized();

        auto tools = co_await client.list_tools();
        std::cout << "✓ tools:";
        for (const auto& t : tools.tools) std::cout << " " << t.name;
        std::cout << "\n";

        auto add = co_await client.call_tool("add", Json{{"a", 17}, {"b", 25}});
        std::cout << "✓ add(17,25): ";
        if (!add.content.empty() && std::holds_alternative<TextContent>(add.content[0]))
            std::cout << std::get<TextContent>(add.content[0]).text;
        if (add.structuredContent) std::cout << "  | structured=" << add.structuredContent->dump();
        std::cout << "\n";

        auto res = co_await client.read_resource("file:///motd");
        if (!res.contents.empty() && std::holds_alternative<TextResourceContents>(res.contents[0]))
            std::cout << "✓ motd: " << std::get<TextResourceContents>(res.contents[0]).text << "\n";

        GetPromptParams gp;
        gp.name = "summarize";
        gp.arguments = std::vector<std::pair<std::string,std::string>>{{"text", "MCP is a protocol."}};
        auto prompt = co_await client.get_prompt(gp);
        std::cout << "✓ prompt 'summarize' produced " << prompt.messages.size() << " message(s)\n";

        co_return 0;
    };

    int rc = drive().get();

    // Tear down: close child stdin → server sees EOF and exits.
    child_in.flush();
    ::close(child.to_child);
    int status = 0;
    waitpid(child.pid, &status, 0);
    transport.stop();
    return rc;
}
