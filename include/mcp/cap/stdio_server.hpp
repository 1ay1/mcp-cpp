// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/stdio_server.hpp — StdioServerProvider: a CapabilityProvider backed
// by an external MCP server spawned as a child process over stdio.
//
//   It owns the whole connection lifecycle: spawn the server, attach a
//   StdioTransport, run a typed Client, do the initialize handshake + a
//   tools/list, and serve execute() by issuing tools/call. From the agent's
//   side it's just another provider — list() / execute(), no MCP visible.
//
//   Construction connects synchronously (bounded by a handshake timeout) and
//   throws std::runtime_error if the server can't be spawned or doesn't
//   complete the handshake, so a host can catch-and-skip a bad server.
//
//   POSIX-only (needs ChildProcess). Guarded by MCP_CAP_HAVE_PROCESS.
//
#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/cap/process.hpp>

#if MCP_CAP_HAVE_PROCESS

#include <mcp/client.hpp>
#include <mcp/stdio.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::cap {

class StdioServerProvider final : public CapabilityProvider {
public:
    struct Config {
        std::string              name;        // origin label suffix → "mcp:<name>"
        ChildProcess::Spawn      spawn;       // command + args + env
        Implementation           client_info{"mcp-cpp", "0.1"};
        std::chrono::milliseconds handshake_timeout{10'000};
        std::chrono::milliseconds call_timeout{60'000};
    };

    explicit StdioServerProvider(Config cfg)
        : origin_("mcp:" + cfg.name) {
        proc_      = std::make_unique<ChildProcess>(cfg.spawn);
        transport_ = std::make_unique<StdioTransport>(proc_->out(), proc_->in());
        client_    = std::make_unique<Client>(transport_->sink());
        // IMPORTANT: Client::request() wraps results in std::async(deferred),
        // so future::wait_for() on them returns `deferred` (never `ready`) and
        // a manual timeout poll is meaningless. Instead we arm the engine's
        // OWN deadline monitor via a default timeout: a late response makes
        // .get() throw RpcError(Timeout). So everywhere below we just .get().
        client_->set_default_timeout(cfg.handshake_timeout);
        transport_->start(client_->engine());

        // From here the reader thread is LIVE and references client_->engine().
        // If the handshake throws, normal member-destruction order would drop
        // client_ BEFORE transport_ (reverse declaration order), leaving the
        // reader thread to touch a freed engine. So we guard the whole
        // handshake and tear down in the SAFE order on any failure.
        try {
            (void)client_->initialize(cfg.client_info).get();   // throws on RPC error / timeout
            client_->initialized();
            refresh();                                          // tools/list (cached)
        } catch (...) {
            teardown_();                       // stop reader, THEN drop client
            throw;
        }
        // Switch to the per-call timeout for steady-state tool calls.
        client_->set_default_timeout(cfg.call_timeout);
    }

    ~StdioServerProvider() override { teardown_(); }

    StdioServerProvider(const StdioServerProvider&)            = delete;
    StdioServerProvider& operator=(const StdioServerProvider&) = delete;

    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }
    [[nodiscard]] std::vector<Tool> list() const override { return tools_; }

    [[nodiscard]] bool alive() const noexcept { return proc_ && proc_->alive(); }

    // Re-query tools/list (e.g. after a tools/list_changed notification).
    // Blocks on the engine's default timeout; throws on timeout/RPC error.
    void refresh() {
        ListToolsResult res = client_->list_tools().get();
        tools_ = std::move(res.tools);
    }

    [[nodiscard]] Result execute(const Request& req) override {
        if (!alive())
            return Result::error("mcp server '" + origin_ + "' is not running");
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            // .get() blocks on the real promise; the engine's deadline monitor
            // (default timeout = call_timeout) throws RpcError on a late reply.
            return result_from_call(client_->call_tool(req.tool, req.args).get());
        } catch (const std::exception& e) {
            return Result::error(std::string{"mcp call failed: "} + e.what());
        } catch (...) {
            return Result::error("mcp call failed");
        }
    }

private:
    // Safe teardown, usable from both the destructor AND the constructor's
    // exception path. Order is load-bearing: the transport's reader thread
    // calls into client_->engine() on every inbound frame, so we must STOP
    // the reader before destroying the client.
    //   1. close_stdin() — EOF the child → it exits → stdout closes → the
    //      reader's getline returns EOF, so transport_.reset()'s join() won't
    //      block.
    //   2. transport_.reset() — joins the reader thread.
    //   3. client_.reset() — now no thread touches the engine.
    //   4. proc_.reset() — reap the child + close the read FD.
    void teardown_() noexcept {
        if (proc_) proc_->close_stdin();
        transport_.reset();
        client_.reset();
        proc_.reset();
    }

    std::string                     origin_;
    std::unique_ptr<ChildProcess>   proc_;
    std::unique_ptr<StdioTransport> transport_;
    std::unique_ptr<Client>         client_;
    std::vector<Tool>               tools_;
    std::mutex                      call_mu_;   // serialize tools/call on one transport
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
