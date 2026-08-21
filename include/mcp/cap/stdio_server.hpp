// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/stdio_server.hpp — StdioServerProvider: a CapabilityProvider backed
// by an external MCP server spawned as a child process over stdio.
//
//   It owns the whole connection lifecycle: spawn the server, attach a
//   StdioTransport, run a typed Client, do the initialize handshake + initial
//   enumeration, and serve list()/execute()/resources()/prompts() through the
//   shared ClientProvider base. From the agent's side it's just another
//   provider — no MCP visible.
//
//   Construction connects synchronously (bounded by a handshake timeout) and
//   throws std::runtime_error if the server can't be spawned or doesn't
//   complete the handshake, so a host can catch-and-skip a bad server.
//
//   CONCURRENCY BY CONSTRUCTION. The mutable connection state (child process
//   + transport) lives inside Guarded<Conn>: the ONLY way to reach it is
//   Guarded::use(), which runs under the connection lock, and ~Guarded parks
//   until every in-flight use() drains. That makes the field crash class
//   "destructor tears the transport down while another thread is mid-
//   handshake inside execute()'s reconnect" UNREPRESENTABLE — there is no
//   unlocked path to the state for a destructor to race, and no lock a
//   future edit can forget to take.
//
//   POSIX-only (needs ChildProcess). Guarded by MCP_CAP_HAVE_PROCESS.
//
#pragma once

#include <mcp/cap/client_provider.hpp>
#include <mcp/cap/process.hpp>

#if MCP_CAP_HAVE_PROCESS

#include <mcp/cap/guarded.hpp>
#include <mcp/stdio.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp::cap {

class StdioServerProvider final : public ClientProvider {
public:
    struct Config {
        std::string              name;        // origin label suffix → "mcp:<name>"
        ChildProcess::Spawn      spawn;       // command + args + env
        Implementation           client_info{"mcp-cpp", "0.1"};
        std::chrono::milliseconds handshake_timeout{10'000};
        std::chrono::milliseconds call_timeout{60'000};
        ClientProvider::Integration integration;
    };

    explicit StdioServerProvider(Config cfg) : cfg_(std::move(cfg)) {
        conn_.use([&](Conn& c) { start_(c); });
    }

    [[nodiscard]] Result execute(const Request& req) override {
        // The reconnect (spawn + seconds-long handshake) runs entirely under
        // the connection lock via use(). A concurrent destructor cannot free
        // the transport/engine mid-handshake: ~Guarded blocks until this
        // use() returns. (Field SIGSEGV, crash report 2026-08-16.)
        if (auto err = conn_.use([&](Conn& c) -> std::string {
                if (alive_(c) && !connection_poisoned()) return {};   // healthy
                try {
                    teardown_(c);
                    start_(c);
                    if (on_list_changed_) on_list_changed_();
                } catch (const std::exception& error) {
                    return std::string{"mcp reconnect failed: "} + error.what();
                }
                return {};
            });
            !err.empty())
            return Result::error(std::move(err));
        return ClientProvider::execute(req);
    }

    // NOTE: no hand-written destructor. ~Guarded<Conn> parks until in-flight
    // use() calls drain, then Conn's members tear down in declaration order
    // (transport joins its reader first — see Conn) — the safe order is the
    // DECLARATION order, not a comment.
    ~StdioServerProvider() override {
        // The base's client_ (engine) must outlive the transport reader.
        // Stop the reader while the engine is still alive; ~Guarded makes
        // this wait for any concurrent execute()/reconnect first.
        conn_.use([&](Conn& c) { teardown_(c); });
    }

    StdioServerProvider(const StdioServerProvider&)            = delete;
    StdioServerProvider& operator=(const StdioServerProvider&) = delete;

    [[nodiscard]] bool alive() const noexcept override {
        try {
            return conn_.use([&](const Conn& c) { return alive_(c); });
        } catch (...) { return false; }
    }

protected:
    void on_teardown() noexcept override {
        // Reached from the base's connect() failure path — which only ever
        // runs INSIDE one of our use() calls (construction / reconnect), so
        // the recursive lock re-enters instead of deadlocking.
        try {
            conn_.use([&](Conn& c) { stop_reader_(c); });
        } catch (...) {}
    }

private:
    // The connection state. Declaration order is the teardown contract:
    // members destroy bottom-up, so `transport` (whose dtor joins the reader
    // thread) dies BEFORE `proc` (whose dtor reaps the child) — and both die
    // after ~StdioServerProvider already stopped the reader against the
    // still-alive engine.
    struct Conn {
        std::unique_ptr<ChildProcess>   proc;
        std::unique_ptr<StdioTransport> transport;
    };

    [[nodiscard]] static bool alive_(const Conn& c) noexcept {
        return c.proc && c.proc->alive();
    }

    void start_(Conn& c) {
        c.proc      = std::make_unique<ChildProcess>(cfg_.spawn);
        c.transport = std::make_unique<StdioTransport>(c.proc->out(), c.proc->in());
        auto client = std::make_unique<Client>(c.transport->sink());
        c.transport->start(client->engine());
        connect(cfg_.name, std::move(client), cfg_.client_info,
                cfg_.handshake_timeout, cfg_.call_timeout, cfg_.integration);
    }

    // Stop the transport's reader thread while the engine still exists.
    // Order is load-bearing:
    //   1. close_stdin() — EOF the child → it exits → its stdout closes →
    //      the reader's getline returns EOF, so the join won't block.
    //   2. terminate() — makes the join BOUNDED. A misbehaving child that
    //      ignores EOF would otherwise wedge the join forever (observed in
    //      the field as a quit-path hang). SIGTERM→SIGKILL guarantees the
    //      child dies and the reader unblocks; a well-behaved child already
    //      exited on EOF, so this is a no-op for it.
    //   3. transport.reset() — joins the reader thread.
    static void stop_reader_(Conn& c) noexcept {
        // Invalidate the transport's output pointer FIRST: once this is
        // null, sink() silently drops any write the reader thread makes
        // while we're shutting down (e.g. a notification handler that
        // tries to send a reply). Without this, the reader could dereference
        // a dangling ostream& after close_stdin/terminate closes the FD.
        if (c.transport) c.transport->invalidate_output();
        if (c.proc) {
            c.proc->close_stdin();
            c.proc->terminate();
        }
        c.transport.reset();
    }

    void teardown_(Conn& c) noexcept {
        stop_reader_(c);    // 1–3: reader stopped against the live engine
        reset_client();     // 4. now no thread touches the engine
        c.proc.reset();     // 5. reap the child + close the read FD
    }

    Config        cfg_;
    Guarded<Conn> conn_;
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
