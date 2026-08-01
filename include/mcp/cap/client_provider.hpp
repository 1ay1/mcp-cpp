// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/client_provider.hpp — ClientProvider: a CapabilityProvider backed by
// a connected mcp::Client over ANY transport.
//
//   The whole MCP-client lifecycle — initialize handshake, tools/resources/
//   prompts enumeration (with pagination), tools/call, resources/read,
//   prompts/get, and the *_list_changed refresh — is transport-agnostic: it
//   only ever talks to an mcp::RpcEngine through an mcp::Client. The transport
//   (stdio child process, Streamable HTTP, …) differs only in HOW frames move.
//
//   So this base owns everything EXCEPT the transport. A concrete provider:
//     1. builds its transport + an mcp::Client wired to it,
//     2. hands the Client to connect(), which runs the handshake + initial
//        enumeration,
//     3. implements alive() (is the connection still up?) and on_teardown()
//        (stop the transport before the Client is destroyed).
//
//   StdioServerProvider and the host's HTTP provider are both ~30-line
//   subclasses over this.
//
#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/client.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::cap {

class ClientProvider : public CapabilityProvider {
public:
    struct Integration {
        std::vector<Root> roots;
        std::function<void(const ResourceUpdatedParams&)> on_resource_updated;
        std::function<void(const LoggingMessageParams&)> on_log;
    };

    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }

    [[nodiscard]] std::vector<Tool> list() const override {
        std::lock_guard<std::mutex> lk(state_mu_); return tools_;
    }
    [[nodiscard]] const ServerCapabilities& server_capabilities() const noexcept { return server_caps_; }

    // ── tools ────────────────────────────────────────────────────────────
    void refresh_tools() {
        std::vector<Tool> all;
        Maybe<std::string> cursor = Nothing;
        Maybe<std::int64_t> ttl;
        do {
            ListToolsResult res = client_->list_tools(cursor).get();
            for (auto& t : res.tools) all.push_back(std::move(t));
            cursor = res.nextCursor;
            if (res.ttlMs.has_value()) ttl = res.ttlMs;   // first page's hint
        } while (cursor.has_value());
        std::lock_guard<std::mutex> lk(state_mu_);
        tools_ = std::move(all);
        tools_fresh_until_ = deadline_from_ttl(ttl);
    }
    // True while a cached tools list is still within its server-declared TTL —
    // callers may skip a redundant refresh. Always false when no TTL was given.
    [[nodiscard]] bool tools_cache_fresh() const {
        std::lock_guard<std::mutex> lk(state_mu_);
        return tools_fresh_until_.time_since_epoch().count() != 0 &&
               std::chrono::steady_clock::now() < tools_fresh_until_;
    }
    // Refresh only if the cache TTL has lapsed (or none was advertised).
    void refresh_tools_if_stale() { if (!tools_cache_fresh()) refresh_tools(); }
    void refresh() { refresh_tools(); }   // back-compat alias

    [[nodiscard]] Result execute(const Request& req) override {
        if (poisoned_.load(std::memory_order_acquire))
            return Result::error("mcp connection requires reconnect after cancellation");
        if (!alive()) return Result::error("mcp server '" + origin_ + "' is not running");
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            {
                std::lock_guard<std::mutex> progress_lock(progress_mu_);
                active_progress_ = req.progress;
            }
            auto clear_progress = std::shared_ptr<void>{nullptr, [this](void*) {
                std::lock_guard<std::mutex> lock(progress_mu_);
                active_progress_ = {};
            }};
            // call_tool_interactive drives MCP 2026-07-28 MRTR: if the remote
            // server answers input_required (asking us to sample / elicit /
            // list roots), we fulfil it locally with this client's handlers
            // and retry — transparently to the agent loop. Falls back to a
            // single round for a legacy server that never asks.
            if (!req.cancelled)
                return result_from_call(client_->call_tool_interactive(req.tool, req.args));

            auto pending = std::async(std::launch::async, [this, tool = req.tool, args = req.args] {
                return client_->call_tool_interactive(tool, args);
            });
            using namespace std::chrono_literals;
            while (pending.wait_for(20ms) != std::future_status::ready) {
                if (!req.cancelled()) continue;
                poisoned_.store(true, std::memory_order_release);
                client_->engine().on_transport_closed("tool call cancelled");
                break;
            }
            return result_from_call(pending.get());
        } catch (const std::exception& e) {
            return Result::error(std::string{"mcp call failed: "} + e.what());
        } catch (...) {
            return Result::error("mcp call failed");
        }
    }

    // ── resources ────────────────────────────────────────────────────────
    void refresh_resources() {
        std::vector<Resource> all;
        Maybe<std::string> cursor = Nothing;
        do {
            ListResourcesResult res = client_->list_resources(cursor).get();
            for (auto& r : res.resources) all.push_back(std::move(r));
            cursor = res.nextCursor;
        } while (cursor.has_value());
        std::vector<ResourceTemplate> tpls;
        try {
            cursor = Nothing;
            do {
                ListResourceTemplatesResult res = client_->list_resource_templates(cursor).get();
                for (auto& r : res.resourceTemplates) tpls.push_back(std::move(r));
                cursor = res.nextCursor;
            } while (cursor.has_value());
        } catch (...) { /* templates optional */ }
        std::lock_guard<std::mutex> lk(state_mu_);
        resources_ = std::move(all);
        resource_templates_ = std::move(tpls);
    }
    [[nodiscard]] std::vector<Resource> resources() const override {
        std::lock_guard<std::mutex> lk(state_mu_); return resources_;
    }
    [[nodiscard]] std::vector<ResourceTemplate> resource_templates() const override {
        std::lock_guard<std::mutex> lk(state_mu_); return resource_templates_;
    }
    [[nodiscard]] bool read_resource(const std::string& uri,
                                     std::vector<ResourceContents>& out,
                                     std::string& err) override {
        if (!alive()) { err = "mcp server '" + origin_ + "' is not running"; return false; }
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            ReadResourceResult res = client_->read_resource(uri).get();
            out = std::move(res.contents);
            return true;
        } catch (const std::exception& e) { err = std::string{"resources/read failed: "} + e.what(); }
          catch (...) { err = "resources/read failed"; }
        return false;
    }

    // ── prompts ──────────────────────────────────────────────────────────
    void refresh_prompts() {
        std::vector<Prompt> all;
        Maybe<std::string> cursor = Nothing;
        do {
            ListPromptsResult res = client_->list_prompts(cursor).get();
            for (auto& p : res.prompts) all.push_back(std::move(p));
            cursor = res.nextCursor;
        } while (cursor.has_value());
        std::lock_guard<std::mutex> lk(state_mu_); prompts_ = std::move(all);
    }
    [[nodiscard]] std::vector<Prompt> prompts() const override {
        std::lock_guard<std::mutex> lk(state_mu_); return prompts_;
    }
    [[nodiscard]] bool get_prompt(const std::string& name,
                                  const std::vector<std::pair<std::string, std::string>>& args,
                                  GetPromptResult& out,
                                  std::string& err) override {
        if (!alive()) { err = "mcp server '" + origin_ + "' is not running"; return false; }
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            GetPromptParams p;
            p.name = name;
            if (!args.empty()) p.arguments = args;
            out = client_->get_prompt(p).get();
            return true;
        } catch (const std::exception& e) { err = std::string{"prompts/get failed: "} + e.what(); }
          catch (...) { err = "prompts/get failed"; }
        return false;
    }

protected:
    // A subclass builds the Client (wired to its transport) and calls connect()
    // from its constructor. `handshake_timeout`/`call_timeout` arm the engine's
    // deadline monitor (see StdioServerProvider for why .get() is the only safe
    // wait). On any handshake failure, connect() invokes on_teardown() and
    // rethrows — so the subclass can tear its transport down in the safe order.
    void connect(const std::string& name,
                 std::unique_ptr<Client> client,
                 Implementation client_info,
                 std::chrono::milliseconds handshake_timeout,
                 std::chrono::milliseconds call_timeout,
                 Integration integration = {}) {
        origin_ = "mcp:" + name;
        poisoned_.store(false, std::memory_order_release);
        client_ = std::move(client);

        ClientHandlers handlers;
        const bool has_roots = !integration.roots.empty();
        if (has_roots) {
            auto roots = integration.roots;
            handlers.on_list_roots = [roots = std::move(roots)] {
                return ListRootsResult{roots, Json::object()};
            };
        }
        handlers.on_progress = [this](const ProgressParams& update) {
            std::function<void(std::string_view)> sink;
            {
                std::lock_guard<std::mutex> lock(progress_mu_);
                sink = active_progress_;
            }
            if (!sink) return;
            if (update.message && !update.message->empty()) {
                sink(*update.message);
                return;
            }
            std::string text = "MCP progress: " + std::to_string(update.progress);
            if (update.total) text += " / " + std::to_string(*update.total);
            sink(text);
        };
        handlers.on_resource_updated = std::move(integration.on_resource_updated);
        handlers.on_log = std::move(integration.on_log);
        client_->set_handlers(std::move(handlers));

        ClientCapabilities client_caps;
        if (has_roots)
            client_caps.roots = RootsCapability{true};

        // *_list_changed → refresh + fire host callback. Installed via the
        // engine directly (the Client was constructed before we had `this`).
        client_->engine().on_notification(std::string(method::ToolsListChanged),
            [this](const Json&) { try { refresh_tools(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });
        client_->engine().on_notification(std::string(method::ResourcesListChanged),
            [this](const Json&) { try { refresh_resources(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });
        client_->engine().on_notification(std::string(method::PromptsListChanged),
            [this](const Json&) { try { refresh_prompts(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });

        client_->set_default_timeout(handshake_timeout);

        // MCP 2026-07-28 stateless core (DUAL-ERA): attach the modern
        // per-request protocol metadata (protocolVersion + clientInfo +
        // clientCapabilities) to EVERY subsequent request. A modern server
        // then authenticates/versions each request independently — no session
        // needed. A legacy server simply ignores the unknown `_meta` keys, so
        // we ALSO still perform the legacy initialize handshake below for
        // backward compatibility. Enabling this before initialize means even
        // the handshake requests carry the modern metadata.
        client_->enable_modern_metadata(client_info, client_caps);

        // MCP 2026-07-28: prefer the stateless `server/discover` step over the
        // legacy initialize handshake. If the server answers it, we learn its
        // capabilities in ONE round-trip with no session. A legacy server
        // replies MethodNotFound — we fall through to initialize. Either way we
        // end up with server_caps_ populated.
        bool discovered = false;
        try {
            DiscoverResult disc = client_->discover().get();
            server_caps_ = disc.capabilities;
            discovered   = true;
        } catch (...) {
            discovered = false;   // legacy server (MethodNotFound) or transport hiccup
        }

        try {
            if (!discovered) {
                // Legacy path: full initialize handshake.
                InitializeResult init = client_->initialize(client_info).get();
                server_caps_ = init.capabilities;
                client_->initialized();
            }
            refresh_tools();
            if (server_caps_.resources.has_value()) { try { refresh_resources(); } catch (...) {} }
            if (server_caps_.prompts.has_value())   { try { refresh_prompts();   } catch (...) {} }
        } catch (...) {
            on_teardown();
            client_.reset();
            throw;
        }
        client_->set_default_timeout(call_timeout);
    }

    // Subclass hooks.
    [[nodiscard]] virtual bool alive() const noexcept = 0;
    // Stop the transport's reader so no thread touches the engine, THEN it's
    // safe to drop client_. Called from connect()'s failure path and from the
    // subclass destructor (which must also reset client_ AFTER this).
    virtual void on_teardown() noexcept {}

    [[nodiscard]] bool connection_poisoned() const noexcept {
        return poisoned_.load(std::memory_order_acquire);
    }

    Client*     client_ptr()  noexcept { return client_.get(); }
    void        reset_client() noexcept { client_.reset(); }

    // Convert a server-declared ttlMs into a steady-clock deadline. A zero /
    // absent TTL yields the epoch (interpreted as "no cache" by tools_cache_fresh).
    static std::chrono::steady_clock::time_point deadline_from_ttl(Maybe<std::int64_t> ttl) {
        if (!ttl.has_value() || *ttl <= 0) return {};
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(*ttl);
    }

    std::string                 origin_;
    std::unique_ptr<Client>     client_;
    ServerCapabilities          server_caps_;
    std::vector<Tool>           tools_;
    std::chrono::steady_clock::time_point tools_fresh_until_{};  // 2026-07-28 cache TTL
    std::vector<Resource>       resources_;
    std::vector<ResourceTemplate> resource_templates_;
    std::vector<Prompt>         prompts_;
    mutable std::mutex          state_mu_;  // guards cached lists (reader writes)
    std::mutex                  call_mu_;   // serialize tool/resource/prompt calls
    std::mutex                  progress_mu_;
    std::function<void(std::string_view)> active_progress_;
    std::atomic<bool> poisoned_{false};
};

} // namespace mcp::cap
