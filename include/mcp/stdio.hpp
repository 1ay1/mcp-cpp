// SPDX-License-Identifier: Apache-2.0
//
// acp/stdio.hpp — line-delimited stdio transport.
//
//   Per the ACP spec:
//     • messages are JSON-RPC envelopes
//     • framing is a single '\n' between messages
//     • messages MUST NOT contain embedded '\n'
//     • stderr is free for logging (we leave it alone)
//
//   StdioTransport owns:
//     • a write-side mutex (so multiple threads may call the Transport)
//     • a dedicated reader thread that pumps lines into the engine
//
//   The reader stops when the input descriptor returns EOF.
//
#pragma once

#include <mcp/rpc.hpp>

#include <atomic>
#include <cstdio>
#include <istream>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

namespace mcp {

class StdioTransport {
public:
    // The streams must outlive the transport. `in` is the agent's stdin (when
    // wrapping an agent) or the spawned child's stdout (when wrapping a client).
    // out_ptr_ mirrors out_ as an atomic pointer so sink() can detect
    // invalidation (invalidate_output() sets it to nullptr) without
    // dereferencing a dangling reference.
    StdioTransport(std::istream& in, std::ostream& out)
        : in_(in), out_(out), out_ptr_(&out) {}

    StdioTransport(const StdioTransport&)            = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    ~StdioTransport() { stop(); }

    // The Transport function the engine writes through.
    Transport sink() {
        return [this](std::string_view line) {
            std::lock_guard lk(write_mu_);
            // Use the atomic pointer, not the reference: if close_stdin()
            // or teardown has nulled out_ptr_, we silently drop the write
            // instead of dereferencing a dangling ostream&.
            auto* out = out_ptr_.load(std::memory_order_acquire);
            if (!out || !out->good()) return;
            out->write(line.data(), static_cast<std::streamsize>(line.size()));
            out->put('\n');
            out->flush();
            if (!out->good()) out->clear(std::ios::badbit);
        };
    }

    // Called by the host before destroying the ostream (e.g. in
    // close_stdin/terminate). After this, sink() silently drops writes.
    void invalidate_output() noexcept {
        out_ptr_.store(nullptr, std::memory_order_release);
    }

    // Run the read pump on a dedicated thread. The pump terminates on EOF or
    // when stop() is called. On natural EOF (peer closed) the engine's
    // on_transport_closed() fires, failing all in-flight requests with
    // errc::ConnectionLost and invoking its error callback.
    void start(RpcEngine& engine) {
        engine_ = &engine;
        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this, &engine]{
            std::string line;
            while (running_.load(std::memory_order_acquire)) {
                if (!std::getline(in_, line)) break;        // EOF or error
                if (!line.empty()) {
                    try { engine.feed_line(line); }
                    catch (...) { /* never let one frame kill the pump */ }
                }
            }
            const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
            // Only surface a transport-closed event if we stopped because the
            // stream ended, not because stop() was called deliberately.
            if (was_running) engine.on_transport_closed("eof");
        });
    }

    // Wait until the reader thread exits.
    void join() {
        if (reader_.joinable()) reader_.join();
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        // We can't easily interrupt std::getline; users should close the stream
        // (e.g. send EOF) to wake the reader. join() does the rest.
        if (reader_.joinable()) reader_.join();
    }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    std::istream& in_;
    std::ostream& out_;
    std::atomic<std::ostream*> out_ptr_;
    std::mutex    write_mu_;
    std::thread   reader_;
    std::atomic<bool> running_{false};
    RpcEngine*    engine_{nullptr};
};

} // namespace mcp
