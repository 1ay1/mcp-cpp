// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/registry.hpp — Registry: the capability fan-in.
//
//   Holds N CapabilityProviders (local closures, spawned MCP servers, remote
//   HTTP servers, …). It presents the union of their tools to the agent and
//   routes execute() to the right provider — so the host application sees ONE
//   capability surface no matter how many heterogeneous sources back it.
//
//   Tool naming:
//     • A tool keeps its bare name when it's unambiguous across providers.
//     • When two providers expose the same name (or always, if you pass
//       always_namespace=true), tools are exposed as "<origin>__<name>" so
//       there's never a collision and the agent can see provenance.
//   dispatch() accepts either the bare or namespaced form and resolves it.
//
//   A Registry is a value with one owner. Build it (add providers), then read
//   it; if several threads read it while another adds, the host wraps it.
//   Routes are computed from the providers' current lists on each lookup, so
//   a provider whose list changed is seen without being told.
#pragma once

#include <mcp/cap/capability.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::cap {

class Registry {
public:
    explicit Registry(bool always_namespace = false)
        : always_namespace_(always_namespace) {}

    void add(std::shared_ptr<CapabilityProvider> provider) {
        if (provider) providers_.push_back(std::move(provider));
    }

    [[nodiscard]] std::size_t provider_count() const noexcept { return providers_.size(); }
    [[nodiscard]] const std::vector<std::shared_ptr<CapabilityProvider>>& providers() const noexcept {
        return providers_;
    }

    // The union of all tools, with names resolved per the namespacing policy.
    // Each returned Tool's `name` is the EXPOSED (possibly namespaced) name —
    // exactly what you advertise to the model and pass back to dispatch().
    [[nodiscard]] std::vector<Tool> tools() const {
        std::vector<Tool> out;
        for (auto& [exposed, route] : tool_routes()) {
            Tool t = std::move(route.tool);
            t.name = exposed;
            out.push_back(std::move(t));
        }
        return out;
    }

    // Route a request by its EXPOSED tool name (bare or "<origin>__<name>").
    // Never throws — an unknown tool yields Result::error.
    [[nodiscard]] Result dispatch(const Request& req) const {
        auto routes = tool_routes();
        auto it = routes.find(req.tool);
        if (it == routes.end())
            return Result::error("capability not found: '" + req.tool + "'");
        return it->second.provider->execute(Request{it->second.bare_name, req.args,
                                                    req.progress, req.cancelled, req.stop});
    }

    [[nodiscard]] Result dispatch(const std::string& tool, Json args = Json::object()) const {
        return dispatch(Request{tool, std::move(args)});
    }

    // ── Resources fan-in ────────────────────────────────────────────
    // Each Resource keeps its own URI (globally unique by construction), so no
    // namespacing is needed.
    [[nodiscard]] std::vector<Resource> resources() const {
        std::vector<Resource> out;
        for (const auto& p : providers_)
            for (auto& r : p->resources()) out.push_back(std::move(r));
        return out;
    }
    [[nodiscard]] std::vector<ResourceTemplate> resource_templates() const {
        std::vector<ResourceTemplate> out;
        for (const auto& p : providers_)
            for (auto& r : p->resource_templates()) out.push_back(std::move(r));
        return out;
    }
    // Read a resource by URI, routing to its owning provider. Falls back to
    // trying every provider if no provider lists the URI (templates).
    [[nodiscard]] bool read_resource(const std::string& uri,
                                     std::vector<ResourceContents>& out,
                                     std::string& err) const {
        for (const auto& p : providers_)
            for (const auto& r : p->resources())
                if (r.uri == uri) return p->read_resource(uri, out, err);
        for (const auto& p : providers_)
            if (p->read_resource(uri, out, err)) return true;
        if (err.empty()) err = "resource not found: '" + uri + "'";
        return false;
    }

    // ── Prompts fan-in ─────────────────────────────────────────────
    // Prompts are namespaced like tools, so two servers can both expose
    // "summarize".
    [[nodiscard]] std::vector<Prompt> prompts() const {
        std::vector<Prompt> out;
        for (auto& [exposed, route] : prompt_routes()) {
            Prompt p = std::move(route.prompt);
            p.name = exposed;
            out.push_back(std::move(p));
        }
        return out;
    }
    [[nodiscard]] bool get_prompt(const std::string& name,
                                  const std::vector<std::pair<std::string, std::string>>& args,
                                  GetPromptResult& out,
                                  std::string& err) const {
        auto routes = prompt_routes();
        auto it = routes.find(name);
        if (it == routes.end()) {
            err = "prompt not found: '" + name + "'";
            return false;
        }
        return it->second.provider->get_prompt(it->second.bare_name, args, out, err);
    }

private:
    struct Route {
        std::shared_ptr<CapabilityProvider> provider;
        std::string bare_name;   // the provider's own tool name
        Tool        tool;        // descriptor (bare name inside)
    };
    struct PromptRoute {
        std::shared_ptr<CapabilityProvider> provider;
        std::string bare_name;
        Prompt      prompt;
    };

    [[nodiscard]] std::unordered_map<std::string, Route> tool_routes() const {
        std::vector<std::vector<Tool>> lists;
        std::unordered_map<std::string, int> seen;
        for (const auto& p : providers_) {
            lists.push_back(p->list());
            for (const auto& t : lists.back()) seen[t.name]++;
        }
        std::unordered_map<std::string, Route> routes;
        for (std::size_t i = 0; i < providers_.size(); ++i) {
            const std::string origin{providers_[i]->origin()};
            for (auto& t : lists[i]) {
                const bool collide = always_namespace_ || seen[t.name] > 1;
                std::string exposed = collide ? (origin + "__" + t.name) : t.name;
                std::string bare = t.name;
                routes.emplace(std::move(exposed), Route{providers_[i], std::move(bare), std::move(t)});
            }
        }
        return routes;
    }

    [[nodiscard]] std::unordered_map<std::string, PromptRoute> prompt_routes() const {
        std::vector<std::vector<Prompt>> lists;
        std::unordered_map<std::string, int> seen;
        for (const auto& p : providers_) {
            lists.push_back(p->prompts());
            for (const auto& pr : lists.back()) seen[pr.name]++;
        }
        std::unordered_map<std::string, PromptRoute> routes;
        for (std::size_t i = 0; i < providers_.size(); ++i) {
            const std::string origin{providers_[i]->origin()};
            for (auto& pr : lists[i]) {
                const bool collide = always_namespace_ || seen[pr.name] > 1;
                std::string exposed = collide ? (origin + "__" + pr.name) : pr.name;
                std::string bare = pr.name;
                routes.emplace(std::move(exposed), PromptRoute{providers_[i], std::move(bare), std::move(pr)});
            }
        }
        return routes;
    }

    bool always_namespace_ = false;
    std::vector<std::shared_ptr<CapabilityProvider>> providers_;
};

} // namespace mcp::cap
