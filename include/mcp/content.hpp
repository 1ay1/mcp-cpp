// SPDX-License-Identifier: Apache-2.0
//
// mcp/content.hpp — content blocks, annotations, and resource contents.
//
//   The schema's content model is a family of tagged sums keyed on "type":
//
//     ContentBlock = text | image | audio | resource_link | resource
//     SamplingMessageContentBlock = text | image | audio | tool_use | tool_result
//
//   We give each arm a struct and assemble the sum with `sum_tagged("type", …)`.
//   The two sums SHARE the leaf structs (TextContent, ImageContent, …) so a
//   TextContent flows through both without duplication.
//
#pragma once

#include <mcp/ids.hpp>

namespace mcp {

//==============================================================================
//  Annotations — optional metadata hints (schema.ts Annotations).
//==============================================================================
struct Annotations {
    Maybe<List<Role>>    audience;
    Maybe<double>        priority;
    Maybe<std::string>   lastModified;   // ISO 8601
};
template <> struct CodecOf<Annotations> {
    static Codec<Annotations> get() {
        return record<Annotations>(
            optional("audience",     &Annotations::audience),
            optional("priority",     &Annotations::priority),
            optional("lastModified", &Annotations::lastModified));
    }
};

//==============================================================================
//  Resource contents — text or blob, both carry uri + optional mimeType.
//==============================================================================
struct TextResourceContents {
    std::string        uri;
    std::string        text;
    Maybe<std::string> mimeType;
    Json               meta = Json::object();
};
template <> struct CodecOf<TextResourceContents> {
    static Codec<TextResourceContents> get() {
        return record<TextResourceContents>(
            required("uri",       &TextResourceContents::uri),
            required("text",      &TextResourceContents::text),
            optional("mimeType",  &TextResourceContents::mimeType),
            meta    ("_meta",     &TextResourceContents::meta));
    }
};

struct BlobResourceContents {
    std::string        uri;
    std::string        blob;   // base64
    Maybe<std::string> mimeType;
    Json               meta = Json::object();
};
template <> struct CodecOf<BlobResourceContents> {
    static Codec<BlobResourceContents> get() {
        return record<BlobResourceContents>(
            required("uri",       &BlobResourceContents::uri),
            required("blob",      &BlobResourceContents::blob),
            optional("mimeType",  &BlobResourceContents::mimeType),
            meta    ("_meta",     &BlobResourceContents::meta));
    }
};

//  ResourceContents = text | blob   (structural — disambiguated by which of
//  "text"/"blob" is present, so an untagged variant_codec fits).
using ResourceContents = Sum<TextResourceContents, BlobResourceContents>;
template <> struct CodecOf<ResourceContents> {
    static Codec<ResourceContents> get() {
        return variant_codec<ResourceContents>(
            codec<TextResourceContents>(), codec<BlobResourceContents>());
    }
};

//==============================================================================
//  Content block leaves.
//==============================================================================
struct TextContent {
    std::string        text;
    Maybe<Annotations> annotations;
    Json               meta = Json::object();
};
template <> struct CodecOf<TextContent> {
    static Codec<TextContent> get() {
        return record<TextContent>(
            required("text",        &TextContent::text),
            optional("annotations", &TextContent::annotations),
            meta    ("_meta",       &TextContent::meta));
    }
};

struct ImageContent {
    std::string        data;       // base64
    std::string        mimeType;
    Maybe<Annotations> annotations;
    Json               meta = Json::object();
};
template <> struct CodecOf<ImageContent> {
    static Codec<ImageContent> get() {
        return record<ImageContent>(
            required("data",        &ImageContent::data),
            required("mimeType",    &ImageContent::mimeType),
            optional("annotations", &ImageContent::annotations),
            meta    ("_meta",       &ImageContent::meta));
    }
};

struct AudioContent {
    std::string        data;       // base64
    std::string        mimeType;
    Maybe<Annotations> annotations;
    Json               meta = Json::object();
};
template <> struct CodecOf<AudioContent> {
    static Codec<AudioContent> get() {
        return record<AudioContent>(
            required("data",        &AudioContent::data),
            required("mimeType",    &AudioContent::mimeType),
            optional("annotations", &AudioContent::annotations),
            meta    ("_meta",       &AudioContent::meta));
    }
};

//  EmbeddedResource — { type: "resource", resource: text|blob, … }
struct EmbeddedResource {
    ResourceContents   resource;
    Maybe<Annotations> annotations;
    Json               meta = Json::object();
};
template <> struct CodecOf<EmbeddedResource> {
    static Codec<EmbeddedResource> get() {
        return record<EmbeddedResource>(
            required("resource",    &EmbeddedResource::resource),
            optional("annotations", &EmbeddedResource::annotations),
            meta    ("_meta",       &EmbeddedResource::meta));
    }
};

//  ResourceLink — a Resource with type:"resource_link". We inline the Resource
//  fields here (rather than inherit) to keep the codec a flat record; the full
//  Resource type lives in types.hpp and shares the same wire shape.
struct ResourceLink {
    std::string        uri;
    std::string        name;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<std::string> mimeType;
    Maybe<Annotations> annotations;
    Maybe<std::int64_t> size;
    Json               meta = Json::object();
};
template <> struct CodecOf<ResourceLink> {
    static Codec<ResourceLink> get() {
        return record<ResourceLink>(
            required("uri",         &ResourceLink::uri),
            required("name",        &ResourceLink::name),
            optional("title",       &ResourceLink::title),
            optional("description", &ResourceLink::description),
            optional("mimeType",    &ResourceLink::mimeType),
            optional("annotations", &ResourceLink::annotations),
            optional("size",        &ResourceLink::size),
            meta    ("_meta",       &ResourceLink::meta));
    }
};

//==============================================================================
//  ContentBlock = text | image | audio | resource_link | resource
//==============================================================================
using ContentBlock =
    Sum<TextContent, ImageContent, AudioContent, ResourceLink, EmbeddedResource>;

template <> struct CodecOf<ContentBlock> {
    static Codec<ContentBlock> get() {
        return sum_tagged<ContentBlock>("type",
            arm<ContentBlock, TextContent>     ("text"),
            arm<ContentBlock, ImageContent>    ("image"),
            arm<ContentBlock, AudioContent>    ("audio"),
            arm<ContentBlock, ResourceLink>    ("resource_link"),
            arm<ContentBlock, EmbeddedResource>("resource"));
    }
};

//==============================================================================
//  Sampling content blocks — adds tool_use / tool_result.
//==============================================================================
struct ToolUseContent {
    std::string id;
    std::string name;
    Json        input = Json::object();
    Json        meta  = Json::object();
};
template <> struct CodecOf<ToolUseContent> {
    static Codec<ToolUseContent> get() {
        return record<ToolUseContent>(
            required ("id",    &ToolUseContent::id),
            required ("name",  &ToolUseContent::name),
            defaulted("input", &ToolUseContent::input, Json::object()),
            meta     ("_meta", &ToolUseContent::meta));
    }
};

struct ToolResultContent {
    std::string        toolUseId;
    List<ContentBlock> content;
    Maybe<Json>        structuredContent;
    Maybe<bool>        isError;
    Json               meta = Json::object();
};
template <> struct CodecOf<ToolResultContent> {
    static Codec<ToolResultContent> get() {
        return record<ToolResultContent>(
            required ("toolUseId",         &ToolResultContent::toolUseId),
            defaulted("content",           &ToolResultContent::content, List<ContentBlock>{}),
            optional ("structuredContent", &ToolResultContent::structuredContent),
            optional ("isError",           &ToolResultContent::isError),
            meta     ("_meta",             &ToolResultContent::meta));
    }
};

using SamplingContentBlock =
    Sum<TextContent, ImageContent, AudioContent, ToolUseContent, ToolResultContent>;

template <> struct CodecOf<SamplingContentBlock> {
    static Codec<SamplingContentBlock> get() {
        return sum_tagged<SamplingContentBlock>("type",
            arm<SamplingContentBlock, TextContent>      ("text"),
            arm<SamplingContentBlock, ImageContent>     ("image"),
            arm<SamplingContentBlock, AudioContent>     ("audio"),
            arm<SamplingContentBlock, ToolUseContent>   ("tool_use"),
            arm<SamplingContentBlock, ToolResultContent>("tool_result"));
    }
};

//==============================================================================
//  Convenience constructors — terse content authoring at call sites.
//==============================================================================
inline ContentBlock text(std::string s) {
    return ContentBlock{TextContent{std::move(s), Nothing, Json::object()}};
}
inline ContentBlock image(std::string base64, std::string mime) {
    return ContentBlock{ImageContent{std::move(base64), std::move(mime), Nothing, Json::object()}};
}

} // namespace mcp
