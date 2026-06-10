// SPDX-License-Identifier: Apache-2.0
//
// mcp/protocol.hpp — the JSON-RPC envelope algebra + the message-level sums.
//
//   schema.ts closes with seven discriminated unions describing the entire
//   message graph:
//
//     JSONRPCMessage   = Request | Notification | Response
//     ClientRequest    = Ping | Initialize | … | CancelTask
//     ClientNotification / ServerNotification
//     ClientResult / ServerResult / ServerRequest
//
//   We model them faithfully, AND we go one step further: a compile-time
//   *method dictionary* pairs every wire method with its (Params, Result)
//   types. The unions are then a fold over that one source of truth, and a
//   typed peer can dispatch `request<dict::CallTool>(params)` with the result
//   type deduced — there is no place to get the pairing wrong.
//
#pragma once

#include <mcp/methods.hpp>

namespace mcp {

//==============================================================================
//  Error — the JSON-RPC 2.0 error object (schema.ts Error).
//==============================================================================
struct Error {
    int         code = 0;
    std::string message;
    Maybe<Json> data;
};
template <> struct CodecOf<Error> {
    static Codec<Error> get() {
        return record<Error>(
            required("code",    &Error::code),
            required("message", &Error::message),
            optional("data",    &Error::data));
    }
};

//==============================================================================
//  URLElicitationRequiredError — the structured -32042 payload (schema.ts).
//==============================================================================
inline constexpr int kUrlElicitationRequired = -32042;

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

//==============================================================================
//  The JSON-RPC envelope as a tagged algebra.
//==============================================================================
struct JsonRpcRequest {
    RpcId       id;
    std::string method;
    Json        params = Json::object();
};
template <> struct CodecOf<JsonRpcRequest> {
    static Codec<JsonRpcRequest> get() {
        return {
            [](const JsonRpcRequest& r) -> Json {
                Json j = {{"jsonrpc", "2.0"}, {"id", r.id}, {"method", r.method}};
                if (!(r.params.is_object() && r.params.empty()) && !r.params.is_null())
                    j["params"] = r.params;
                return j;
            },
            [](const Json& j) -> JsonRpcRequest {
                JsonRpcRequest r;
                r.id     = j.at("id");
                r.method = j.at("method").get<std::string>();
                r.params = j.value("params", Json::object());
                return r;
            }};
    }
};

struct JsonRpcNotification {
    std::string method;
    Json        params = Json::object();
};
template <> struct CodecOf<JsonRpcNotification> {
    static Codec<JsonRpcNotification> get() {
        return {
            [](const JsonRpcNotification& n) -> Json {
                Json j = {{"jsonrpc", "2.0"}, {"method", n.method}};
                if (!(n.params.is_object() && n.params.empty()) && !n.params.is_null())
                    j["params"] = n.params;
                return j;
            },
            [](const Json& j) -> JsonRpcNotification {
                JsonRpcNotification n;
                n.method = j.at("method").get<std::string>();
                n.params = j.value("params", Json::object());
                return n;
            }};
    }
};

struct JsonRpcResult {
    RpcId id;
    Json  result = Json::object();
};
template <> struct CodecOf<JsonRpcResult> {
    static Codec<JsonRpcResult> get() {
        return {
            [](const JsonRpcResult& r) -> Json {
                return Json{{"jsonrpc", "2.0"}, {"id", r.id}, {"result", r.result}};
            },
            [](const Json& j) -> JsonRpcResult {
                return JsonRpcResult{j.at("id"), j.value("result", Json::object())};
            }};
    }
};

struct JsonRpcError {
    Maybe<RpcId> id;        // may be null when the request id is unknown
    Error        error;
};
template <> struct CodecOf<JsonRpcError> {
    static Codec<JsonRpcError> get() {
        return {
            [](const JsonRpcError& e) -> Json {
                Json j = {{"jsonrpc", "2.0"}, {"error", to_json(e.error)}};
                j["id"] = e.id ? *e.id : Json(nullptr);
                return j;
            },
            [](const Json& j) -> JsonRpcError {
                JsonRpcError e;
                if (auto it = j.find("id"); it != j.end() && !it->is_null()) e.id = *it;
                e.error = from_json<Error>(j.at("error"));
                return e;
            }};
    }
};

//  Response = Result + Error  (disambiguated by which key is present).
using JsonRpcResponse = Sum<JsonRpcResult, JsonRpcError>;
template <> struct CodecOf<JsonRpcResponse> {
    static Codec<JsonRpcResponse> get() {
        return {
            [](const JsonRpcResponse& r) -> Json {
                return std::visit([](const auto& x) { return to_json(x); }, r);
            },
            [](const Json& j) -> JsonRpcResponse {
                if (j.contains("error")) return JsonRpcResponse{from_json<JsonRpcError>(j)};
                return JsonRpcResponse{from_json<JsonRpcResult>(j)};
            }};
    }
};

//  JsonRpcMessage = Request + Notification + Response  (schema.ts JSONRPCMessage).
using JsonRpcMessage = Sum<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse>;
template <> struct CodecOf<JsonRpcMessage> {
    static Codec<JsonRpcMessage> get() {
        return {
            [](const JsonRpcMessage& m) -> Json {
                return std::visit([](const auto& x) { return to_json(x); }, m);
            },
            [](const Json& j) -> JsonRpcMessage {
                const bool has_method = j.contains("method");
                const bool has_id     = j.contains("id");
                if (has_method && has_id) return JsonRpcMessage{from_json<JsonRpcRequest>(j)};
                if (has_method)           return JsonRpcMessage{from_json<JsonRpcNotification>(j)};
                return JsonRpcMessage{from_json<JsonRpcResponse>(j)};
            }};
    }
};

//==============================================================================
//  EmptyResult ≅ Unit  (the schema's `EmptyResult = Result` with no fields).
//==============================================================================
using EmptyResult = Unit;

//==============================================================================
//  RpcId constructors — RpcId is raw Json (string | number per JSON-RPC 2.0).
//  Use these instead of brace-init: `Json{1}` is an ARRAY `[1]`, a classic
//  nlohmann pitfall; `id(1)` always yields the scalar `1`.
//==============================================================================
inline RpcId id(std::int64_t n) { return RpcId(n); }
inline RpcId id(std::string s) { return RpcId(std::move(s)); }
inline RpcId id(const char* s)  { return RpcId(std::string(s)); }

//==============================================================================
//  The method dictionary — one descriptor per wire method, pairing the params
//  type with the result type (or marking it a notification). This is the
//  single source of truth the ClientRequest / ServerRequest unions fold over.
//==============================================================================
template <StaticString M, class P, class R>
struct Rpc {
    using Params = P;
    using Result = R;
    static constexpr std::string_view method = M.view();
};
template <StaticString M, class P>
struct Note {
    using Params = P;
    static constexpr std::string_view method = M.view();
};

namespace dict {
    // ── Client → Server requests (schema.ts ClientRequest) ─────────────────
    using Ping                  = Rpc<"ping",                         Unit,                         EmptyResult>;
    using Initialize            = Rpc<"initialize",                   InitializeParams,             InitializeResult>;
    using Complete              = Rpc<"completion/complete",          CompleteParams,               CompleteResult>;
    using SetLevel              = Rpc<"logging/setLevel",             SetLevelParams,               EmptyResult>;
    using GetPrompt             = Rpc<"prompts/get",                  GetPromptParams,              GetPromptResult>;
    using ListPrompts           = Rpc<"prompts/list",                 ListPromptsParams,            ListPromptsResult>;
    using ListResources         = Rpc<"resources/list",              ListResourcesParams,          ListResourcesResult>;
    using ListResourceTemplates = Rpc<"resources/templates/list",    ListResourceTemplatesParams,  ListResourceTemplatesResult>;
    using ReadResource          = Rpc<"resources/read",              ReadResourceParams,           ReadResourceResult>;
    using Subscribe             = Rpc<"resources/subscribe",         SubscribeParams,              EmptyResult>;
    using Unsubscribe           = Rpc<"resources/unsubscribe",       UnsubscribeParams,            EmptyResult>;
    using CallTool              = Rpc<"tools/call",                   CallToolParams,               CallToolResult>;
    using ListTools             = Rpc<"tools/list",                   ListToolsParams,              ListToolsResult>;
    using GetTask               = Rpc<"tasks/get",                    TaskIdParams,                 GetTaskResult>;
    using GetTaskPayload        = Rpc<"tasks/result",                 TaskIdParams,                 GetTaskPayloadResult>;
    using ListTasks             = Rpc<"tasks/list",                   PaginatedParams,              ListTasksResult>;
    using CancelTask            = Rpc<"tasks/cancel",                 TaskIdParams,                 CancelTaskResult>;

    // ── Server → Client requests (schema.ts ServerRequest) ──────────────────
    using CreateMessage         = Rpc<"sampling/createMessage",      CreateMessageParams,          CreateMessageResult>;
    using ListRoots             = Rpc<"roots/list",                   Unit,                         ListRootsResult>;
    using Elicit                = Rpc<"elicitation/create",          ElicitParams,                 ElicitResult>;

    // ── Client → Server notifications (schema.ts ClientNotification) ────────
    using Cancelled             = Note<"notifications/cancelled",            CancelledParams>;
    using Progress              = Note<"notifications/progress",             ProgressParams>;
    using Initialized           = Note<"notifications/initialized",          Unit>;
    using RootsListChanged      = Note<"notifications/roots/list_changed",    Unit>;
    using TaskStatus            = Note<"notifications/tasks/status",          TaskStatusParams>;

    // ── Server → Client notifications (schema.ts ServerNotification) ────────
    using LoggingMessage        = Note<"notifications/message",                  LoggingMessageParams>;
    using ResourceUpdated       = Note<"notifications/resources/updated",        ResourceUpdatedParams>;
    using ResourcesListChanged  = Note<"notifications/resources/list_changed",   Unit>;
    using ToolsListChanged      = Note<"notifications/tools/list_changed",       Unit>;
    using PromptsListChanged    = Note<"notifications/prompts/list_changed",     Unit>;
    using ElicitationComplete   = Note<"notifications/elicitation/complete",     ElicitationCompleteParams>;
} // namespace dict

//==============================================================================
//  method_v<Desc> — the wire method literal carried by the descriptor itself.
//  (Each Rpc/Note bakes its method in as an NTTP, so the pairing of method ↔
//  params ↔ result is a single indivisible token.)
//==============================================================================
template <class Desc>
inline constexpr std::string_view method_v = Desc::method;

//==============================================================================
//  Message-level sums (schema.ts ClientRequest / ServerRequest / … / *Result).
//
//      These mirror the spec's closing discriminated unions exactly. They are
//      method-tagged: encode prepends jsonrpc+method, decode reads `method`.
//      The engine itself dispatches by method string for speed; these typed
//      sums exist for callers who want an exhaustively-matchable value.
//==============================================================================

//  A request union arm: a typed Params wrapped with its method literal.
template <class P>
struct MethodReq { std::string_view method; P params; };
template <class P>
struct MethodNote { std::string_view method; P params; };

//  ClientRequest — every request the client may send.
using ClientRequest = Sum<
    MethodReq<InitializeParams>, MethodReq<CompleteParams>, MethodReq<SetLevelParams>,
    MethodReq<GetPromptParams>, MethodReq<ListPromptsParams>, MethodReq<ListResourcesParams>,
    MethodReq<ListResourceTemplatesParams>, MethodReq<ReadResourceParams>,
    MethodReq<SubscribeParams>, MethodReq<UnsubscribeParams>, MethodReq<CallToolParams>,
    MethodReq<ListToolsParams>, MethodReq<TaskIdParams>, MethodReq<PaginatedParams>,
    MethodReq<Unit>>;   // Unit arm covers ping

//  ServerRequest — every request the server may send back.
using ServerRequest = Sum<
    MethodReq<CreateMessageParams>, MethodReq<ElicitParams>, MethodReq<TaskIdParams>,
    MethodReq<PaginatedParams>, MethodReq<Unit>>;   // Unit covers ping / roots/list

//  ClientResult — every result the client may return (schema.ts ClientResult).
using ClientResult = Sum<
    EmptyResult, CreateMessageResult, ListRootsResult, ElicitResult,
    GetTaskResult, GetTaskPayloadResult, ListTasksResult, CancelTaskResult>;

//  ServerResult — every result the server may return (schema.ts ServerResult).
using ServerResult = Sum<
    EmptyResult, InitializeResult, CompleteResult, GetPromptResult, ListPromptsResult,
    ListResourceTemplatesResult, ListResourcesResult, ReadResourceResult,
    CallToolResult, ListToolsResult, GetTaskResult, GetTaskPayloadResult,
    ListTasksResult, CancelTaskResult>;

//  ClientNotification / ServerNotification — method-tagged notification sums.
using ClientNotification = Sum<
    MethodNote<CancelledParams>, MethodNote<ProgressParams>,
    MethodNote<TaskStatusParams>, MethodNote<Unit>>;   // Unit covers initialized / roots changed
using ServerNotification = Sum<
    MethodNote<CancelledParams>, MethodNote<ProgressParams>,
    MethodNote<LoggingMessageParams>, MethodNote<ResourceUpdatedParams>,
    MethodNote<ElicitationCompleteParams>, MethodNote<TaskStatusParams>,
    MethodNote<Unit>>;   // Unit covers the three *_list_changed notifications

//==============================================================================
//  Typed descriptor dispatch — the payoff of the method dictionary.
//
//      call<Desc>(engine, params)   → std::future<Desc::Result>
//      send<Desc>(engine, params)   fire-and-forget notification
//      handle<Desc>(engine, fn)     register a typed request handler
//      observe<Desc>(engine, fn)    register a typed notification handler
//
//   The method string AND the result type are both derived from `Desc`, so a
//   mismatched pairing is impossible to express. (RpcEngine is defined in
//   rpc.hpp, included transitively; these are thin free functions over it.)
//==============================================================================
template <class Desc, class E>
[[nodiscard]] auto call(E& engine, const typename Desc::Params& p) {
    return engine.template request<typename Desc::Result, typename Desc::Params>(
        method_v<Desc>, p);
}
template <class Desc, class E>
[[nodiscard]] auto call(E& engine) {
    return engine.template request<typename Desc::Result>(method_v<Desc>);
}
template <class Desc, class E>
void send(E& engine, const typename Desc::Params& p) {
    engine.notify(method_v<Desc>, p);
}
template <class Desc, class E>
void send(E& engine) {
    engine.notify_raw(method_v<Desc>, Json::object());
}
template <class Desc, class E, class F>
void handle(E& engine, F fn) {
    engine.template on<typename Desc::Params, typename Desc::Result>(
        std::string(method_v<Desc>), std::move(fn));
}
template <class Desc, class E, class F>
void observe(E& engine, F fn) {
    engine.template on_note<typename Desc::Params>(
        std::string(method_v<Desc>), std::move(fn));
}

} // namespace mcp
