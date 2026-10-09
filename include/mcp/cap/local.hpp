// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/local.hpp — LocalProvider: capabilities backed by in-process C++
// closures. The simplest CapabilityProvider, and the one that lets a host
// application expose ITS OWN tools through the same uniform interface MCP
// servers use — so there is exactly one capability abstraction in the system.
//
//   LocalProvider local{"local"};
//   local.add(my_tool_descriptor, [](const Json& args) -> cap::Result {
//       return cap::Result::ok("did the thing");
//   });
//   registry.add(local_shared_ptr);
//
#pragma once

#include <mcp/cap/capability.hpp>

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp::cap {

class LocalProvider final : public CapabilityProvider {
public:
    // Handler: the request → Result. Must not throw (the provider guards it
    // anyway, mapping a thrown exception to Result::error). A handler that
    // only needs the arguments can be given as one of `const Json&`.
    using Handler = std::function<Result(const Request& req)>;
    using ArgsHandler = std::function<Result(const Json& args)>;

    explicit LocalProvider(std::string origin = "local")
        : origin_(std::move(origin)) {}

    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }

    // Register a tool + its handler. `tool.name` is the dispatch key.
    template <class F>
        requires std::invocable<F&, const Request&> || std::invocable<F&, const Json&>
    void add(Tool tool, F handler) {
        const std::string key = tool.name;
        if constexpr (std::invocable<F&, const Request&>)
            handlers_[key] = Handler{std::move(handler)};
        else
            handlers_[key] = Handler{[h = std::move(handler)](const Request& r) mutable { return h(r.args); }};
        tools_.push_back(std::move(tool));
    }

    // Convenience: build a minimal Tool from name + description + raw schema.
    template <class F>
    void add(std::string name, std::string description, Json input_schema,
             F handler) {
        Tool t;
        t.name = name;
        if (!description.empty()) t.description = description;
        // The raw JSON schema → typed JsonSchema via the codec round-trip.
        try { t.inputSchema = from_json<JsonSchema>(input_schema); }
        catch (...) { /* leave default; the wire still carries name+desc */ }
        add(std::move(t), std::move(handler));
    }

    [[nodiscard]] std::vector<Tool> list() const override { return tools_; }

    [[nodiscard]] Result execute(const Request& req) override {
        auto it = handlers_.find(req.tool);
        if (it == handlers_.end())
            return Result::error("local: unknown tool '" + req.tool + "'");
        try {
            return it->second(req);
        } catch (const std::exception& e) {
            return Result::error(std::string{"local tool threw: "} + e.what());
        } catch (...) {
            return Result::error("local tool threw");
        }
    }

private:
    std::string                                  origin_;
    std::vector<Tool>                            tools_;
    std::unordered_map<std::string, Handler>     handlers_;
};

} // namespace mcp::cap
