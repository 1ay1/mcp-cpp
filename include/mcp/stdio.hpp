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
//     • a reader job, run on the installed mcp::Runtime, that pumps lines
//       into the engine
//
//   The reader stops when the input descriptor returns EOF.
//
#pragma once

#include <mcp/rpc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
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

    // Run the read pump as a job on the installed mcp::Runtime. The pump
    // ends on EOF or stop(). On natural EOF (peer closed) the engine's
    // on_transport_closed() fires, failing in-flight requests with
    // errc::ConnectionLost and invoking its error callback.
    void start(RpcEngine& engine) {
        engine_ = &engine;
        running_.store(true, std::memory_order_release);
        // The job holds SHARED guards, never `this` past a stop request: if
        // stop() has to give up a reader wedged in getline (the peer never
        // closed the stream), the straggler must not touch a destroyed
        // transport or engine. `alive_` goes false in stop().
        alive_ = std::make_shared<std::atomic<bool>>(true);
        auto alive = alive_;
        // `in_` is the caller's stream; it outlives the transport by contract
        // (see the constructor), so the reader may keep reading it.
        std::istream* in = &in_;
        reader_ = runtime().spawn("mcp.stdio.reader",
            [this, in, &engine, alive](std::stop_token st) {
                std::string line;
                while (!st.stop_requested()) {
                    if (!std::getline(*in, line)) break;            // EOF/error
                    if (!alive->load(std::memory_order_acquire)) return;
                    if (!line.empty()) {
                        try { engine.feed_line(line); }
                        catch (...) { /* never let one frame kill the pump */ }
                    }
                }
                // Past here `this` is touched, so only while still alive: a
                // given-up straggler returns before reaching it.
                if (!alive->load(std::memory_order_acquire)) return;
                // Report a closed stream only if it closed on its own, not
                // because stop() asked.
                if (running_.exchange(false, std::memory_order_acq_rel)
                    && !st.stop_requested())
                    engine.on_transport_closed("eof");
            });
    }

    // Wait until the reader has finished on its own (EOF).
    void join() {
        if (reader_) reader_->wait();
        reader_.reset();
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (!reader_) return;
        // The reader may be blocked in std::getline, which no token can
        // interrupt; it only wakes when the peer closes the stream. Callers
        // close/terminate the peer first (cap/stdio_server.hpp does), which is
        // the fast path. If it is still wedged after a short grace, sever it
        // (alive_ = false: any late callback is a no-op) and let the runtime
        // give it up rather than hang teardown.
        if (alive_) alive_->store(false, std::memory_order_release);
        if (!reader_->stop(std::chrono::milliseconds(500)))
            std::fprintf(stderr,
                "mcp: StdioTransport::stop() gave up a reader still blocked in "
                "getline. Close or terminate the peer before stop().\n");
        reader_.reset();
    }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    std::istream& in_;
    std::ostream& out_;
    std::atomic<std::ostream*> out_ptr_;
    std::mutex    write_mu_;
    std::unique_ptr<Runtime::Job> reader_;
    std::atomic<bool> running_{false};
    RpcEngine*    engine_{nullptr};
    // Shared with the reader job so a given-up straggler (wedged in getline)
    // is severed: `alive_` false suppresses any late callback.
    std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace mcp
