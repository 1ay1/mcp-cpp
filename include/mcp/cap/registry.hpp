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
#pragma once

#include <mcp/cap/capability.hpp>

#include <functional>
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
        if (provider) {
            // Forward each provider's list-changed up to the registry's own
            // subscriber, and mark the route cache dirty so the next tools()/
            // dispatch() rebuilds. The provider has already refreshed itself.
            provider->set_on_list_changed([this] {
                dirty_ = true;
                if (on_list_changed_) on_list_changed_();
            });
            providers_.push_back(std::move(provider));
        }
        dirty_ = true;
    }

    // Host subscribes to ANY provider's list-changing. Fires after the
    // provider refreshed + the route cache was invalidated.
    void set_on_list_changed(std::function<void()> fn) { on_list_changed_ = std::move(fn); }

    [[nodiscard]] std::size_t provider_count() const noexcept { return providers_.size(); }

    // The union of all tools, with names resolved per the namespacing policy.
    // Each returned Tool's `name` is the EXPOSED (possibly namespaced) name —
    // exactly what you advertise to the model and pass back to dispatch().
    [[nodiscard]] std::vector<Tool> tools() const {
        rebuild_();
        std::vector<Tool> out;
        out.reserve(routes_.size());
        for (const auto& [exposed, route] : routes_) {
            Tool t = route.tool;
            t.name = exposed;
            out.push_back(std::move(t));
        }
        return out;
    }

    // Route a request by its EXPOSED tool name (bare or "<origin>__<name>").
    // Never throws — an unknown tool yields Result::error.
    [[nodiscard]] Result dispatch(const Request& req) {
        rebuild_();
        auto it = routes_.find(req.tool);
        if (it == routes_.end())
            return Result::error("capability not found: '" + req.tool + "'");
        // Forward with the provider's UNQUALIFIED name (strip namespace).
        Request fwd{it->second.bare_name, req.args};
        return it->second.provider->execute(fwd);
    }

    // Convenience overload.
    [[nodiscard]] Result dispatch(const std::string& tool, Json args = Json::object()) {
        return dispatch(Request{tool, std::move(args)});
    }

    // ── Resources fan-in ────────────────────────────────────────────
    // The union of all providers' resources. Each Resource keeps its own URI
    // (globally unique by construction), so no namespacing is needed; we just
    // remember which provider owns each URI for read routing.
    [[nodiscard]] std::vector<Resource> resources() const {
        rebuild_();
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
    // trying every provider if the URI wasn't seen at list time (templates).
    [[nodiscard]] bool read_resource(const std::string& uri,
                                     std::vector<ResourceContents>& out,
                                     std::string& err) {
        rebuild_();
        if (auto it = resource_owner_.find(uri); it != resource_owner_.end())
            return it->second->read_resource(uri, out, err);
        for (const auto& p : providers_)
            if (p->read_resource(uri, out, err)) return true;
        if (err.empty()) err = "resource not found: '" + uri + "'";
        return false;
    }

    // ── Prompts fan-in ─────────────────────────────────────────────
    // Prompts are namespaced like tools (exposed name → owning provider +
    // bare name) so two servers can both expose "summarize".
    [[nodiscard]] std::vector<Prompt> prompts() const {
        rebuild_();
        std::vector<Prompt> out;
        out.reserve(prompt_routes_.size());
        for (const auto& [exposed, route] : prompt_routes_) {
            Prompt p = route.prompt;
            p.name = exposed;
            out.push_back(std::move(p));
        }
        return out;
    }
    [[nodiscard]] bool get_prompt(const std::string& name,
                                  const std::vector<std::pair<std::string, std::string>>& args,
                                  GetPromptResult& out,
                                  std::string& err) {
        rebuild_();
        auto it = prompt_routes_.find(name);
        if (it == prompt_routes_.end()) { err = "prompt not found: '" + name + "'"; return false; }
        return it->second.provider->get_prompt(it->second.bare_name, args, out, err);
    }

private:
    struct Route {
        CapabilityProvider* provider = nullptr;
        std::string         bare_name;   // the provider's own tool name
        Tool                tool;        // descriptor (bare name inside)
    };
    struct PromptRoute {
        CapabilityProvider* provider = nullptr;
        std::string         bare_name;
        Prompt              prompt;
    };

    void rebuild_() const {
        if (!dirty_) return;
        routes_.clear();
        prompt_routes_.clear();
        resource_owner_.clear();

        // First pass: count bare-name multiplicity to decide namespacing.
        std::unordered_map<std::string, int> seen;
        std::unordered_map<std::string, int> seen_prompt;
        for (const auto& p : providers_) {
            for (const auto& t : p->list()) seen[t.name]++;
            for (const auto& pr : p->prompts()) seen_prompt[pr.name]++;
        }

        for (const auto& p : providers_) {
            const std::string origin{p->origin()};
            for (auto& t : p->list()) {
                const bool collide = always_namespace_ || seen[t.name] > 1;
                std::string exposed = collide ? (origin + "__" + t.name) : t.name;
                routes_.emplace(std::move(exposed),
                                Route{p.get(), t.name, t});
            }
            for (auto& pr : p->prompts()) {
                const bool collide = always_namespace_ || seen_prompt[pr.name] > 1;
                std::string exposed = collide ? (origin + "__" + pr.name) : pr.name;
                prompt_routes_.emplace(std::move(exposed),
                                       PromptRoute{p.get(), pr.name, pr});
            }
            for (const auto& r : p->resources())
                resource_owner_.emplace(r.uri, p.get());
        }
        dirty_ = false;
    }

    bool always_namespace_ = false;
    std::function<void()> on_list_changed_;
    std::vector<std::shared_ptr<CapabilityProvider>> providers_;
    mutable std::unordered_map<std::string, Route>       routes_;
    mutable std::unordered_map<std::string, PromptRoute> prompt_routes_;
    mutable std::unordered_map<std::string, CapabilityProvider*> resource_owner_;
    mutable bool                                     dirty_ = true;
};

} // namespace mcp::cap
