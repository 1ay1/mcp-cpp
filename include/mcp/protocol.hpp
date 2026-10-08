// SPDX-License-Identifier: Apache-2.0
//
// mcp/protocol.hpp — every MCP method, declared once, as a type.
//
//   A method's name, params and result live in exactly one place. The
//   engine's request<M>, reply<M>, result<M> decoding and a Router's on<M>
//   handler are all checked against these, so a call site, a handler and the
//   wire format can't disagree about a method's shape.
//
//   Direction is part of the namespace:
//
//     to_server::  the client calls these on the server
//     to_client::  the server calls these on the client
//
//   There is no connection class here and no runtime; the host drives a
//   jsonrpc::Engine and dispatches with a jsonrpc::Router (see
//   docs/PROTOCOL_LIBRARIES.md in agentty).
#pragma once

#include <jsonrpc/engine.hpp>
#include <jsonrpc/method.hpp>
#include <jsonrpc/router.hpp>

#include <mcp/methods.hpp>

namespace mcp {

using jsonrpc::Method;
using jsonrpc::Note;

//  EmptyResult ≅ Unit  (the schema's `EmptyResult = Result` with no fields).
using EmptyResult = Unit;

// ── The client calls these on the server ────────────────────────────────────
namespace to_server {
using Ping                  = Method<"ping",                      Unit,                        EmptyResult>;
using Initialize            = Method<"initialize",                InitializeParams,            InitializeResult>;
using Discover              = Method<"server/discover",           DiscoverParams,              DiscoverResult>;
using Complete              = Method<"completion/complete",       CompleteParams,              CompleteResult>;
using SetLevel              = Method<"logging/setLevel",          SetLevelParams,              EmptyResult>;
using GetPrompt             = Method<"prompts/get",               GetPromptParams,             GetPromptResult>;
using ListPrompts           = Method<"prompts/list",              ListPromptsParams,           ListPromptsResult>;
using ListResources         = Method<"resources/list",            ListResourcesParams,         ListResourcesResult>;
using ListResourceTemplates = Method<"resources/templates/list",  ListResourceTemplatesParams, ListResourceTemplatesResult>;
using ReadResource          = Method<"resources/read",            ReadResourceParams,          ReadResourceResult>;
using Subscribe             = Method<"resources/subscribe",       SubscribeParams,             EmptyResult>;
using Unsubscribe           = Method<"resources/unsubscribe",     UnsubscribeParams,           EmptyResult>;
using CallTool              = Method<"tools/call",                CallToolParams,              CallToolResult>;
using ListTools             = Method<"tools/list",                ListToolsParams,             ListToolsResult>;
using GetTask               = Method<"tasks/get",                 TaskIdParams,                GetTaskResult>;
using GetTaskPayload        = Method<"tasks/result",              TaskIdParams,                GetTaskPayloadResult>;
using ListTasks             = Method<"tasks/list",                PaginatedParams,             ListTasksResult>;
using CancelTask            = Method<"tasks/cancel",              TaskIdParams,                CancelTaskResult>;
using UpdateTask            = Method<"tasks/update",              UpdateTaskParams,            UpdateTaskResult>;
using SubscriptionsListen   = Method<"subscriptions/listen",      SubscriptionsListenParams,   EmptyResult>;

using Initialized           = Note<"notifications/initialized",         Unit>;
using RootsListChanged      = Note<"notifications/roots/list_changed",  Unit>;
}  // namespace to_server

// ── The server calls these on the client ────────────────────────────────────
namespace to_client {
using CreateMessage         = Method<"sampling/createMessage",    CreateMessageParams,         CreateMessageResult>;
using ListRoots             = Method<"roots/list",                Unit,                        ListRootsResult>;
using Elicit                = Method<"elicitation/create",        ElicitParams,                ElicitResult>;
using Ping                  = Method<"ping",                      Unit,                        EmptyResult>;

using LoggingMessage        = Note<"notifications/message",                LoggingMessageParams>;
using ResourceUpdated       = Note<"notifications/resources/updated",      ResourceUpdatedParams>;
using ResourcesListChanged  = Note<"notifications/resources/list_changed", Unit>;
using ToolsListChanged      = Note<"notifications/tools/list_changed",     Unit>;
using PromptsListChanged    = Note<"notifications/prompts/list_changed",   Unit>;
using ElicitationComplete   = Note<"notifications/elicitation/complete",   ElicitationCompleteParams>;
}  // namespace to_client

// ── Either side ─────────────────────────────────────────────────────────────
using Cancelled         = Note<"notifications/cancelled",    CancelledParams>;
using Progress          = Note<"notifications/progress",     ProgressParams>;
using TaskStatusChanged = Note<"notifications/tasks/status", TaskStatusParams>;

// The engine and its vocabulary, under mcp:: for protocol code.
using jsonrpc::Engine;
using jsonrpc::Router;
using jsonrpc::Effects;
using jsonrpc::Event;
using jsonrpc::Received;
using jsonrpc::Tick;
using jsonrpc::Cancel;
using jsonrpc::Closed;
using jsonrpc::Completed;
using jsonrpc::Call;
using jsonrpc::Notification;
using jsonrpc::Id;
using jsonrpc::RpcError;
using jsonrpc::Deadline;
using jsonrpc::step;
using jsonrpc::request;
using jsonrpc::request_raw;
using jsonrpc::reply;
using jsonrpc::result;
using jsonrpc::notify;
using jsonrpc::notify_raw;

namespace errc {
using namespace jsonrpc::errc;
// MCP's own codes, in the implementation-defined range.
inline constexpr int UrlElicitationRequired          = -32042;
inline constexpr int UnsupportedProtocolVersion      = -32020;
inline constexpr int MissingRequiredClientCapability = -32021;
}  // namespace errc

// ── URLElicitationRequiredError: the structured -32042 payload ──────────────
inline constexpr int kUrlElicitationRequired = errc::UrlElicitationRequired;

struct UrlElicitationRequiredErrorData {
    List<ElicitUrlParams> elicitations;
    Json                  extra = Json::object();   // open-ended per schema
};
template <> struct CodecOf<UrlElicitationRequiredErrorData> {
    static Codec<UrlElicitationRequiredErrorData> get() {
        auto el = list_codec(codec<ElicitUrlParams>());
        return {
            [el](const UrlElicitationRequiredErrorData& d) -> Json {
                Json j = d.extra.is_object() ? d.extra : Json::object();
                j["elicitations"] = el.encode(d.elicitations);
                return j;
            },
            [el](const Json& j) -> UrlElicitationRequiredErrorData {
                UrlElicitationRequiredErrorData d;
                if (auto it = j.find("elicitations"); it != j.end())
                    d.elicitations = el.decode(*it);
                d.extra = j;
                d.extra.erase("elicitations");
                return d;
            }};
    }
};

}  // namespace mcp
