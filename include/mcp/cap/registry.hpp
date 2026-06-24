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

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp::cap {

class Registry {
public:
    explicit Registry(bool always_namespace = false)
        : always_namespace_(always_namespace) {}

    void add(std::shared_ptr<CapabilityProvider> provider) {
        if (provider) providers_.push_back(std::move(provider));
        dirty_ = true;
    }

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

private:
    struct Route {
        CapabilityProvider* provider = nullptr;
        std::string         bare_name;   // the provider's own tool name
        Tool                tool;        // descriptor (bare name inside)
    };

    void rebuild_() const {
        if (!dirty_) return;
        routes_.clear();

        // First pass: count bare-name multiplicity to decide namespacing.
        std::unordered_map<std::string, int> seen;
        for (const auto& p : providers_)
            for (const auto& t : p->list()) seen[t.name]++;

        for (const auto& p : providers_) {
            const std::string origin{p->origin()};
            for (auto& t : p->list()) {
                const bool collide = always_namespace_ || seen[t.name] > 1;
                std::string exposed = collide ? (origin + "__" + t.name) : t.name;
                routes_.emplace(std::move(exposed),
                                Route{p.get(), t.name, t});
            }
        }
        dirty_ = false;
    }

    bool always_namespace_ = false;
    std::vector<std::shared_ptr<CapabilityProvider>> providers_;
    mutable std::unordered_map<std::string, Route>   routes_;
    mutable bool                                     dirty_ = true;
};

} // namespace mcp::cap
