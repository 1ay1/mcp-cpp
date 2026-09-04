// SPDX-License-Identifier: Apache-2.0
//
// stdio_transport_test.cpp — StdioTransport reader-thread teardown. Pins the
// two exit paths: (1) natural EOF stops the reader promptly; (2) a reader still
// BLOCKED in getline (peer never closed the stream) does NOT hang stop() — it
// detaches after a bounded grace window. This is the "^C during a wedged MCP
// server never quits" guard, at the library level.
//
#include "agtest.hpp"

static int g_failures = 0;

#include <mcp/rpc.hpp>
#include <mcp/stdio.hpp>

#include <atomic>
#include <chrono>
#include <istream>
#include <sstream>
#include <streambuf>
#include <thread>

using namespace mcp;

namespace {
// A streambuf whose underflow() blocks until released — models a pipe read-end
// with no EOF (a wedged MCP server). getline() parks until release() is called.
struct BlockingBuf : std::streambuf {
    std::atomic<bool> released{false};
    int underflow() override {
        while (!released.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return traits_type::eof();
    }
    void release() { released.store(true, std::memory_order_release); }
};
} // namespace

TEST_CASE("stdio transport teardown") {
    // ── 1. Natural EOF: the reader ends, stop() joins instantly ──────────
    {
        std::istringstream in("");    // already at EOF
        std::ostringstream out;
        StdioTransport t(in, out);
        RpcEngine engine(t.sink());

        t.start(engine);
        const auto t0 = std::chrono::steady_clock::now();
        t.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 300);   // reader hit EOF at once; join is immediate
    }

    // ── 2. Wedged reader: stop() detaches instead of hanging ─────────────
    {
        BlockingBuf buf;
        std::istream in(&buf);
        std::ostringstream out;
        StdioTransport t(in, out);
        RpcEngine engine(t.sink());
        t.start(engine);

        // Reader is parked in getline (underflow blocks). stop() must return
        // within the ~500 ms grace window via detach — never hang.
        const auto t0 = std::chrono::steady_clock::now();
        t.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 1500);

        // Release the detached reader so it exits before buf/in leave scope.
        buf.release();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(g_failures == 0);
}
