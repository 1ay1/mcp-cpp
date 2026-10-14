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
//   tools()/prompts() ask the providers each time, so a changed list is seen
//   without being told. With always_namespace, dispatch() routes by the
//   origin prefix and never lists.
#pragma once

#include <mcp/cap/capability.hpp>

#include <memory>
#include <optional>
#include <tuple>
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
        std::shared_ptr<CapabilityProvider> provider;
        std::string bare;
        if (auto direct = by_origin(req.tool)) {
            std::tie(provider, bare) = std::move(*direct);
        } else {
            auto routes = tool_routes();
            auto it = routes.find(req.tool);
            if (it == routes.end())
                return Result::error("capability not found: '" + req.tool + "'");
            provider = it->second.provider;
            bare     = std::move(it->second.bare_name);
        }
        // Copy, then rename: every field (scope, elicit, any added later)
        // reaches the provider, not just the ones listed here.
        Request fwd = req;
        fwd.tool = std::move(bare);
        return provider->execute(fwd);
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
        if (auto direct = by_origin(name))
            return direct->first->get_prompt(direct->second, args, out, err);
        auto routes = prompt_routes();
        auto it = routes.find(name);
        if (it == routes.end()) {
            err = "prompt not found: '" + name + "'";
            return false;
        }
        return it->second.provider->get_prompt(it->second.bare_name, args, out, err);
    }

private:
    // With always_namespace every exposed name is "<origin>__<name>", so the
    // origin alone picks the provider. No list() calls: for a remote server
    // that can be a blocking RPC, and one slow server must not hold up a
    // call to another. The provider rejects a name it doesn't have.
    [[nodiscard]] std::optional<std::pair<std::shared_ptr<CapabilityProvider>, std::string>>
    by_origin(std::string_view exposed) const {
        if (!always_namespace_) return std::nullopt;
        std::shared_ptr<CapabilityProvider> hit;
        std::size_t len = 0;
        for (const auto& p : providers_) {
            const std::string_view o = p->origin();
            // Longest origin wins if one is a prefix of another.
            if (exposed.size() > o.size() + 2 && exposed.starts_with(o)
                && exposed.substr(o.size(), 2) == "__" && (!hit || o.size() > len)) {
                hit = p; len = o.size();
            }
        }
        if (!hit) return std::nullopt;
        return std::pair{std::move(hit), std::string{exposed.substr(len + 2)}};
    }
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
