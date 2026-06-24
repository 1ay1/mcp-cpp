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
        : origin_("mcp:" + cfg.name),
          call_timeout_(cfg.call_timeout) {
        proc_      = std::make_unique<ChildProcess>(cfg.spawn);
        transport_ = std::make_unique<StdioTransport>(proc_->out(), proc_->in());
        client_    = std::make_unique<Client>(transport_->sink());
        transport_->start(client_->engine());

        // initialize handshake (bounded).
        auto init = client_->initialize(cfg.client_info);
        if (init.wait_for(cfg.handshake_timeout) != std::future_status::ready)
            throw std::runtime_error("mcp::cap: '" + cfg.name + "' initialize timed out");
        (void)init.get();                  // throws on RPC error
        client_->initialized();

        // tools/list (bounded). Cached; re-fetch via refresh().
        refresh(cfg.handshake_timeout);
    }

    ~StdioServerProvider() override {
        // client → transport → proc (stop issuing, stop reader, EOF the child).
        client_.reset();
        transport_.reset();
        proc_.reset();
    }

    StdioServerProvider(const StdioServerProvider&)            = delete;
    StdioServerProvider& operator=(const StdioServerProvider&) = delete;

    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }
    [[nodiscard]] std::vector<Tool> list() const override { return tools_; }

    [[nodiscard]] bool alive() const noexcept { return proc_ && proc_->alive(); }

    // Re-query tools/list (e.g. after a tools/list_changed notification).
    void refresh(std::chrono::milliseconds timeout = std::chrono::milliseconds{10'000}) {
        auto fut = client_->list_tools();
        if (fut.wait_for(timeout) != std::future_status::ready)
            throw std::runtime_error("mcp::cap: '" + origin_ + "' tools/list timed out");
        ListToolsResult res = fut.get();
        tools_ = std::move(res.tools);
    }

    [[nodiscard]] Result execute(const Request& req) override {
        if (!alive())
            return Result::error("mcp server '" + origin_ + "' is not running");
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            auto fut = client_->call_tool(req.tool, req.args);
            if (fut.wait_for(call_timeout_) != std::future_status::ready)
                return Result::error("mcp tool '" + req.tool + "' timed out");
            return result_from_call(fut.get());
        } catch (const std::exception& e) {
            return Result::error(std::string{"mcp call failed: "} + e.what());
        } catch (...) {
            return Result::error("mcp call failed");
        }
    }

private:
    std::string                     origin_;
    std::chrono::milliseconds       call_timeout_;
    std::unique_ptr<ChildProcess>   proc_;
    std::unique_ptr<StdioTransport> transport_;
    std::unique_ptr<Client>         client_;
    std::vector<Tool>               tools_;
    std::mutex                      call_mu_;   // serialize tools/call on one transport
};

} // namespace mcp::cap

#endif // MCP_CAP_HAVE_PROCESS
