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
#include <mcp/mrtr.hpp>

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

    // ─── MCP 2026-07-28 stateless core ───────────────────────────────────
    //
    // Enable the modern (stateless) protocol: attach per-request protocol
    // metadata (protocolVersion + clientInfo + clientCapabilities under their
    // reverse-DNS `_meta` keys) to EVERY subsequent outbound request. After
    // this, no initialize handshake is required — the server authenticates and
    // versions each request independently. Legacy servers ignore the unknown
    // `_meta` keys, so a dual-era client can safely call this AND still fall
    // back to initialize() if the server only speaks a legacy revision.
    //
    // `version` defaults to the newest revision this build advertises; pass a
    // negotiated older-but-modern version to pin it.
    void enable_modern_metadata(Implementation client_info,
                                ClientCapabilities caps = {},
                                std::string_view version = kProtocolVersion) {
        Json meta = Json::object();
        meta[std::string(meta_key::ProtocolVersion)]    = std::string(version);
        meta[std::string(meta_key::ClientInfo)]         = to_json(client_info);
        meta[std::string(meta_key::ClientCapabilities)] = to_json(caps);
        engine_.set_request_meta(std::move(meta));
    }
    // Revert to legacy mode (stop attaching per-request protocol metadata).
    void disable_modern_metadata() { engine_.clear_request_meta(); }

    // server/discover — query the server's supported versions, capabilities,
    // and identity without an initialize handshake (MCP 2026-07-28). Optional:
    // a client may skip discovery and send any RPC inline, handling an
    // UnsupportedProtocolVersionError (errc::UnsupportedProtocolVersion) if the
    // server rejects the declared version.
    [[nodiscard]] std::future<DiscoverResult> discover() {
        return engine_.request<DiscoverResult>(method::Discover, DiscoverParams{});
    }

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

    // MCP 2026-07-28 MRTR: call a tool, transparently fulfilling any
    // input_required rounds (sampling / elicitation / roots) the server asks
    // for, using this client's own handlers. Blocking (drives the retry loop),
    // so call it off the reader thread. Requires enable_modern_metadata().
    [[nodiscard]] CallToolResult call_tool_interactive(const CallToolParams& p) {
        Json final = run_mrtr(engine_, method::CallTool, to_json(p), mrtr_handlers_);
        return from_json<CallToolResult>(final);
    }
    [[nodiscard]] CallToolResult call_tool_interactive(std::string name, Json arguments = Json::object()) {
        return call_tool_interactive(
            CallToolParams{std::move(name), std::move(arguments), Nothing, Json::object()});
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
    // tasks/update (2026-07-28, SEP-2663): update a task's status/metadata.
    [[nodiscard]] std::future<UpdateTaskResult> update_task(const UpdateTaskParams& p) {
        return engine_.request<UpdateTaskResult>(method::UpdateTask, p);
    }
    // subscriptions/listen (2026-07-28, SEP-2663): opt into a notification
    // stream, listing the notification methods to receive.
    [[nodiscard]] std::future<Unit> subscriptions_listen(List<std::string> notifications) {
        return engine_.request<Unit>(method::SubscriptionsListen,
            SubscriptionsListenParams{std::move(notifications), Json::object()});
    }

    // ─── outbound notifications ──────────────────────────────────────────
    void notify_roots_changed() { engine_.notify_raw(method::RootsListChanged, Json::object()); }
    void progress(const ProgressParams& p) { engine_.notify(method::Progress, p); }
    void cancel(const CancelledParams& p)  { engine_.notify(method::Cancelled, p); }

    // Capability adapters construct the transport/client before host-specific
    // integration hooks are known. Installing an additional handler set is
    // safe: RpcEngine replaces handlers by method and MRTR mirrors the latest
    // callbacks.
    void set_handlers(ClientHandlers handlers) {
        install_handlers(std::move(handlers));
    }

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

        // Mirror the sampling / elicitation / roots handlers into raw-Json form
        // for MRTR fulfilment (call_tool_interactive). Same user callbacks,
        // just driven by the client loop instead of a server-initiated request.
        if (h.on_create_message)
            mrtr_handlers_.on_create_message =
                [f = h.on_create_message](const Json& p) { return to_json(f(from_json<CreateMessageParams>(p))); };
        if (h.on_elicit)
            mrtr_handlers_.on_elicit =
                [f = h.on_elicit](const Json& p) { return to_json(f(from_json<ElicitParams>(p))); };
        if (h.on_list_roots)
            mrtr_handlers_.on_list_roots =
                [f = h.on_list_roots](const Json&) { return to_json(f()); };
    }

    MrtrHandlers mrtr_handlers_;
    RpcEngine engine_;
};

} // namespace mcp
