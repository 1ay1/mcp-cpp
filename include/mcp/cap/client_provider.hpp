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

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::cap {

class ClientProvider : public CapabilityProvider {
public:
    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }

    [[nodiscard]] std::vector<Tool> list() const override {
        std::lock_guard<std::mutex> lk(state_mu_); return tools_;
    }
    [[nodiscard]] const ServerCapabilities& server_capabilities() const noexcept { return server_caps_; }

    // ── tools ────────────────────────────────────────────────────────────
    void refresh_tools() {
        std::vector<Tool> all;
        Maybe<std::string> cursor = Nothing;
        do {
            ListToolsResult res = client_->list_tools(cursor).get();
            for (auto& t : res.tools) all.push_back(std::move(t));
            cursor = res.nextCursor;
        } while (cursor.has_value());
        std::lock_guard<std::mutex> lk(state_mu_); tools_ = std::move(all);
    }
    void refresh() { refresh_tools(); }   // back-compat alias

    [[nodiscard]] Result execute(const Request& req) override {
        if (!alive()) return Result::error("mcp server '" + origin_ + "' is not running");
        try {
            std::lock_guard<std::mutex> lk(call_mu_);
            return result_from_call(client_->call_tool(req.tool, req.args).get());
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
                 std::chrono::milliseconds call_timeout) {
        origin_ = "mcp:" + name;
        client_ = std::move(client);

        // *_list_changed → refresh + fire host callback. Installed via the
        // engine directly (the Client was constructed before we had `this`).
        client_->engine().on_notification(std::string(method::ToolsListChanged),
            [this](const Json&) { try { refresh_tools(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });
        client_->engine().on_notification(std::string(method::ResourcesListChanged),
            [this](const Json&) { try { refresh_resources(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });
        client_->engine().on_notification(std::string(method::PromptsListChanged),
            [this](const Json&) { try { refresh_prompts(); } catch (...) {} if (on_list_changed_) on_list_changed_(); });

        client_->set_default_timeout(handshake_timeout);
        try {
            InitializeResult init = client_->initialize(std::move(client_info)).get();
            server_caps_ = init.capabilities;
            client_->initialized();
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

    Client*     client_ptr()  noexcept { return client_.get(); }
    void        reset_client() noexcept { client_.reset(); }

    std::string                 origin_;
    std::unique_ptr<Client>     client_;
    ServerCapabilities          server_caps_;
    std::vector<Tool>           tools_;
    std::vector<Resource>       resources_;
    std::vector<ResourceTemplate> resource_templates_;
    std::vector<Prompt>         prompts_;
    mutable std::mutex          state_mu_;  // guards cached lists (reader writes)
    std::mutex                  call_mu_;   // serialize tool/resource/prompt calls
};

} // namespace mcp::cap
