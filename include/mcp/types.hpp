// SPDX-License-Identifier: Apache-2.0
//
// mcp/types.hpp — the protocol's nominal data types (everything that isn't a
// method param/result record, which live in methods.hpp).
//
#pragma once

#include <mcp/content.hpp>

namespace mcp {

//==============================================================================
//  Icon — visual metadata for tools/resources/prompts (SEP-973).
//==============================================================================
enum class IconTheme { Light, Dark };
template <> struct CodecOf<IconTheme> {
    static Codec<IconTheme> get() {
        return enum_codec<IconTheme>(
            EnumMapping<IconTheme>{IconTheme::Light, "light"},
            EnumMapping<IconTheme>{IconTheme::Dark,  "dark"});
    }
};

struct Icon {
    std::string                  src;
    Maybe<std::string>           mimeType;
    Maybe<List<std::string>>     sizes;
    Maybe<IconTheme>             theme;
};
template <> struct CodecOf<Icon> {
    static Codec<Icon> get() {
        return record<Icon>(
            required("src",      &Icon::src),
            optional("mimeType", &Icon::mimeType),
            optional("sizes",    &Icon::sizes),
            optional("theme",    &Icon::theme));
    }
};

//==============================================================================
//  Implementation — peer identity exchanged at initialize.
//==============================================================================
struct Implementation {
    std::string        name;
    std::string        version;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<std::string> websiteUrl;
    Maybe<List<Icon>>  icons;
};
template <> struct CodecOf<Implementation> {
    static Codec<Implementation> get() {
        return record<Implementation>(
            required("name",        &Implementation::name),
            required("version",     &Implementation::version),
            optional("title",       &Implementation::title),
            optional("description", &Implementation::description),
            optional("websiteUrl",  &Implementation::websiteUrl),
            optional("icons",       &Implementation::icons));
    }
};

//==============================================================================
//  Capabilities — what each peer offers. The schema nests booleans under
//  feature objects; we model the presence of a feature as Maybe<...> and the
//  sub-flags as their own small records.
//==============================================================================
struct RootsCapability    { Maybe<bool> listChanged; };
struct SamplingCapability { Maybe<Json> context; Maybe<Json> tools; };
struct ElicitationCapability { Maybe<Json> form; Maybe<Json> url; };

template <> struct CodecOf<RootsCapability> {
    static Codec<RootsCapability> get() {
        return record<RootsCapability>(optional("listChanged", &RootsCapability::listChanged));
    }
};
template <> struct CodecOf<SamplingCapability> {
    static Codec<SamplingCapability> get() {
        return record<SamplingCapability>(
            optional("context", &SamplingCapability::context),
            optional("tools",   &SamplingCapability::tools));
    }
};
template <> struct CodecOf<ElicitationCapability> {
    static Codec<ElicitationCapability> get() {
        return record<ElicitationCapability>(
            optional("form", &ElicitationCapability::form),
            optional("url",  &ElicitationCapability::url));
    }
};

struct TasksCapability {
    Maybe<Json> list;
    Maybe<Json> cancel;
    Maybe<Json> requests;    // nested shape varies client/server; kept opaque
};
template <> struct CodecOf<TasksCapability> {
    static Codec<TasksCapability> get() {
        return record<TasksCapability>(
            optional("list",     &TasksCapability::list),
            optional("cancel",   &TasksCapability::cancel),
            optional("requests", &TasksCapability::requests));
    }
};

struct ClientCapabilities {
    Maybe<Json>                 experimental;
    Maybe<RootsCapability>      roots;
    Maybe<SamplingCapability>   sampling;
    Maybe<ElicitationCapability> elicitation;
    Maybe<TasksCapability>      tasks;
};
template <> struct CodecOf<ClientCapabilities> {
    static Codec<ClientCapabilities> get() {
        return record<ClientCapabilities>(
            optional("experimental", &ClientCapabilities::experimental),
            optional("roots",        &ClientCapabilities::roots),
            optional("sampling",     &ClientCapabilities::sampling),
            optional("elicitation",  &ClientCapabilities::elicitation),
            optional("tasks",        &ClientCapabilities::tasks));
    }
};

struct PromptsCapability   { Maybe<bool> listChanged; };
struct ResourcesCapability { Maybe<bool> subscribe; Maybe<bool> listChanged; };
struct ToolsCapability     { Maybe<bool> listChanged; };

template <> struct CodecOf<PromptsCapability> {
    static Codec<PromptsCapability> get() {
        return record<PromptsCapability>(optional("listChanged", &PromptsCapability::listChanged));
    }
};
template <> struct CodecOf<ResourcesCapability> {
    static Codec<ResourcesCapability> get() {
        return record<ResourcesCapability>(
            optional("subscribe",   &ResourcesCapability::subscribe),
            optional("listChanged", &ResourcesCapability::listChanged));
    }
};
template <> struct CodecOf<ToolsCapability> {
    static Codec<ToolsCapability> get() {
        return record<ToolsCapability>(optional("listChanged", &ToolsCapability::listChanged));
    }
};

struct ServerCapabilities {
    Maybe<Json>               experimental;
    Maybe<Json>               logging;
    Maybe<Json>               completions;
    Maybe<PromptsCapability>  prompts;
    Maybe<ResourcesCapability> resources;
    Maybe<ToolsCapability>    tools;
    Maybe<TasksCapability>    tasks;
};
template <> struct CodecOf<ServerCapabilities> {
    static Codec<ServerCapabilities> get() {
        return record<ServerCapabilities>(
            optional("experimental", &ServerCapabilities::experimental),
            optional("logging",      &ServerCapabilities::logging),
            optional("completions",  &ServerCapabilities::completions),
            optional("prompts",      &ServerCapabilities::prompts),
            optional("resources",    &ServerCapabilities::resources),
            optional("tools",        &ServerCapabilities::tools),
            optional("tasks",        &ServerCapabilities::tasks));
    }
};

//==============================================================================
//  Tool — the central server primitive.
//==============================================================================
struct JsonSchema {
    Maybe<std::string> schema;       // "$schema"
    Maybe<Json>        properties;
    Maybe<List<std::string>> required;
};
template <> struct CodecOf<JsonSchema> {
    static Codec<JsonSchema> get() {
        // type is always "object"; emitted as a constant via a wrapper below.
        return {
            [](const JsonSchema& s) -> Json {
                Json j = Json::object();
                j["type"] = "object";
                if (s.schema)     j["$schema"]    = *s.schema;
                if (s.properties) j["properties"] = *s.properties;
                if (s.required)   j["required"]   = *s.required;
                return j;
            },
            [](const Json& j) -> JsonSchema {
                JsonSchema s;
                if (auto it = j.find("$schema"); it != j.end() && it->is_string())
                    s.schema = it->get<std::string>();
                if (auto it = j.find("properties"); it != j.end())
                    s.properties = *it;
                if (auto it = j.find("required"); it != j.end() && it->is_array())
                    s.required = it->get<List<std::string>>();
                return s;
            }};
    }
};

struct ToolAnnotations {
    Maybe<std::string> title;
    Maybe<bool>        readOnlyHint;
    Maybe<bool>        destructiveHint;
    Maybe<bool>        idempotentHint;
    Maybe<bool>        openWorldHint;
};
template <> struct CodecOf<ToolAnnotations> {
    static Codec<ToolAnnotations> get() {
        return record<ToolAnnotations>(
            optional("title",           &ToolAnnotations::title),
            optional("readOnlyHint",    &ToolAnnotations::readOnlyHint),
            optional("destructiveHint", &ToolAnnotations::destructiveHint),
            optional("idempotentHint",  &ToolAnnotations::idempotentHint),
            optional("openWorldHint",   &ToolAnnotations::openWorldHint));
    }
};

enum class TaskSupport { Forbidden, Optional, Required };
template <> struct CodecOf<TaskSupport> {
    static Codec<TaskSupport> get() {
        return enum_codec<TaskSupport>(
            EnumMapping<TaskSupport>{TaskSupport::Forbidden, "forbidden"},
            EnumMapping<TaskSupport>{TaskSupport::Optional,  "optional"},
            EnumMapping<TaskSupport>{TaskSupport::Required,  "required"});
    }
};

struct ToolExecution { Maybe<TaskSupport> taskSupport; };
template <> struct CodecOf<ToolExecution> {
    static Codec<ToolExecution> get() {
        return record<ToolExecution>(optional("taskSupport", &ToolExecution::taskSupport));
    }
};

struct Tool {
    std::string            name;
    JsonSchema             inputSchema;
    Maybe<std::string>     title;
    Maybe<std::string>     description;
    Maybe<JsonSchema>      outputSchema;
    Maybe<ToolExecution>   execution;
    Maybe<ToolAnnotations> annotations;
    Maybe<List<Icon>>      icons;
    Json                   meta = Json::object();
};
template <> struct CodecOf<Tool> {
    static Codec<Tool> get() {
        return record<Tool>(
            required("name",         &Tool::name),
            required("inputSchema",  &Tool::inputSchema),
            optional("title",        &Tool::title),
            optional("description",  &Tool::description),
            optional("outputSchema", &Tool::outputSchema),
            optional("execution",    &Tool::execution),
            optional("annotations",  &Tool::annotations),
            optional("icons",        &Tool::icons),
            meta    ("_meta",        &Tool::meta));
    }
};

//==============================================================================
//  Resource & ResourceTemplate.
//==============================================================================
struct Resource {
    std::string         uri;
    std::string         name;
    Maybe<std::string>  title;
    Maybe<std::string>  description;
    Maybe<std::string>  mimeType;
    Maybe<Annotations>  annotations;
    Maybe<std::int64_t> size;
    Maybe<List<Icon>>   icons;
    Json                meta = Json::object();
};
template <> struct CodecOf<Resource> {
    static Codec<Resource> get() {
        return record<Resource>(
            required("uri",         &Resource::uri),
            required("name",        &Resource::name),
            optional("title",       &Resource::title),
            optional("description", &Resource::description),
            optional("mimeType",    &Resource::mimeType),
            optional("annotations", &Resource::annotations),
            optional("size",        &Resource::size),
            optional("icons",       &Resource::icons),
            meta    ("_meta",       &Resource::meta));
    }
};

struct ResourceTemplate {
    std::string        uriTemplate;
    std::string        name;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<std::string> mimeType;
    Maybe<Annotations> annotations;
    Maybe<List<Icon>>  icons;
    Json               meta = Json::object();
};
template <> struct CodecOf<ResourceTemplate> {
    static Codec<ResourceTemplate> get() {
        return record<ResourceTemplate>(
            required("uriTemplate", &ResourceTemplate::uriTemplate),
            required("name",        &ResourceTemplate::name),
            optional("title",       &ResourceTemplate::title),
            optional("description", &ResourceTemplate::description),
            optional("mimeType",    &ResourceTemplate::mimeType),
            optional("annotations", &ResourceTemplate::annotations),
            optional("icons",       &ResourceTemplate::icons),
            meta    ("_meta",       &ResourceTemplate::meta));
    }
};

//==============================================================================
//  Prompt.
//==============================================================================
struct PromptArgument {
    std::string        name;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<bool>        required;
};
template <> struct CodecOf<PromptArgument> {
    static Codec<PromptArgument> get() {
        return record<PromptArgument>(
            required("name",        &PromptArgument::name),
            optional("title",       &PromptArgument::title),
            optional("description", &PromptArgument::description),
            optional("required",    &PromptArgument::required));
    }
};

struct Prompt {
    std::string               name;
    Maybe<std::string>        title;
    Maybe<std::string>        description;
    Maybe<List<PromptArgument>> arguments;
    Maybe<List<Icon>>         icons;
    Json                      meta = Json::object();
};
template <> struct CodecOf<Prompt> {
    static Codec<Prompt> get() {
        return record<Prompt>(
            required("name",        &Prompt::name),
            optional("title",       &Prompt::title),
            optional("description", &Prompt::description),
            optional("arguments",   &Prompt::arguments),
            optional("icons",       &Prompt::icons),
            meta    ("_meta",       &Prompt::meta));
    }
};

struct PromptMessage {
    Role         role;
    ContentBlock content;
};
template <> struct CodecOf<PromptMessage> {
    static Codec<PromptMessage> get() {
        return record<PromptMessage>(
            required("role",    &PromptMessage::role),
            required("content", &PromptMessage::content));
    }
};

//==============================================================================
//  Sampling — model preferences + messages.
//==============================================================================
struct ModelHint { Maybe<std::string> name; };
template <> struct CodecOf<ModelHint> {
    static Codec<ModelHint> get() {
        return record<ModelHint>(optional("name", &ModelHint::name));
    }
};

struct ModelPreferences {
    Maybe<List<ModelHint>> hints;
    Maybe<double>          costPriority;
    Maybe<double>          speedPriority;
    Maybe<double>          intelligencePriority;
};
template <> struct CodecOf<ModelPreferences> {
    static Codec<ModelPreferences> get() {
        return record<ModelPreferences>(
            optional("hints",                &ModelPreferences::hints),
            optional("costPriority",         &ModelPreferences::costPriority),
            optional("speedPriority",        &ModelPreferences::speedPriority),
            optional("intelligencePriority", &ModelPreferences::intelligencePriority));
    }
};

//  SamplingMessage.content = block | block[]. We always store a list and
//  normalise on encode/decode (single block ⇄ one-element list).
struct SamplingMessage {
    Role                       role;
    List<SamplingContentBlock> content;
    Json                       meta = Json::object();
};
template <> struct CodecOf<SamplingMessage> {
    static Codec<SamplingMessage> get() {
        auto blocks = list_codec(codec<SamplingContentBlock>());
        auto one    = codec<SamplingContentBlock>();
        return {
            [blocks](const SamplingMessage& m) -> Json {
                Json j = Json::object();
                j["role"]    = to_json(m.role);
                j["content"] = blocks.encode(m.content);
                if (!(m.meta.is_object() && m.meta.empty())) j["_meta"] = m.meta;
                return j;
            },
            [blocks, one](const Json& j) -> SamplingMessage {
                SamplingMessage m;
                m.role = from_json<Role>(j.at("role"));
                const Json& c = j.at("content");
                if (c.is_array()) m.content = blocks.decode(c);
                else              m.content = {one.decode(c)};
                if (auto it = j.find("_meta"); it != j.end()) m.meta = *it;
                return m;
            }};
    }
};

enum class ToolChoiceMode { Auto, Required, None };
template <> struct CodecOf<ToolChoiceMode> {
    static Codec<ToolChoiceMode> get() {
        return enum_codec<ToolChoiceMode>(
            EnumMapping<ToolChoiceMode>{ToolChoiceMode::Auto,     "auto"},
            EnumMapping<ToolChoiceMode>{ToolChoiceMode::Required, "required"},
            EnumMapping<ToolChoiceMode>{ToolChoiceMode::None,     "none"});
    }
};
struct ToolChoice { Maybe<ToolChoiceMode> mode; };
template <> struct CodecOf<ToolChoice> {
    static Codec<ToolChoice> get() {
        return record<ToolChoice>(optional("mode", &ToolChoice::mode));
    }
};

//==============================================================================
//  Task — durable request bookkeeping.
//==============================================================================
struct TaskMetadata { Maybe<double> ttl; };
template <> struct CodecOf<TaskMetadata> {
    static Codec<TaskMetadata> get() {
        return record<TaskMetadata>(optional("ttl", &TaskMetadata::ttl));
    }
};

struct Task {
    std::string        taskId;
    TaskStatus         status;
    std::string        createdAt;       // ISO 8601
    std::string        lastUpdatedAt;   // ISO 8601
    Maybe<double>      ttl;             // null ⇒ no expiry; we use Nothing for that
    Maybe<std::string> statusMessage;
    Maybe<double>      pollInterval;
};
template <> struct CodecOf<Task> {
    static Codec<Task> get() {
        return record<Task>(
            required("taskId",        &Task::taskId),
            required("status",        &Task::status),
            required("createdAt",     &Task::createdAt),
            required("lastUpdatedAt", &Task::lastUpdatedAt),
            optional("ttl",           &Task::ttl),
            optional("statusMessage", &Task::statusMessage),
            optional("pollInterval",  &Task::pollInterval));
    }
};

//==============================================================================
//  Root — a filesystem/URI boundary the server may operate within.
//==============================================================================
struct Root {
    std::string        uri;
    Maybe<std::string> name;
    Json               meta = Json::object();
};
template <> struct CodecOf<Root> {
    static Codec<Root> get() {
        return record<Root>(
            required("uri",   &Root::uri),
            optional("name",  &Root::name),
            meta    ("_meta", &Root::meta));
    }
};

//==============================================================================
//  Completion references (autocomplete).
//==============================================================================
struct PromptReference { std::string name; Maybe<std::string> title; };
struct ResourceTemplateReference { std::string uri; };
template <> struct CodecOf<PromptReference> {
    static Codec<PromptReference> get() {
        // wire: { type: "ref/prompt", name, title? }
        return {
            [](const PromptReference& r) -> Json {
                Json j = {{"type", "ref/prompt"}, {"name", r.name}};
                if (r.title) j["title"] = *r.title;
                return j;
            },
            [](const Json& j) -> PromptReference {
                PromptReference r; r.name = j.at("name").get<std::string>();
                if (auto it = j.find("title"); it != j.end() && it->is_string())
                    r.title = it->get<std::string>();
                return r;
            }};
    }
};
template <> struct CodecOf<ResourceTemplateReference> {
    static Codec<ResourceTemplateReference> get() {
        return {
            [](const ResourceTemplateReference& r) -> Json {
                return Json{{"type", "ref/resource"}, {"uri", r.uri}};
            },
            [](const Json& j) -> ResourceTemplateReference {
                return ResourceTemplateReference{j.at("uri").get<std::string>()};
            }};
    }
};
using CompletionReference = Sum<PromptReference, ResourceTemplateReference>;
template <> struct CodecOf<CompletionReference> {
    static Codec<CompletionReference> get() {
        return {
            [](const CompletionReference& r) -> Json {
                return std::visit([](const auto& x) { return to_json(x); }, r);
            },
            [](const Json& j) -> CompletionReference {
                const auto t = j.value("type", std::string{});
                if (t == "ref/prompt")   return CompletionReference{from_json<PromptReference>(j)};
                if (t == "ref/resource") return CompletionReference{from_json<ResourceTemplateReference>(j)};
                throw CodecError("unknown completion ref type: '" + t + "'");
            }};
    }
};

} // namespace mcp
