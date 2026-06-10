// SPDX-License-Identifier: Apache-2.0
//
// mcp/client.hpp — the client-side peer surface.
//
//   In MCP a *client* connects a host application to a server. It drives the
//   server (initialize, list/call tools, read resources, get prompts, …) and
//   answers the server's callbacks for sampling, roots, and elicitation.
//
//        ┌────────────────────────────────────────────────────┐
//        │  Client                                            │
//        │                                                    │
//        │   .initialize(...)                    ──► Server   │  requests
//        │   .list_tools() / .call_tool(...)     ──► Server   │
//        │   .read_resource(...) / .get_prompt() ──► Server   │
//        │                                                    │
//        │   handlers = ClientHandlers{...}      ◄── Server   │  callbacks
//        │   on_create_message / on_list_roots / on_elicit    │
//        └────────────────────────────────────────────────────┘
//
#pragma once

#include <mcp/rpc.hpp>

namespace mcp {

//==============================================================================
//  ClientHandlers — the callbacks a server may invoke. Set only what you
//  support; advertise the matching ClientCapabilities flags.
//==============================================================================
struct ClientHandlers {
    std::function<CreateMessageResult (const CreateMessageParams&)> on_create_message;
    std::function<ListRootsResult     ()>                           on_list_roots;
    std::function<ElicitResult        (const ElicitParams&)>        on_elicit;

    // Async sampling — model calls are slow; let the handler defer the reply.
    std::function<void (const CreateMessageParams&,
                        RpcEngine::Responder<CreateMessageResult>)> on_create_message_async;
    std::function<void (const ElicitParams&,
                        RpcEngine::Responder<ElicitResult>)>        on_elicit_async;

    // Server → client notifications.
    std::function<void (const LoggingMessageParams&)>   on_log;
    std::function<void (const ProgressParams&)>         on_progress;
    std::function<void (const ResourceUpdatedParams&)>  on_resource_updated;
    std::function<void ()>                              on_tools_changed;
    std::function<void ()>                              on_resources_changed;
    std::function<void ()>                              on_prompts_changed;
    std::function<void (const Task&)>                   on_task_status;
    std::function<void (const ElicitationCompleteParams&)> on_elicitation_complete;
};

//==============================================================================
//  Client — the host-side connection handle.
//==============================================================================
class Client {
public:
    explicit Client(Transport sink, ClientHandlers handlers = {})
        : engine_(std::move(sink)) {
        install_handlers(std::move(handlers));
    }

    RpcEngine& engine() noexcept { return engine_; }

    void set_wire_trace(WireTrace t)         { engine_.set_wire_trace(std::move(t)); }
    void set_error_callback(ErrorCallback e) { engine_.set_error_callback(std::move(e)); }
    void set_default_timeout(std::chrono::milliseconds d) { engine_.set_default_timeout(d); }

    // ─── lifecycle ─────────────────────────────────────────────────────────
    [[nodiscard]] std::future<InitializeResult> initialize(const InitializeParams& p) {
        return engine_.request<InitializeResult>(method::Initialize, p);
    }
    // Convenience: initialize with our identity + capabilities, then send the
    // required `notifications/initialized` once the handshake resolves.
    [[nodiscard]] std::future<InitializeResult> initialize(
        Implementation client_info, ClientCapabilities caps = {}) {
        InitializeParams p;
        p.protocolVersion = std::string(kProtocolVersion);
        p.capabilities    = std::move(caps);
        p.clientInfo      = std::move(client_info);
        return engine_.request<InitializeResult>(method::Initialize, p);
    }
    void initialized() { engine_.notify_raw(method::Initialized, Json::object()); }

    [[nodiscard]] std::future<Unit> ping() { return engine_.request<Unit>(method::Ping, Unit{}); }

    // ─── tools ─────────────────────────────────────────────────────────────
    [[nodiscard]] std::future<ListToolsResult> list_tools(Maybe<std::string> cursor = Nothing) {
        return engine_.request<ListToolsResult>(method::ListTools,
                                                PaginatedParams{std::move(cursor), Json::object()});
    }
    [[nodiscard]] std::future<CallToolResult> call_tool(const CallToolParams& p) {
        return engine_.request<CallToolResult>(method::CallTool, p);
    }
    [[nodiscard]] std::future<CallToolResult> call_tool(std::string name, Json arguments = Json::object()) {
        return call_tool(CallToolParams{std::move(name), std::move(arguments), Nothing, Json::object()});
    }

    // ─── resources ───────────────────────────────────────────────────────
    [[nodiscard]] std::future<ListResourcesResult> list_resources(Maybe<std::string> cursor = Nothing) {
        return engine_.request<ListResourcesResult>(method::ListResources,
                                                    PaginatedParams{std::move(cursor), Json::object()});
    }
    [[nodiscard]] std::future<ListResourceTemplatesResult> list_resource_templates(Maybe<std::string> cursor = Nothing) {
        return engine_.request<ListResourceTemplatesResult>(method::ListResourceTemplates,
                                                            PaginatedParams{std::move(cursor), Json::object()});
    }
    [[nodiscard]] std::future<ReadResourceResult> read_resource(std::string uri) {
        return engine_.request<ReadResourceResult>(method::ReadResource,
                                                   ReadResourceParams{std::move(uri), Json::object()});
    }
    [[nodiscard]] std::future<Unit> subscribe(std::string uri) {
        return engine_.request<Unit>(method::Subscribe, SubscribeParams{std::move(uri), Json::object()});
    }
    [[nodiscard]] std::future<Unit> unsubscribe(std::string uri) {
        return engine_.request<Unit>(method::Unsubscribe, UnsubscribeParams{std::move(uri), Json::object()});
    }

    // ─── prompts ─────────────────────────────────────────────────────────
    [[nodiscard]] std::future<ListPromptsResult> list_prompts(Maybe<std::string> cursor = Nothing) {
        return engine_.request<ListPromptsResult>(method::ListPrompts,
                                                  PaginatedParams{std::move(cursor), Json::object()});
    }
    [[nodiscard]] std::future<GetPromptResult> get_prompt(const GetPromptParams& p) {
        return engine_.request<GetPromptResult>(method::GetPrompt, p);
    }

    // ─── completion / logging ────────────────────────────────────────────
    [[nodiscard]] std::future<CompleteResult> complete(const CompleteParams& p) {
        return engine_.request<CompleteResult>(method::Complete, p);
    }
    [[nodiscard]] std::future<Unit> set_level(LoggingLevel level) {
        return engine_.request<Unit>(method::SetLevel, SetLevelParams{level, Json::object()});
    }

    // ─── tasks ───────────────────────────────────────────────────────────
    [[nodiscard]] std::future<GetTaskResult> get_task(std::string id) {
        return engine_.request<GetTaskResult>(method::GetTask, TaskIdParams{std::move(id)});
    }
    [[nodiscard]] std::future<GetTaskPayloadResult> get_task_result(std::string id) {
        return engine_.request<GetTaskPayloadResult>(method::GetTaskPayload, TaskIdParams{std::move(id)});
    }
    [[nodiscard]] std::future<CancelTaskResult> cancel_task(std::string id) {
        return engine_.request<CancelTaskResult>(method::CancelTask, TaskIdParams{std::move(id)});
    }
    [[nodiscard]] std::future<ListTasksResult> list_tasks(Maybe<std::string> cursor = Nothing) {
        return engine_.request<ListTasksResult>(method::ListTasks,
                                               PaginatedParams{std::move(cursor), Json::object()});
    }

    // ─── outbound notifications ──────────────────────────────────────────
    void notify_roots_changed() { engine_.notify_raw(method::RootsListChanged, Json::object()); }
    void progress(const ProgressParams& p) { engine_.notify(method::Progress, p); }
    void cancel(const CancelledParams& p)  { engine_.notify(method::Cancelled, p); }

private:
    void install_handlers(ClientHandlers h) {
        // ping always answered with {}.
        engine_.on_request(std::string(method::Ping),
            [](const RpcId&, const Json&) -> Maybe<Json> { return Just<Json>(Json::object()); });

        if (h.on_create_message_async)
            engine_.on_async<CreateMessageParams, CreateMessageResult>(std::string(method::CreateMessage), h.on_create_message_async);
        else if (h.on_create_message)
            engine_.on<CreateMessageParams, CreateMessageResult>(std::string(method::CreateMessage), h.on_create_message);

        if (h.on_list_roots)
            engine_.on_request(std::string(method::ListRoots),
                [f = h.on_list_roots](const RpcId&, const Json&) -> Maybe<Json> {
                    return Just<Json>(to_json(f()));
                });

        if (h.on_elicit_async)
            engine_.on_async<ElicitParams, ElicitResult>(std::string(method::Elicit), h.on_elicit_async);
        else if (h.on_elicit)
            engine_.on<ElicitParams, ElicitResult>(std::string(method::Elicit), h.on_elicit);

        if (h.on_log)               engine_.on_note<LoggingMessageParams>(std::string(method::LoggingMessage), h.on_log);
        if (h.on_progress)          engine_.on_note<ProgressParams>(std::string(method::Progress), h.on_progress);
        if (h.on_resource_updated)  engine_.on_note<ResourceUpdatedParams>(std::string(method::ResourceUpdated), h.on_resource_updated);
        if (h.on_task_status)       engine_.on_note<Task>(std::string(method::TaskStatus), h.on_task_status);
        if (h.on_elicitation_complete) engine_.on_note<ElicitationCompleteParams>(std::string(method::ElicitationComplete), h.on_elicitation_complete);

        if (h.on_tools_changed)     engine_.on_notification(std::string(method::ToolsListChanged),     [f=h.on_tools_changed](const Json&){ f(); });
        if (h.on_resources_changed) engine_.on_notification(std::string(method::ResourcesListChanged), [f=h.on_resources_changed](const Json&){ f(); });
        if (h.on_prompts_changed)   engine_.on_notification(std::string(method::PromptsListChanged),   [f=h.on_prompts_changed](const Json&){ f(); });
    }

    RpcEngine engine_;
};

} // namespace mcp
