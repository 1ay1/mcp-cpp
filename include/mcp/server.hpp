// SPDX-License-Identifier: Apache-2.0
//
// mcp/server.hpp — the server-side peer surface.
//
//   In MCP a *server* exposes context & capabilities (tools, resources,
//   prompts, logging, completion) and may call BACK to the client for
//   sampling, roots, and elicitation.
//
//        ┌────────────────────────────────────────────────────┐
//        │  Server                                            │
//        │                                                    │
//        │   handlers = ServerHandlers{...}      ◄── Client   │  requests
//        │   on_initialize / list_tools / call_tool / ...     │
//        │                                                    │
//        │   .create_message(...)                ──► Client   │  outbound
//        │   .list_roots() / .elicit(...)        ──► Client   │
//        │   .log(...) / .notify_*_changed()     ──► Client   │  notifications
//        └────────────────────────────────────────────────────┘
//
//   Beyond the low-level ServerHandlers, the class offers an ergonomic
//   *registry* (register_tool / register_resource / register_prompt) that
//   auto-wires list + dispatch so a typical server is a few lines.
//
#pragma once

#include <mcp/rpc.hpp>

#include <unordered_map>

namespace mcp {

//==============================================================================
//  ServerHandlers — raw client→server entry points. Set only what you serve;
//  unset handlers reply MethodNotFound. Capabilities should mirror this set.
//==============================================================================
struct ServerHandlers {
    std::function<InitializeResult            (const InitializeParams&)>            on_initialize;
    std::function<void                        ()>                                  on_initialized;

    std::function<ListToolsResult             (const ListToolsParams&)>            on_list_tools;
    std::function<CallToolResult              (const CallToolParams&)>             on_call_tool;
    // Async tools/call — for durable/long-running calls that must not block the
    // reader thread (the handler hands the Responder to a worker).
    std::function<void (const CallToolParams&, RpcEngine::Responder<CallToolResult>)> on_call_tool_async;

    std::function<ListResourcesResult         (const ListResourcesParams&)>        on_list_resources;
    std::function<ListResourceTemplatesResult (const ListResourceTemplatesParams&)> on_list_resource_templates;
    std::function<ReadResourceResult          (const ReadResourceParams&)>         on_read_resource;
    std::function<Unit                        (const SubscribeParams&)>            on_subscribe;
    std::function<Unit                        (const UnsubscribeParams&)>          on_unsubscribe;

    std::function<ListPromptsResult           (const ListPromptsParams&)>          on_list_prompts;
    std::function<GetPromptResult             (const GetPromptParams&)>            on_get_prompt;

    std::function<CompleteResult              (const CompleteParams&)>             on_complete;
    std::function<Unit                        (const SetLevelParams&)>             on_set_level;

    // Tasks (durable requests).
    std::function<GetTaskResult               (const TaskIdParams&)>               on_get_task;
    std::function<GetTaskPayloadResult        (const TaskIdParams&)>               on_get_task_payload;
    std::function<CancelTaskResult            (const TaskIdParams&)>               on_cancel_task;
    std::function<ListTasksResult             (const PaginatedParams&)>            on_list_tasks;
};

//==============================================================================
//  A registered tool — schema + an invocation lambda.
//==============================================================================
struct ToolEntry {
    Tool                                              spec;
    std::function<CallToolResult(const Json& arguments)> invoke;
};
struct ResourceEntry {
    Resource                                              spec;
    std::function<ReadResourceResult(const std::string& uri)> read;
};
struct PromptEntry {
    Prompt                                                                       spec;
    std::function<GetPromptResult(const std::vector<std::pair<std::string,std::string>>&)> get;
};

//==============================================================================
//  Server — the server-side connection handle.
//==============================================================================
class Server {
public:
    explicit Server(Transport sink, Implementation info = {})
        : engine_(std::move(sink)), info_(std::move(info)) {
        install_default_handlers();
    }

    RpcEngine& engine() noexcept { return engine_; }

    void set_wire_trace(WireTrace t)         { engine_.set_wire_trace(std::move(t)); }
    void set_error_callback(ErrorCallback e) { engine_.set_error_callback(std::move(e)); }
    void set_default_timeout(std::chrono::milliseconds d) { engine_.set_default_timeout(d); }

    void set_info(Implementation info)         { info_ = std::move(info); }
    void set_capabilities(ServerCapabilities c){ caps_ = std::move(c); }
    void set_instructions(std::string s)       { instructions_ = std::move(s); }

    // ------------------------------------------------------- handler override
    // Install raw handlers (advanced). Overrides anything the registry set up.
    void set_handlers(ServerHandlers h) { install_handlers(std::move(h)); }

    // ------------------------------------------------------- registry sugar
    //
    //   register_tool wires both `tools/list` (the spec) and `tools/call`
    //   dispatch (the invoke). Likewise for resources & prompts. The relevant
    //   ServerCapabilities flag is set automatically.
    void register_tool(Tool spec, std::function<CallToolResult(const Json&)> invoke) {
        const std::string name = spec.name;
        tools_[name] = ToolEntry{std::move(spec), std::move(invoke)};
        if (!caps_.tools) caps_.tools = ToolsCapability{};
        wire_tool_registry();
    }

    void register_resource(Resource spec,
                           std::function<ReadResourceResult(const std::string&)> read) {
        const std::string uri = spec.uri;
        resources_[uri] = ResourceEntry{std::move(spec), std::move(read)};
        if (!caps_.resources) caps_.resources = ResourcesCapability{};
        wire_resource_registry();
    }

    void register_prompt(
        Prompt spec,
        std::function<GetPromptResult(const std::vector<std::pair<std::string,std::string>>&)> get) {
        const std::string name = spec.name;
        prompts_[name] = PromptEntry{std::move(spec), std::move(get)};
        if (!caps_.prompts) caps_.prompts = PromptsCapability{};
        wire_prompt_registry();
    }

    // ------------------------------------------------------- outbound: client
    [[nodiscard]] std::future<CreateMessageResult> create_message(const CreateMessageParams& p) {
        return engine_.request<CreateMessageResult>(method::CreateMessage, p);
    }
    [[nodiscard]] std::future<ListRootsResult> list_roots() {
        return engine_.request<ListRootsResult>(method::ListRoots, Unit{});
    }
    [[nodiscard]] std::future<ElicitResult> elicit(const ElicitParams& p) {
        return engine_.request<ElicitResult>(method::Elicit, p);
    }
    [[nodiscard]] std::future<Unit> ping() {
        return engine_.request<Unit>(method::Ping, Unit{});
    }

    // ------------------------------------------------------- outbound: notify
    void log(LoggingLevel level, Json data, Maybe<std::string> logger = Nothing) {
        engine_.notify(method::LoggingMessage,
                       LoggingMessageParams{level, std::move(data), std::move(logger), Json::object()});
    }
    void progress(const ProgressParams& p) { engine_.notify(method::Progress, p); }
    void notify_tools_changed()      { engine_.notify_raw(method::ToolsListChanged, Json::object()); }
    void notify_resources_changed()  { engine_.notify_raw(method::ResourcesListChanged, Json::object()); }
    void notify_prompts_changed()    { engine_.notify_raw(method::PromptsListChanged, Json::object()); }
    void notify_resource_updated(std::string uri) {
        engine_.notify(method::ResourceUpdated, ResourceUpdatedParams{std::move(uri), Json::object()});
    }
    void notify_task_status(const Task& t) { engine_.notify(method::TaskStatus, t); }

    const ServerCapabilities& capabilities() const noexcept { return caps_; }

private:
    void install_default_handlers() {
        // initialize: answer with our identity + accumulated capabilities.
        engine_.on_request(std::string(method::Initialize),
            [this](const RpcId&, const Json& j) -> Maybe<Json> {
                InitializeParams p = from_json<InitializeParams>(j);
                InitializeResult r;
                // Negotiate: echo the client's version if we understand it, else ours.
                r.protocolVersion = p.protocolVersion.empty()
                                    ? std::string(kProtocolVersion) : p.protocolVersion;
                r.capabilities = caps_;
                r.serverInfo   = info_;
                if (!instructions_.empty()) r.instructions = instructions_;
                if (on_initialize_) r = on_initialize_(p);
                return Just<Json>(to_json(r));
            });
        engine_.on_notification(std::string(method::Initialized),
            [this](const Json&) { if (on_initialized_) on_initialized_(); });
        // ping: spec requires an empty-object reply.
        engine_.on_request(std::string(method::Ping),
            [](const RpcId&, const Json&) -> Maybe<Json> { return Just<Json>(Json::object()); });
    }

    template <class Params, class Result, class F>
    void bind(std::string_view m, F& slot) {
        if (!slot) return;
        engine_.on<Params, Result>(std::string(m), slot);
    }

    void install_handlers(ServerHandlers h) {
        on_initialize_  = h.on_initialize;
        on_initialized_ = h.on_initialized;

        if (h.on_list_tools)              engine_.on<ListToolsParams, ListToolsResult>(std::string(method::ListTools), h.on_list_tools);
        if (h.on_call_tool_async)
            engine_.on_async<CallToolParams, CallToolResult>(std::string(method::CallTool), h.on_call_tool_async);
        else if (h.on_call_tool)
            engine_.on<CallToolParams, CallToolResult>(std::string(method::CallTool), h.on_call_tool);

        if (h.on_list_resources)          engine_.on<ListResourcesParams, ListResourcesResult>(std::string(method::ListResources), h.on_list_resources);
        if (h.on_list_resource_templates) engine_.on<ListResourceTemplatesParams, ListResourceTemplatesResult>(std::string(method::ListResourceTemplates), h.on_list_resource_templates);
        if (h.on_read_resource)           engine_.on<ReadResourceParams, ReadResourceResult>(std::string(method::ReadResource), h.on_read_resource);
        if (h.on_subscribe)               engine_.on<SubscribeParams, Unit>(std::string(method::Subscribe), h.on_subscribe);
        if (h.on_unsubscribe)             engine_.on<UnsubscribeParams, Unit>(std::string(method::Unsubscribe), h.on_unsubscribe);

        if (h.on_list_prompts)            engine_.on<ListPromptsParams, ListPromptsResult>(std::string(method::ListPrompts), h.on_list_prompts);
        if (h.on_get_prompt)              engine_.on<GetPromptParams, GetPromptResult>(std::string(method::GetPrompt), h.on_get_prompt);

        if (h.on_complete)                engine_.on<CompleteParams, CompleteResult>(std::string(method::Complete), h.on_complete);
        if (h.on_set_level)               engine_.on<SetLevelParams, Unit>(std::string(method::SetLevel), h.on_set_level);

        if (h.on_get_task)                engine_.on<TaskIdParams, GetTaskResult>(std::string(method::GetTask), h.on_get_task);
        if (h.on_get_task_payload)        engine_.on<TaskIdParams, GetTaskPayloadResult>(std::string(method::GetTaskPayload), h.on_get_task_payload);
        if (h.on_cancel_task)             engine_.on<TaskIdParams, CancelTaskResult>(std::string(method::CancelTask), h.on_cancel_task);
        if (h.on_list_tasks)              engine_.on<PaginatedParams, ListTasksResult>(std::string(method::ListTasks), h.on_list_tasks);
    }

    void wire_tool_registry() {
        engine_.on_request(std::string(method::ListTools),
            [this](const RpcId&, const Json&) -> Maybe<Json> {
                ListToolsResult r;
                for (const auto& [_, e] : tools_) r.tools.push_back(e.spec);
                return Just<Json>(to_json(r));
            });
        engine_.on_request(std::string(method::CallTool),
            [this](const RpcId&, const Json& j) -> Maybe<Json> {
                CallToolParams p = from_json<CallToolParams>(j);
                auto it = tools_.find(p.name);
                if (it == tools_.end())
                    throw RpcError(errc::InvalidParams, "unknown tool: " + p.name);
                return Just<Json>(to_json(it->second.invoke(p.arguments)));
            });
    }
    void wire_resource_registry() {
        engine_.on_request(std::string(method::ListResources),
            [this](const RpcId&, const Json&) -> Maybe<Json> {
                ListResourcesResult r;
                for (const auto& [_, e] : resources_) r.resources.push_back(e.spec);
                return Just<Json>(to_json(r));
            });
        engine_.on_request(std::string(method::ReadResource),
            [this](const RpcId&, const Json& j) -> Maybe<Json> {
                ReadResourceParams p = from_json<ReadResourceParams>(j);
                auto it = resources_.find(p.uri);
                if (it == resources_.end())
                    throw RpcError(errc::InvalidParams, "unknown resource: " + p.uri);
                return Just<Json>(to_json(it->second.read(p.uri)));
            });
    }
    void wire_prompt_registry() {
        engine_.on_request(std::string(method::ListPrompts),
            [this](const RpcId&, const Json&) -> Maybe<Json> {
                ListPromptsResult r;
                for (const auto& [_, e] : prompts_) r.prompts.push_back(e.spec);
                return Just<Json>(to_json(r));
            });
        engine_.on_request(std::string(method::GetPrompt),
            [this](const RpcId&, const Json& j) -> Maybe<Json> {
                GetPromptParams p = from_json<GetPromptParams>(j);
                auto it = prompts_.find(p.name);
                if (it == prompts_.end())
                    throw RpcError(errc::InvalidParams, "unknown prompt: " + p.name);
                std::vector<std::pair<std::string,std::string>> args;
                if (p.arguments) args = *p.arguments;
                return Just<Json>(to_json(it->second.get(args)));
            });
    }

    RpcEngine          engine_;
    Implementation     info_;
    ServerCapabilities caps_{};
    std::string        instructions_;

    std::function<InitializeResult(const InitializeParams&)> on_initialize_;
    std::function<void()>                                    on_initialized_;

    std::unordered_map<std::string, ToolEntry>     tools_;
    std::unordered_map<std::string, ResourceEntry> resources_;
    std::unordered_map<std::string, PromptEntry>   prompts_;
};

} // namespace mcp
