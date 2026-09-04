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
#include <chrono>
#include <cstdio>
#include <istream>
#include <memory>
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
        alive_ = std::make_shared<std::atomic<bool>>(true);
        reader_done_ = std::make_shared<std::atomic<bool>>(false);
        // Capture SHARED guards, not `this`: if stop() has to detach a reader
        // wedged in a blocking getline (peer never closed the stream), the
        // detached thread must NOT touch a destroyed transport/engine. `alive_`
        // is flipped false in stop()/dtor, so any late feed_line/close is
        // suppressed; `reader_done_` lets stop() observe a natural exit.
        auto alive = alive_;
        auto done  = reader_done_;
        reader_ = std::thread([this, &engine, alive, done]{
            std::string line;
            while (running_.load(std::memory_order_acquire)) {
                if (!std::getline(in_, line)) break;        // EOF or error
                if (!alive->load(std::memory_order_acquire)) break;  // detached
                if (!line.empty()) {
                    try { engine.feed_line(line); }
                    catch (...) { /* never let one frame kill the pump */ }
                }
            }
            const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
            // Only surface a transport-closed event if we stopped because the
            // stream ended, not because stop() was called deliberately — and
            // only if the transport is still alive (not a detached straggler).
            if (was_running && alive->load(std::memory_order_acquire))
                engine.on_transport_closed("eof");
            done->store(true, std::memory_order_release);
        });
    }

    // Wait until the reader thread exits.
    void join() {
        if (reader_.joinable()) reader_.join();
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (!reader_.joinable()) return;

        // The reader may be blocked in std::getline, which no flag can
        // interrupt — it only wakes when the peer closes the stream (well-
        // behaved callers close_stdin()/terminate() the child first, which is
        // the fast path here). If it hasn't exited within a short grace window,
        // sever the reader from this transport (alive_=false, so a late
        // feed_line/on_transport_closed is a no-op) and DETACH it rather than
        // block teardown forever. A detached thread parked on a dead stream is
        // harmless; the process reclaims it at exit. Same deadline-then-detach
        // discipline the HTTP prewarm-dial teardown uses.
        constexpr auto kGrace = std::chrono::milliseconds(500);
        const auto deadline = std::chrono::steady_clock::now() + kGrace;
        while (reader_done_ && !reader_done_->load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (reader_done_ && reader_done_->load(std::memory_order_acquire)) {
            reader_.join();
        } else {
            // The reader is still wedged in getline after the grace window —
            // the peer stream was never closed. This is a CALLER BUG: a stdio
            // transport must have its child terminated (close_stdin + kill, as
            // cap/stdio_server.hpp's stop_reader_ does) before stop(), so the
            // reader wakes on EOF. We sever + detach so teardown never hangs;
            // warn so the misuse is visible rather than a silent leak.
            std::fprintf(stderr,
                "mcp: StdioTransport::stop() detached a reader still blocked in "
                "getline — close/terminate the peer before stop() to avoid a "
                "leaked thread.\n");
            if (alive_) alive_->store(false, std::memory_order_release);
            reader_.detach();
        }
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
    // Shared with the reader thread so a DETACHED straggler (wedged in
    // getline) can be severed safely: `alive_` false suppresses its late
    // callbacks, `reader_done_` lets stop() see a natural exit vs a wedge.
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> reader_done_;
};

} // namespace mcp
