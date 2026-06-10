// SPDX-License-Identifier: Apache-2.0
//
// mcp/methods.hpp — request/result payload records + wire-method constants.
//
//   One Params (and, for requests, one Result) record per MCP method. Method
//   names match the wire 1:1 and live in mcp::method::{}. Notifications carry
//   only Params. Decoupled-from-RPC param schemas per SEP-1319.
//
#pragma once

#include <mcp/elicit.hpp>

namespace mcp {

//==============================================================================
//  Wire-method names — single source of truth (schema.ts method literals).
//==============================================================================
namespace method {
inline constexpr std::string_view Initialize          = "initialize";
inline constexpr std::string_view Ping                = "ping";
inline constexpr std::string_view Complete            = "completion/complete";
inline constexpr std::string_view SetLevel            = "logging/setLevel";

inline constexpr std::string_view ListPrompts         = "prompts/list";
inline constexpr std::string_view GetPrompt           = "prompts/get";

inline constexpr std::string_view ListResources       = "resources/list";
inline constexpr std::string_view ListResourceTemplates = "resources/templates/list";
inline constexpr std::string_view ReadResource        = "resources/read";
inline constexpr std::string_view Subscribe           = "resources/subscribe";
inline constexpr std::string_view Unsubscribe         = "resources/unsubscribe";

inline constexpr std::string_view ListTools           = "tools/list";
inline constexpr std::string_view CallTool            = "tools/call";

inline constexpr std::string_view CreateMessage       = "sampling/createMessage";
inline constexpr std::string_view ListRoots           = "roots/list";
inline constexpr std::string_view Elicit              = "elicitation/create";

inline constexpr std::string_view GetTask             = "tasks/get";
inline constexpr std::string_view GetTaskPayload      = "tasks/result";
inline constexpr std::string_view CancelTask          = "tasks/cancel";
inline constexpr std::string_view ListTasks           = "tasks/list";

// Notifications.
inline constexpr std::string_view Cancelled              = "notifications/cancelled";
inline constexpr std::string_view Progress               = "notifications/progress";
inline constexpr std::string_view Initialized            = "notifications/initialized";
inline constexpr std::string_view RootsListChanged       = "notifications/roots/list_changed";
inline constexpr std::string_view ResourcesListChanged   = "notifications/resources/list_changed";
inline constexpr std::string_view ResourceUpdated        = "notifications/resources/updated";
inline constexpr std::string_view ToolsListChanged       = "notifications/tools/list_changed";
inline constexpr std::string_view PromptsListChanged     = "notifications/prompts/list_changed";
inline constexpr std::string_view LoggingMessage         = "notifications/message";
inline constexpr std::string_view TaskStatus             = "notifications/tasks/status";
inline constexpr std::string_view ElicitationComplete    = "notifications/elicitation/complete";
} // namespace method

//==============================================================================
//  initialize
//==============================================================================
struct InitializeParams {
    std::string        protocolVersion = std::string(kProtocolVersion);
    ClientCapabilities capabilities{};
    Implementation     clientInfo{};
    Json               meta = Json::object();
};
template <> struct CodecOf<InitializeParams> {
    static Codec<InitializeParams> get() {
        return record<InitializeParams>(
            required("protocolVersion", &InitializeParams::protocolVersion),
            required("capabilities",    &InitializeParams::capabilities),
            required("clientInfo",      &InitializeParams::clientInfo),
            meta    ("_meta",           &InitializeParams::meta));
    }
};

struct InitializeResult {
    std::string        protocolVersion = std::string(kProtocolVersion);
    ServerCapabilities capabilities{};
    Implementation     serverInfo{};
    Maybe<std::string> instructions;
    Json               meta = Json::object();
};
template <> struct CodecOf<InitializeResult> {
    static Codec<InitializeResult> get() {
        return record<InitializeResult>(
            required("protocolVersion", &InitializeResult::protocolVersion),
            required("capabilities",    &InitializeResult::capabilities),
            required("serverInfo",      &InitializeResult::serverInfo),
            optional("instructions",    &InitializeResult::instructions),
            meta    ("_meta",           &InitializeResult::meta));
    }
};

//==============================================================================
//  Pagination scaffolding (cursor in / nextCursor out).
//==============================================================================
struct PaginatedParams { Maybe<std::string> cursor; Json meta = Json::object(); };
template <> struct CodecOf<PaginatedParams> {
    static Codec<PaginatedParams> get() {
        return record<PaginatedParams>(
            optional("cursor", &PaginatedParams::cursor),
            meta    ("_meta",  &PaginatedParams::meta));
    }
};

//==============================================================================
//  resources/list, resources/templates/list, resources/read, subscribe…
//==============================================================================
using ListResourcesParams = PaginatedParams;
struct ListResourcesResult {
    List<Resource>     resources;
    Maybe<std::string> nextCursor;
    Json               meta = Json::object();
};
template <> struct CodecOf<ListResourcesResult> {
    static Codec<ListResourcesResult> get() {
        return record<ListResourcesResult>(
            defaulted("resources",  &ListResourcesResult::resources, List<Resource>{}),
            optional ("nextCursor", &ListResourcesResult::nextCursor),
            meta     ("_meta",      &ListResourcesResult::meta));
    }
};

using ListResourceTemplatesParams = PaginatedParams;
struct ListResourceTemplatesResult {
    List<ResourceTemplate> resourceTemplates;
    Maybe<std::string>     nextCursor;
    Json                   meta = Json::object();
};
template <> struct CodecOf<ListResourceTemplatesResult> {
    static Codec<ListResourceTemplatesResult> get() {
        return record<ListResourceTemplatesResult>(
            defaulted("resourceTemplates", &ListResourceTemplatesResult::resourceTemplates, List<ResourceTemplate>{}),
            optional ("nextCursor",        &ListResourceTemplatesResult::nextCursor),
            meta     ("_meta",             &ListResourceTemplatesResult::meta));
    }
};

struct ReadResourceParams { std::string uri; Json meta = Json::object(); };
template <> struct CodecOf<ReadResourceParams> {
    static Codec<ReadResourceParams> get() {
        return record<ReadResourceParams>(
            required("uri",   &ReadResourceParams::uri),
            meta    ("_meta", &ReadResourceParams::meta));
    }
};
struct ReadResourceResult { List<ResourceContents> contents; Json meta = Json::object(); };
template <> struct CodecOf<ReadResourceResult> {
    static Codec<ReadResourceResult> get() {
        return record<ReadResourceResult>(
            required("contents", &ReadResourceResult::contents),
            meta    ("_meta",    &ReadResourceResult::meta));
    }
};

struct SubscribeParams   { std::string uri; Json meta = Json::object(); };
struct UnsubscribeParams { std::string uri; Json meta = Json::object(); };
template <> struct CodecOf<SubscribeParams> {
    static Codec<SubscribeParams> get() {
        return record<SubscribeParams>(
            required("uri", &SubscribeParams::uri), meta("_meta", &SubscribeParams::meta));
    }
};
template <> struct CodecOf<UnsubscribeParams> {
    static Codec<UnsubscribeParams> get() {
        return record<UnsubscribeParams>(
            required("uri", &UnsubscribeParams::uri), meta("_meta", &UnsubscribeParams::meta));
    }
};

//==============================================================================
//  prompts/list, prompts/get
//==============================================================================
using ListPromptsParams = PaginatedParams;
struct ListPromptsResult {
    List<Prompt>       prompts;
    Maybe<std::string> nextCursor;
    Json               meta = Json::object();
};
template <> struct CodecOf<ListPromptsResult> {
    static Codec<ListPromptsResult> get() {
        return record<ListPromptsResult>(
            defaulted("prompts",    &ListPromptsResult::prompts, List<Prompt>{}),
            optional ("nextCursor", &ListPromptsResult::nextCursor),
            meta     ("_meta",      &ListPromptsResult::meta));
    }
};

struct GetPromptParams {
    std::string                                name;
    Maybe<std::vector<std::pair<std::string, std::string>>> arguments;  // string map
    Json                                       meta = Json::object();
};
template <> struct CodecOf<GetPromptParams> {
    static Codec<GetPromptParams> get() {
        return {
            [](const GetPromptParams& p) -> Json {
                Json j = {{"name", p.name}};
                if (p.arguments) {
                    Json a = Json::object();
                    for (const auto& [k, v] : *p.arguments) a[k] = v;
                    j["arguments"] = std::move(a);
                }
                if (!(p.meta.is_object() && p.meta.empty())) j["_meta"] = p.meta;
                return j;
            },
            [](const Json& j) -> GetPromptParams {
                GetPromptParams p; p.name = j.at("name").get<std::string>();
                if (auto it = j.find("arguments"); it != j.end() && it->is_object()) {
                    std::vector<std::pair<std::string, std::string>> args;
                    for (auto& [k, v] : it->items()) args.emplace_back(k, v.get<std::string>());
                    p.arguments = std::move(args);
                }
                if (auto it = j.find("_meta"); it != j.end()) p.meta = *it;
                return p;
            }};
    }
};
struct GetPromptResult {
    List<PromptMessage> messages;
    Maybe<std::string>  description;
    Json                meta = Json::object();
};
template <> struct CodecOf<GetPromptResult> {
    static Codec<GetPromptResult> get() {
        return record<GetPromptResult>(
            required("messages",    &GetPromptResult::messages),
            optional("description", &GetPromptResult::description),
            meta    ("_meta",       &GetPromptResult::meta));
    }
};

//==============================================================================
//  tools/list, tools/call
//==============================================================================
using ListToolsParams = PaginatedParams;
struct ListToolsResult {
    List<Tool>         tools;
    Maybe<std::string> nextCursor;
    Json               meta = Json::object();
};
template <> struct CodecOf<ListToolsResult> {
    static Codec<ListToolsResult> get() {
        return record<ListToolsResult>(
            defaulted("tools",      &ListToolsResult::tools, List<Tool>{}),
            optional ("nextCursor", &ListToolsResult::nextCursor),
            meta     ("_meta",      &ListToolsResult::meta));
    }
};

struct CallToolParams {
    std::string         name;
    Json                arguments = Json::object();
    Maybe<TaskMetadata> task;        // task-augmented (durable requests)
    Json                meta = Json::object();
};
template <> struct CodecOf<CallToolParams> {
    static Codec<CallToolParams> get() {
        return record<CallToolParams>(
            required ("name",      &CallToolParams::name),
            defaulted("arguments", &CallToolParams::arguments, Json::object()),
            optional ("task",      &CallToolParams::task),
            meta     ("_meta",     &CallToolParams::meta));
    }
};
struct CallToolResult {
    List<ContentBlock> content;
    Maybe<Json>        structuredContent;
    Maybe<bool>        isError;
    Json               meta = Json::object();
};
template <> struct CodecOf<CallToolResult> {
    static Codec<CallToolResult> get() {
        return record<CallToolResult>(
            defaulted("content",           &CallToolResult::content, List<ContentBlock>{}),
            optional ("structuredContent", &CallToolResult::structuredContent),
            optional ("isError",           &CallToolResult::isError),
            meta     ("_meta",             &CallToolResult::meta));
    }
};

//==============================================================================
//  logging/setLevel  +  logging notification
//==============================================================================
struct SetLevelParams { LoggingLevel level; Json meta = Json::object(); };
template <> struct CodecOf<SetLevelParams> {
    static Codec<SetLevelParams> get() {
        return record<SetLevelParams>(
            required("level", &SetLevelParams::level), meta("_meta", &SetLevelParams::meta));
    }
};
struct LoggingMessageParams {
    LoggingLevel       level;
    Json               data;
    Maybe<std::string> logger;
    Json               meta = Json::object();
};
template <> struct CodecOf<LoggingMessageParams> {
    static Codec<LoggingMessageParams> get() {
        return record<LoggingMessageParams>(
            required("level",  &LoggingMessageParams::level),
            required("data",   &LoggingMessageParams::data),
            optional("logger", &LoggingMessageParams::logger),
            meta    ("_meta",  &LoggingMessageParams::meta));
    }
};

//==============================================================================
//  completion/complete
//==============================================================================
struct CompleteArgument { std::string name; std::string value; };
template <> struct CodecOf<CompleteArgument> {
    static Codec<CompleteArgument> get() {
        return record<CompleteArgument>(
            required("name",  &CompleteArgument::name),
            required("value", &CompleteArgument::value));
    }
};
struct CompleteParams {
    CompletionReference ref;
    CompleteArgument    argument;
    Maybe<Json>         context;
    Json                meta = Json::object();
};
template <> struct CodecOf<CompleteParams> {
    static Codec<CompleteParams> get() {
        return record<CompleteParams>(
            required("ref",      &CompleteParams::ref),
            required("argument", &CompleteParams::argument),
            optional("context",  &CompleteParams::context),
            meta    ("_meta",    &CompleteParams::meta));
    }
};
struct Completion {
    List<std::string>   values;
    Maybe<std::int64_t> total;
    Maybe<bool>         hasMore;
};
template <> struct CodecOf<Completion> {
    static Codec<Completion> get() {
        return record<Completion>(
            required("values",  &Completion::values),
            optional("total",   &Completion::total),
            optional("hasMore", &Completion::hasMore));
    }
};
struct CompleteResult { Completion completion; Json meta = Json::object(); };
template <> struct CodecOf<CompleteResult> {
    static Codec<CompleteResult> get() {
        return record<CompleteResult>(
            required("completion", &CompleteResult::completion),
            meta    ("_meta",      &CompleteResult::meta));
    }
};

//==============================================================================
//  sampling/createMessage
//==============================================================================
enum class IncludeContext { None, ThisServer, AllServers };
template <> struct CodecOf<IncludeContext> {
    static Codec<IncludeContext> get() {
        return enum_codec<IncludeContext>(
            EnumMapping<IncludeContext>{IncludeContext::None,       "none"},
            EnumMapping<IncludeContext>{IncludeContext::ThisServer, "thisServer"},
            EnumMapping<IncludeContext>{IncludeContext::AllServers, "allServers"});
    }
};
struct CreateMessageParams {
    List<SamplingMessage>    messages;
    std::int64_t             maxTokens = 0;
    Maybe<ModelPreferences>  modelPreferences;
    Maybe<std::string>       systemPrompt;
    Maybe<IncludeContext>    includeContext;
    Maybe<double>            temperature;
    Maybe<List<std::string>> stopSequences;
    Maybe<Json>              metadata;
    Maybe<List<Tool>>        tools;
    Maybe<ToolChoice>        toolChoice;
    Maybe<TaskMetadata>      task;
    Json                     meta = Json::object();
};
template <> struct CodecOf<CreateMessageParams> {
    static Codec<CreateMessageParams> get() {
        return record<CreateMessageParams>(
            required("messages",         &CreateMessageParams::messages),
            required("maxTokens",        &CreateMessageParams::maxTokens),
            optional("modelPreferences", &CreateMessageParams::modelPreferences),
            optional("systemPrompt",     &CreateMessageParams::systemPrompt),
            optional("includeContext",   &CreateMessageParams::includeContext),
            optional("temperature",      &CreateMessageParams::temperature),
            optional("stopSequences",    &CreateMessageParams::stopSequences),
            optional("metadata",         &CreateMessageParams::metadata),
            optional("tools",            &CreateMessageParams::tools),
            optional("toolChoice",       &CreateMessageParams::toolChoice),
            optional("task",             &CreateMessageParams::task),
            meta    ("_meta",            &CreateMessageParams::meta));
    }
};
struct CreateMessageResult {
    Role                       role = Role::Assistant;
    List<SamplingContentBlock> content;
    std::string                model;
    Maybe<std::string>         stopReason;
    Json                       meta = Json::object();
};
template <> struct CodecOf<CreateMessageResult> {
    static Codec<CreateMessageResult> get() {
        // content is a single block on the wire for CreateMessageResult
        // (it extends SamplingMessage which allows block | block[]).
        auto one    = codec<SamplingContentBlock>();
        auto blocks = list_codec(codec<SamplingContentBlock>());
        return {
            [one](const CreateMessageResult& r) -> Json {
                Json j = Json::object();
                j["role"]  = to_json(r.role);
                j["model"] = r.model;
                j["content"] = r.content.size() == 1 ? one.encode(r.content[0])
                                                      : Json(Json::array());
                if (r.content.size() != 1) {
                    Json a = Json::array();
                    for (const auto& b : r.content) a.push_back(one.encode(b));
                    j["content"] = std::move(a);
                }
                if (r.stopReason) j["stopReason"] = *r.stopReason;
                if (!(r.meta.is_object() && r.meta.empty())) j["_meta"] = r.meta;
                return j;
            },
            [one, blocks](const Json& j) -> CreateMessageResult {
                CreateMessageResult r;
                r.role  = from_json<Role>(j.at("role"));
                r.model = j.at("model").get<std::string>();
                const Json& c = j.at("content");
                if (c.is_array()) r.content = blocks.decode(c);
                else              r.content = {one.decode(c)};
                if (auto it = j.find("stopReason"); it != j.end() && it->is_string())
                    r.stopReason = it->get<std::string>();
                if (auto it = j.find("_meta"); it != j.end()) r.meta = *it;
                return r;
            }};
    }
};

//==============================================================================
//  roots/list
//==============================================================================
struct ListRootsResult { List<Root> roots; Json meta = Json::object(); };
template <> struct CodecOf<ListRootsResult> {
    static Codec<ListRootsResult> get() {
        return record<ListRootsResult>(
            required("roots", &ListRootsResult::roots), meta("_meta", &ListRootsResult::meta));
    }
};

//==============================================================================
//  elicitation/create  (form & url modes)
//==============================================================================
struct ElicitFormParams {
    std::string                                          message;
    std::vector<std::pair<std::string, PrimitiveSchema>> properties;  // ordered map
    Maybe<List<std::string>>                             required;
    Maybe<TaskMetadata>                                  task;
    Json                                                 meta = Json::object();
};
template <> struct CodecOf<ElicitFormParams> {
    static Codec<ElicitFormParams> get() {
        auto sc = codec<PrimitiveSchema>();
        return {
            [sc](const ElicitFormParams& p) -> Json {
                Json props = Json::object();
                for (const auto& [k, v] : p.properties) props[k] = sc.encode(v);
                Json schema = {{"type", "object"}, {"properties", std::move(props)}};
                if (p.required) schema["required"] = *p.required;
                Json j = {{"mode", "form"}, {"message", p.message}, {"requestedSchema", schema}};
                if (p.task) j["task"] = to_json(*p.task);
                if (!(p.meta.is_object() && p.meta.empty())) j["_meta"] = p.meta;
                return j;
            },
            [sc](const Json& j) -> ElicitFormParams {
                ElicitFormParams p;
                p.message = j.at("message").get<std::string>();
                const Json& schema = j.at("requestedSchema");
                if (auto it = schema.find("properties"); it != schema.end())
                    for (auto& [k, v] : it->items())
                        p.properties.emplace_back(k, sc.decode(v));
                if (auto it = schema.find("required"); it != schema.end())
                    p.required = it->get<List<std::string>>();
                if (auto it = j.find("task"); it != j.end()) p.task = from_json<TaskMetadata>(*it);
                if (auto it = j.find("_meta"); it != j.end()) p.meta = *it;
                return p;
            }};
    }
};

struct ElicitUrlParams {
    std::string         message;
    std::string         elicitationId;
    std::string         url;
    Maybe<TaskMetadata> task;
    Json                meta = Json::object();
};
template <> struct CodecOf<ElicitUrlParams> {
    static Codec<ElicitUrlParams> get() {
        return {
            [](const ElicitUrlParams& p) -> Json {
                Json j = {{"mode", "url"}, {"message", p.message},
                          {"elicitationId", p.elicitationId}, {"url", p.url}};
                if (p.task) j["task"] = to_json(*p.task);
                if (!(p.meta.is_object() && p.meta.empty())) j["_meta"] = p.meta;
                return j;
            },
            [](const Json& j) -> ElicitUrlParams {
                ElicitUrlParams p;
                p.message       = j.at("message").get<std::string>();
                p.elicitationId = j.at("elicitationId").get<std::string>();
                p.url           = j.at("url").get<std::string>();
                if (auto it = j.find("task"); it != j.end()) p.task = from_json<TaskMetadata>(*it);
                if (auto it = j.find("_meta"); it != j.end()) p.meta = *it;
                return p;
            }};
    }
};

using ElicitParams = Sum<ElicitFormParams, ElicitUrlParams>;
template <> struct CodecOf<ElicitParams> {
    static Codec<ElicitParams> get() {
        return {
            [](const ElicitParams& p) -> Json {
                return std::visit([](const auto& x) { return to_json(x); }, p);
            },
            [](const Json& j) -> ElicitParams {
                if (j.value("mode", "form") == "url")
                    return ElicitParams{from_json<ElicitUrlParams>(j)};
                return ElicitParams{from_json<ElicitFormParams>(j)};
            }};
    }
};

struct ElicitResult {
    ElicitAction action;
    Maybe<std::vector<std::pair<std::string, ElicitValue>>> content;
    Json         meta = Json::object();
};
template <> struct CodecOf<ElicitResult> {
    static Codec<ElicitResult> get() {
        auto vc = codec<ElicitValue>();
        return {
            [vc](const ElicitResult& r) -> Json {
                Json j = {{"action", to_json(r.action)}};
                if (r.content) {
                    Json c = Json::object();
                    for (const auto& [k, v] : *r.content) c[k] = vc.encode(v);
                    j["content"] = std::move(c);
                }
                if (!(r.meta.is_object() && r.meta.empty())) j["_meta"] = r.meta;
                return j;
            },
            [vc](const Json& j) -> ElicitResult {
                ElicitResult r;
                r.action = from_json<ElicitAction>(j.at("action"));
                if (auto it = j.find("content"); it != j.end() && it->is_object()) {
                    std::vector<std::pair<std::string, ElicitValue>> c;
                    for (auto& [k, v] : it->items()) c.emplace_back(k, vc.decode(v));
                    r.content = std::move(c);
                }
                if (auto it = j.find("_meta"); it != j.end()) r.meta = *it;
                return r;
            }};
    }
};

//==============================================================================
//  Tasks (durable requests).
//==============================================================================
struct CreateTaskResult { Task task; Json meta = Json::object(); };
template <> struct CodecOf<CreateTaskResult> {
    static Codec<CreateTaskResult> get() {
        return record<CreateTaskResult>(
            required("task",  &CreateTaskResult::task), meta("_meta", &CreateTaskResult::meta));
    }
};
struct TaskIdParams { std::string taskId; };
template <> struct CodecOf<TaskIdParams> {
    static Codec<TaskIdParams> get() {
        return record<TaskIdParams>(required("taskId", &TaskIdParams::taskId));
    }
};
//  GetTaskResult / CancelTaskResult = Result & Task (flattened). We reuse Task
//  directly; its codec already produces/consumes the right object shape.
using GetTaskResult    = Task;
using CancelTaskResult = Task;
//  GetTaskPayloadResult is an opaque result blob.
using GetTaskPayloadResult = Json;
struct ListTasksResult {
    List<Task>         tasks;
    Maybe<std::string> nextCursor;
    Json               meta = Json::object();
};
template <> struct CodecOf<ListTasksResult> {
    static Codec<ListTasksResult> get() {
        return record<ListTasksResult>(
            defaulted("tasks",      &ListTasksResult::tasks, List<Task>{}),
            optional ("nextCursor", &ListTasksResult::nextCursor),
            meta     ("_meta",      &ListTasksResult::meta));
    }
};

//==============================================================================
//  Notifications.
//==============================================================================
struct CancelledParams {
    Maybe<RequestId>   requestId;
    Maybe<std::string> reason;
    Json               meta = Json::object();
};
template <> struct CodecOf<CancelledParams> {
    static Codec<CancelledParams> get() {
        return record<CancelledParams>(
            optional("requestId", &CancelledParams::requestId, scalar_codec()),
            optional("reason",    &CancelledParams::reason),
            meta    ("_meta",     &CancelledParams::meta));
    }
};

struct ProgressParams {
    ProgressToken      progressToken;
    double             progress = 0;
    Maybe<double>      total;
    Maybe<std::string> message;
    Json               meta = Json::object();
};
template <> struct CodecOf<ProgressParams> {
    static Codec<ProgressParams> get() {
        return record<ProgressParams>(
            required("progressToken", &ProgressParams::progressToken, scalar_codec()),
            required("progress",      &ProgressParams::progress),
            optional("total",         &ProgressParams::total),
            optional("message",       &ProgressParams::message),
            meta    ("_meta",         &ProgressParams::meta));
    }
};

struct ResourceUpdatedParams { std::string uri; Json meta = Json::object(); };
template <> struct CodecOf<ResourceUpdatedParams> {
    static Codec<ResourceUpdatedParams> get() {
        return record<ResourceUpdatedParams>(
            required("uri", &ResourceUpdatedParams::uri),
            meta    ("_meta", &ResourceUpdatedParams::meta));
    }
};

//  TaskStatusNotification params = NotificationParams & Task (flattened).
using TaskStatusParams = Task;

struct ElicitationCompleteParams { std::string elicitationId; };
template <> struct CodecOf<ElicitationCompleteParams> {
    static Codec<ElicitationCompleteParams> get() {
        return record<ElicitationCompleteParams>(
            required("elicitationId", &ElicitationCompleteParams::elicitationId));
    }
};

} // namespace mcp
