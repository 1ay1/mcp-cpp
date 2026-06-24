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
    };

    explicit StdioServerProvider(Config cfg) {
        proc_      = std::make_unique<ChildProcess>(cfg.spawn);
        transport_ = std::make_unique<StdioTransport>(proc_->out(), proc_->in());
        auto client = std::make_unique<Client>(transport_->sink());
        // IMPORTANT: Client::request() wraps results in std::async(deferred),
        // so future::wait_for() returns `deferred` (never `ready`) and a manual
        // timeout poll is meaningless. Instead the engine's deadline monitor
        // (armed by set_default_timeout in connect()) makes a late .get() throw
        // RpcError(Timeout). So everywhere we just .get().
        transport_->start(client->engine());

        // From here the reader thread is LIVE and references the engine. If the
        // handshake throws, connect() calls on_teardown() (stop the reader),
        // then resets the client in the SAFE order before rethrowing.
        connect(cfg.name, std::move(client), std::move(cfg.client_info),
                cfg.handshake_timeout, cfg.call_timeout);
    }

    ~StdioServerProvider() override { teardown_(); }

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
        //   2. transport_.reset() — joins the reader thread.
        if (proc_) proc_->close_stdin();
        transport_.reset();
    }

private:
    void teardown_() noexcept {
        on_teardown();      // stop reader (steps 1+2)
        reset_client();     // 3. now no thread touches the engine
        proc_.reset();      // 4. reap the child + close the read FD
    }

    std::unique_ptr<ChildProcess>   proc_;
    std::unique_ptr<StdioTransport> transport_;
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
