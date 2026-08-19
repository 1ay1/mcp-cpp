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
//   POSIX-only (needs ChildProcess). Guarded by MCP_CAP_HAVE_PROCESS.
//
#pragma once

#include <mcp/cap/client_provider.hpp>
#include <mcp/cap/process.hpp>

#if MCP_CAP_HAVE_PROCESS

#include <mcp/stdio.hpp>

#include <chrono>
#include <memory>
#include <mutex>
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
        start_();
    }

    [[nodiscard]] Result execute(const Request& req) override {
        std::lock_guard<std::mutex> lock(reconnect_mu_);
        if (!alive() || connection_poisoned()) {
            try {
                teardown_();
                reset_client();
                proc_.reset();
                start_();
                if (on_list_changed_) on_list_changed_();
            } catch (const std::exception& error) {
                return Result::error(std::string{"mcp reconnect failed: "} + error.what());
            }
        }
        return ClientProvider::execute(req);
    }

    ~StdioServerProvider() override {
        // Serialize with an in-flight execute()/reconnect. A tool-call worker
        // may be inside start_() → connect() → initialize() (a seconds-long
        // handshake) when the host drops its last pool reference at quit.
        // Destroying the transport/engine under that thread's feet is a
        // use-after-free (observed in the field: SIGSEGV in
        // RpcEngine::request on thread A while ~StdioServerProvider ran on
        // thread B). Taking reconnect_mu_ here parks the destructor until
        // the reconnect either finishes or fails; only then is teardown safe.
        std::lock_guard<std::mutex> lock(reconnect_mu_);
        teardown_();
    }

    StdioServerProvider(const StdioServerProvider&)            = delete;
    StdioServerProvider& operator=(const StdioServerProvider&) = delete;

    [[nodiscard]] bool alive() const noexcept override { return proc_ && proc_->alive(); }

protected:
    void on_teardown() noexcept override {
        // Order is load-bearing: the transport's reader thread calls into the
        // engine on every inbound frame, so STOP the reader before the client
        // is destroyed.
        //   1. close_stdin() — EOF the child → it exits → stdout closes → the
        //      reader's getline returns EOF, so transport_.reset()'s join()
        //      won't block.
        //   2. terminate() — makes step 3 BOUNDED. A misbehaving child that
        //      ignores EOF and keeps its stdout open would otherwise wedge
        //      the join forever (observed in the field: quit-path thread
        //      parked in pthread_join inside on_teardown). SIGTERM→SIGKILL
        //      guarantees the child dies, its stdout closes, and the reader
        //      unblocks within ChildProcess's own deadline. A well-behaved
        //      child has already exited on EOF, so this is a no-op for it.
        //   3. transport_.reset() — joins the reader thread.
        if (proc_) {
            proc_->close_stdin();
            proc_->terminate();
        }
        transport_.reset();
    }

private:
    void start_() {
        proc_      = std::make_unique<ChildProcess>(cfg_.spawn);
        transport_ = std::make_unique<StdioTransport>(proc_->out(), proc_->in());
        auto client = std::make_unique<Client>(transport_->sink());
        transport_->start(client->engine());
        connect(cfg_.name, std::move(client), cfg_.client_info,
                cfg_.handshake_timeout, cfg_.call_timeout, cfg_.integration);
    }

    void teardown_() noexcept {
        on_teardown();      // stop reader (steps 1+2)
        reset_client();     // 3. now no thread touches the engine
        proc_.reset();      // 4. reap the child + close the read FD
    }

    Config                           cfg_;
    std::mutex                       reconnect_mu_;
    std::unique_ptr<ChildProcess>   proc_;
    std::unique_ptr<StdioTransport> transport_;
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
