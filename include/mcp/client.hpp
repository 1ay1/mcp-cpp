// SPDX-License-Identifier: Apache-2.0
//
// mcp/client.hpp — what an MCP client answers, as a table.
//
//   A server may call the client back (sampling, roots, elicitation) and
//   notify it (logging, progress, list_changed, …). ClientHandlers says which
//   of those the client supports; handle(call) / handle(note) answer them
//   synchronously, as reply frames. MRTR fulfilment (mrtr.hpp) uses the same
//   handlers.
//
//   Calls TO the server are ordinary requests on the host's own engine with
//   the method types in protocol.hpp (request<to_server::CallTool>(…)). There
//   is no engine, transport or thread here.
#pragma once

#include <mcp/mrtr.hpp>
#include <mcp/protocol.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace mcp {

struct ClientHandlers {
    // Server → client requests. Set only what you support, and advertise the
    // matching ClientCapabilities.
    std::function<CreateMessageResult (const CreateMessageParams&)> on_create_message;
    std::function<ListRootsResult     ()>                           on_list_roots;
    std::function<ElicitResult        (const ElicitParams&)>        on_elicit;

    // Server → client notifications.
    std::function<void (const LoggingMessageParams&)>      on_log;
    std::function<void (const ProgressParams&)>            on_progress;
    std::function<void (const ResourceUpdatedParams&)>     on_resource_updated;
    std::function<void ()>                                 on_tools_changed;
    std::function<void ()>                                 on_resources_changed;
    std::function<void ()>                                 on_prompts_changed;
    std::function<void (const Task&)>                      on_task_status;
    std::function<void (const ElicitationCompleteParams&)> on_elicitation_complete;

    // The reply frame for one of the server's calls. A ping is always
    // answered; anything without a handler is MethodNotFound.
    [[nodiscard]] std::string handle(const Call& c) const {
        auto ok = [&](auto r) { return reply(c.id, std::expected<Json, RpcError>(to_json(r))); };
        try {
            if (c.method == to_client::Ping::name) return ok(EmptyResult{});
            if (c.method == to_client::CreateMessage::name && on_create_message)
                return ok(on_create_message(from_json<CreateMessageParams>(c.params)));
            if (c.method == to_client::ListRoots::name && on_list_roots)
                return ok(on_list_roots());
            if (c.method == to_client::Elicit::name && on_elicit)
                return ok(on_elicit(from_json<ElicitParams>(c.params)));
        } catch (const RpcError& e) {
            return reply(c.id, std::unexpected(e));
        } catch (const CodecError& e) {
            return reply(c.id, std::unexpected(RpcError(errc::InvalidParams, e.what())));
        } catch (const std::exception& e) {
            return reply(c.id, std::unexpected(RpcError(errc::InternalError, e.what())));
        }
        return reply(c.id, std::unexpected(RpcError(errc::MethodNotFound, "Method not found: " + c.method)));
    }

    // Run the notification's handler, if any. A handler that throws, or
    // params that don't decode, are dropped: a notification has no reply.
    void handle(const Notification& n) const {
        try {
            const auto& m = n.method;
            if      (m == to_client::LoggingMessage::name && on_log)                   on_log(from_json<LoggingMessageParams>(n.params));
            else if (m == Progress::name && on_progress)                               on_progress(from_json<ProgressParams>(n.params));
            else if (m == to_client::ResourceUpdated::name && on_resource_updated)     on_resource_updated(from_json<ResourceUpdatedParams>(n.params));
            else if (m == to_client::ToolsListChanged::name && on_tools_changed)       on_tools_changed();
            else if (m == to_client::ResourcesListChanged::name && on_resources_changed) on_resources_changed();
            else if (m == to_client::PromptsListChanged::name && on_prompts_changed)   on_prompts_changed();
            else if (m == TaskStatusChanged::name && on_task_status)                          on_task_status(from_json<Task>(n.params));
            else if (m == to_client::ElicitationComplete::name && on_elicitation_complete)
                on_elicitation_complete(from_json<ElicitationCompleteParams>(n.params));
        } catch (...) {}
    }

    // The same request handlers in the raw form MRTR fulfilment wants.
    [[nodiscard]] MrtrHandlers mrtr() const {
        MrtrHandlers h;
        if (on_create_message)
            h.on_create_message = [f = on_create_message](const Json& p) { return to_json(f(from_json<CreateMessageParams>(p))); };
        if (on_elicit)
            h.on_elicit = [f = on_elicit](const Json& p) { return to_json(f(from_json<ElicitParams>(p))); };
        if (on_list_roots)
            h.on_list_roots = [f = on_list_roots](const Json&) { return to_json(f()); };
        return h;
    }
};

// The per-request `_meta` a 2026-07-28 client sends on every request (the
// stateless protocol has no session to remember it). Install it with
// Engine::set_request_meta.
[[nodiscard]] inline Json modern_request_meta(const Implementation& client_info,
                                              const ClientCapabilities& caps = {},
                                              std::string_view version = kProtocolVersion) {
    Json meta = Json::object();
    meta[std::string(meta_key::ProtocolVersion)]    = std::string(version);
    meta[std::string(meta_key::ClientInfo)]         = to_json(client_info);
    meta[std::string(meta_key::ClientCapabilities)] = to_json(caps);
    return meta;
}

}  // namespace mcp
