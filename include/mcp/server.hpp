// SPDX-License-Identifier: Apache-2.0
//
// mcp/server.hpp — what an MCP server answers, as a table.
//
//   A Server holds the server's identity (info, capabilities, instructions)
//   and its registrations: tools, resources and prompts, each a spec plus a
//   function. handle(call) answers one of the client's calls from them,
//   synchronously, as the reply frame; handle(note) takes a notification.
//
//       Server s{Implementation{"my-server", "1.0"}};
//       s.register_tool(spec, [](const Json& args) { return CallToolResult{…}; });
//
//       // the host's read loop:
//       Effects fx = step(engine, Received{line});
//       for (auto& call : fx.calls) if (auto f = s.handle(call)) write(*f);
//       for (auto& note : fx.notifications) s.handle(note);
//
//   It has no engine, transport or thread. Calls TO the client (sampling,
//   roots, elicitation) and notifications (logging, list_changed) are
//   ordinary requests the host sends on its own engine with the method types
//   in protocol.hpp. Registration happens before serving; handle() is const.
#pragma once

#include <mcp/protocol.hpp>
#include <mcp/server_stateless.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

struct ToolEntry {
    Tool                                                 spec;
    std::function<CallToolResult(const Json& arguments)> invoke;
};
struct ResourceEntry {
    Resource                                                  spec;
    std::function<ReadResourceResult(const std::string& uri)> read;
};
struct PromptEntry {
    Prompt                                                                                  spec;
    std::function<GetPromptResult(const std::vector<std::pair<std::string, std::string>>&)> get;
};

class Server {
public:
    explicit Server(Implementation info = {}) : info_(std::move(info)), router_(make_router()) {}

    // ── identity ─────────────────────────────────────────────────────────
    void set_info(Implementation info)          { info_ = std::move(info); }
    void set_capabilities(ServerCapabilities c) { caps_ = std::move(c); }
    void set_instructions(std::string s)        { instructions_ = std::move(s); }
    [[nodiscard]] const ServerCapabilities& capabilities() const noexcept { return caps_; }

    // Sign the opaque `requestState` carried across MRTR rounds. A handler
    // seals its resume context with state_codec().seal(json); the client
    // echoes it back verbatim.
    void set_state_secret(std::string secret) { state_codec_ = RequestStateCodec{std::move(secret)}; }
    [[nodiscard]] const RequestStateCodec& state_codec() const noexcept { return state_codec_; }

    // Cache hint advertised on server/discover and the list results.
    // ttl_ms == 0 means not cacheable.
    void set_discover_cache(CacheHint h) { discover_cache_ = h; }
    [[nodiscard]] const CacheHint& discover_cache() const noexcept { return discover_cache_; }

    // Replace the default initialize answer, or observe `initialized`.
    void on_initialize(std::function<InitializeResult(const InitializeParams&)> f) { on_initialize_ = std::move(f); }
    void on_initialized(std::function<void()> f) { on_initialized_ = std::move(f); }

    // ── registrations ────────────────────────────────────────────────────
    // Each also sets the matching capability flag.
    void register_tool(Tool spec, std::function<CallToolResult(const Json&)> invoke) {
        const std::string name = spec.name;
        tools_[name] = ToolEntry{std::move(spec), std::move(invoke)};
        if (!caps_.tools) caps_.tools = ToolsCapability{};
    }
    void register_resource(Resource spec, std::function<ReadResourceResult(const std::string&)> read) {
        const std::string uri = spec.uri;
        resources_[uri] = ResourceEntry{std::move(spec), std::move(read)};
        if (!caps_.resources) caps_.resources = ResourcesCapability{};
    }
    void register_prompt(
        Prompt spec,
        std::function<GetPromptResult(const std::vector<std::pair<std::string, std::string>>&)> get) {
        const std::string name = spec.name;
        prompts_[name] = PromptEntry{std::move(spec), std::move(get)};
        if (!caps_.prompts) caps_.prompts = PromptsCapability{};
    }

    // Answer `method` with a raw handler instead (params in, result out),
    // e.g. a tools/call that drives MRTR by hand with input_required().
    void on_raw(std::string method, std::function<Json(const Json& params)> f) {
        router_.on_raw(std::move(method), [f = std::move(f)](const Server&, const Json& p) { return f(p); });
    }

    // ── serving ──────────────────────────────────────────────────────────
    // The reply frame for one of the client's calls (MethodNotFound for a
    // method this server doesn't answer).
    [[nodiscard]] std::string handle(const Call& c) const {
        if (auto frame = router_.handle(*this, c)) return *frame;
        return reply(c.id, std::unexpected(RpcError(errc::InternalError, "deferred method has no handler")));
    }
    void handle(const Notification& n) const {
        if (n.method == to_server::Initialized::name && on_initialized_) on_initialized_();
    }

private:
    // Built once. Handlers look registrations up at call time, so tools
    // registered later are served too.
    [[nodiscard]] static Router<const Server> make_router() {
        Router<const Server> r;
        r.on<to_server::Initialize>([](const Server& s, const InitializeParams& p) {
            if (s.on_initialize_) return s.on_initialize_(p);
            InitializeResult out;
            out.protocolVersion = p.protocolVersion.empty() ? std::string(kProtocolVersion)
                                                            : p.protocolVersion;
            out.capabilities = s.caps_;
            out.serverInfo   = s.info_;
            if (!s.instructions_.empty()) out.instructions = s.instructions_;
            return out;
        });
        r.on<to_server::Ping>([](const Server&, const Unit&) { return EmptyResult{}; });
        r.on<to_server::Discover>([](const Server& s, const DiscoverParams&) {
            DiscoverResult out;
            out.resultType = "complete";
            for (auto v : kSupportedProtocolVersions) out.supportedVersions.push_back(std::string(v));
            out.capabilities = s.caps_;
            if (!s.instructions_.empty()) out.instructions = s.instructions_;
            s.stamp_cache(out.ttlMs, out.cacheScope);
            out.meta = Json::object();
            out.meta[std::string(meta_key::ServerInfo)] = to_json(s.info_);
            return out;
        });
        r.on<to_server::ListTools>([](const Server& s, const ListToolsParams&) {
            served(!s.tools_.empty(), to_server::ListTools::name);
            ListToolsResult out;
            for (const auto& [_, e] : s.tools_) out.tools.push_back(e.spec);
            std::sort(out.tools.begin(), out.tools.end(),
                      [](const Tool& a, const Tool& b) { return a.name < b.name; });
            s.stamp_cache(out.ttlMs, out.cacheScope);
            return out;
        });
        r.on<to_server::CallTool>([](const Server& s, const CallToolParams& p) {
            served(!s.tools_.empty(), to_server::CallTool::name);
            auto it = s.tools_.find(p.name);
            if (it == s.tools_.end()) throw RpcError(errc::InvalidParams, "unknown tool: " + p.name);
            return it->second.invoke(p.arguments);
        });
        r.on<to_server::ListResources>([](const Server& s, const ListResourcesParams&) {
            served(!s.resources_.empty(), to_server::ListResources::name);
            ListResourcesResult out;
            for (const auto& [_, e] : s.resources_) out.resources.push_back(e.spec);
            return out;
        });
        r.on<to_server::ReadResource>([](const Server& s, const ReadResourceParams& p) {
            served(!s.resources_.empty(), to_server::ReadResource::name);
            auto it = s.resources_.find(p.uri);
            if (it == s.resources_.end()) throw RpcError(errc::InvalidParams, "unknown resource: " + p.uri);
            return it->second.read(p.uri);
        });
        r.on<to_server::ListPrompts>([](const Server& s, const ListPromptsParams&) {
            served(!s.prompts_.empty(), to_server::ListPrompts::name);
            ListPromptsResult out;
            for (const auto& [_, e] : s.prompts_) out.prompts.push_back(e.spec);
            return out;
        });
        r.on<to_server::GetPrompt>([](const Server& s, const GetPromptParams& p) {
            served(!s.prompts_.empty(), to_server::GetPrompt::name);
            auto it = s.prompts_.find(p.name);
            if (it == s.prompts_.end()) throw RpcError(errc::InvalidParams, "unknown prompt: " + p.name);
            std::vector<std::pair<std::string, std::string>> args;
            if (p.arguments) args = *p.arguments;
            return it->second.get(args);
        });
        return r;
    }

    // A kind with nothing registered isn't served, as if the route were absent.
    static void served(bool any, std::string_view method) {
        if (!any) throw RpcError(errc::MethodNotFound, "Method not found: " + std::string(method));
    }

    void stamp_cache(std::int64_t& ttl, std::string& scope) const {
        if (discover_cache_.ttl_ms <= 0) return;
        ttl   = std::max<std::int64_t>(0, discover_cache_.ttl_ms);
        scope = discover_cache_.scope.empty() ? std::string{"private"} : discover_cache_.scope;
    }

    Implementation     info_;
    ServerCapabilities caps_{};
    std::string        instructions_;
    RequestStateCodec  state_codec_{};
    CacheHint          discover_cache_{};

    std::function<InitializeResult(const InitializeParams&)> on_initialize_;
    std::function<void()>                                    on_initialized_;

    std::map<std::string, ToolEntry>     tools_;
    std::map<std::string, ResourceEntry> resources_;
    std::map<std::string, PromptEntry>   prompts_;

    mutable Router<const Server> router_;   // fixed after construction
};

}  // namespace mcp
