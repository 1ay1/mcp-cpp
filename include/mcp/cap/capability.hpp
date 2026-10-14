// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/capability.hpp — the CAPABILITY LAYER: one uniform abstraction for
// "things the agent can do", regardless of where they live.
//
//   The MCP wire protocol (client.hpp / server.hpp) answers "how do two peers
//   talk JSON-RPC". This layer answers the question an AGENT actually has:
//
//        What can I do?      → CapabilityProvider::list()
//        Do this thing.      → CapabilityProvider::execute(Request)
//
//   A provider may be a set of in-process C++ closures (LocalProvider), a
//   spawned MCP server over stdio (StdioServerProvider), a remote MCP server
//   over HTTP, an RPC service — the agent CANNOT tell them apart, and that is
//   the whole point. New providers = subclass one interface + register it.
//
//   This decouples the host application from MCP entirely: a host registers
//   providers into a Registry and asks the Registry for tools + dispatch. MCP
//   is just *one kind of provider*. Remove it and the abstraction is intact.
//
#pragma once

#include <mcp/types.hpp>     // Tool, JsonSchema, Resource, Prompt
#include <mcp/content.hpp>   // ContentBlock, TextContent, ResourceContents
#include <mcp/methods.hpp>   // CallToolResult, GetPromptResult
#include <mcp/codec.hpp>     // to_json

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::cap {

// ── What a capability does to the world ──────────────────────────────────────
// The one effect type in mcp-cpp. Permission policy and the parallel
// scheduler both read it; tools declare it.
enum class Effect : std::uint8_t {
    ReadFs  = 1u << 0,
    WriteFs = 1u << 1,
    Net     = 1u << 2,
    Exec    = 1u << 3,
};

class EffectSet {
public:
    constexpr EffectSet() = default;
    constexpr EffectSet(std::initializer_list<Effect> es) {
        for (auto e : es) bits_ |= static_cast<std::uint8_t>(e);
    }
    constexpr explicit EffectSet(std::uint8_t bits) : bits_(bits & 0x0Fu) {}
    [[nodiscard]] constexpr bool has(Effect e) const {
        return (bits_ & static_cast<std::uint8_t>(e)) != 0;
    }
    [[nodiscard]] constexpr std::uint8_t bits() const { return bits_; }
    [[nodiscard]] constexpr bool empty() const { return bits_ == 0; }
    [[nodiscard]] constexpr bool writes() const { return has(Effect::WriteFs); }
    [[nodiscard]] constexpr bool execs()  const { return has(Effect::Exec); }
    /// Touches the filesystem in any way.
    [[nodiscard]] constexpr bool touches_fs() const {
        return has(Effect::ReadFs) || has(Effect::WriteFs);
    }
    /// Reads and/or talks to the network, never writes or runs anything.
    /// Two pure-read calls are always safe to run at once.
    [[nodiscard]] constexpr bool pure_read() const { return !writes() && !execs(); }
    constexpr EffectSet operator|(EffectSet o) const { return EffectSet(std::uint8_t(bits_ | o.bits_)); }
    friend constexpr bool operator==(EffectSet, EffectSet) = default;
private:
    std::uint8_t bits_ = 0;
};

// A file a capability wrote: enough for a host's diff-review UI.
struct FileChange {
    std::string path;
    int         added   = 0;
    int         removed = 0;
    std::string before;     // "" for a new file
    std::string after;
};

// An image a capability wants a vision model to see.
struct Image {
    std::string media_type;   // "image/png", "image/jpeg", ...
    std::string bytes;        // raw, not base64
};

// A request to perform one capability. `tool` is the UNQUALIFIED tool name as
// the provider advertised it (the Registry strips any namespace prefix before
// dispatch); `args` is the JSON object matching the tool's inputSchema.
struct Request {
    std::string tool;
    Json        args = Json::object();
    std::function<void(std::string_view)> progress = nullptr;
    std::function<bool()> cancelled = nullptr;
    // The same cancel as an event, when the caller has one. Lets a provider
    // react the moment it fires instead of polling `cancelled`.
    std::stop_token stop = {};
    // Whose context the call serves (a conversation, a subagent run). A
    // provider that remembers what it has shown someone keys on this.
    std::string reader = {};
    // The host's own per-call data, passed through untouched to whatever the
    // host plugged in (its Exec, SkillResolver, SubagentRunner). mcp-cpp
    // never reads it. Lets a host keep call-scoped facts off thread-locals.
    const void* host = nullptr;
    // Optional per-call workspace scope, for a host whose calls serve
    // different folders (one ACP agent, several editor sessions). Empty
    // fields leave the provider's shared defaults in place.
    std::string              scope_workspace = {};
    std::vector<std::string> scope_extra_roots = {};
    std::string              scope_project = {};
    // Ask the user (an MCP server's elicitation/create during this call).
    // MCP-shaped params in, ElicitResult JSON out. Empty: declined.
    std::function<Json(const Json&)> elicit = nullptr;
};

// The outcome of executing a capability. `text` is the human/model-facing
// rendering (already flattened from MCP content blocks); `structured` carries
// typed output when the provider produced it (MCP structuredContent); raw
// holds the original content-block list for callers that want full fidelity.
//
// effects / changes / images are what a rich host needs beside the text and
// the bare MCP wire has no field for. They are plain typed fields: a host
// reads them directly, and lowering to the wire (call_result_from) drops
// them, so a plain MCP client never sees them.
struct Result {
    std::string text;
    Json        structured = Json::object();   // empty object == none
    Json        raw        = Json::array();    // original content blocks
    bool        is_error   = false;

    EffectSet               effects;   // what the call did (declared by the tool)
    std::vector<FileChange> changes;   // files it wrote, one per path
    std::vector<Image>      images;    // pictures for a vision model

    [[nodiscard]] static Result ok(std::string t) {
        Result r; r.text = std::move(t); return r;
    }
    [[nodiscard]] static Result error(std::string t) {
        Result r; r.text = std::move(t); r.is_error = true; return r;
    }
};

// The one seam. A source of capabilities.
class CapabilityProvider {
public:
    virtual ~CapabilityProvider() = default;

    // Stable origin label for provenance/namespacing: "local", "mcp:github",
    // "http:bloomberg", … The Registry namespaces tools as "<origin>__<name>"
    // when more than one provider is present.
    [[nodiscard]] virtual std::string_view origin() const noexcept = 0;

    // Everything this provider can currently do. May be cached by the impl.
    [[nodiscard]] virtual std::vector<Tool> list() const = 0;

    // Perform one capability. MUST NOT throw — map any failure to
    // Result::error so the agent can react instead of crashing.
    [[nodiscard]] virtual Result execute(const Request& req) = 0;

    // ── Resources (MCP resources/*) ──────────────────────────────────────
    // A provider that exposes no resources returns {} (the default). MUST NOT
    // throw — map failures into the returned value / a logged warning.
    [[nodiscard]] virtual std::vector<Resource> resources() const { return {}; }
    [[nodiscard]] virtual std::vector<ResourceTemplate> resource_templates() const { return {}; }
    // Read a resource by URI. `out` receives the decoded contents; returns
    // false (and leaves a human message in `err`) on any failure.
    [[nodiscard]] virtual bool read_resource(const std::string& /*uri*/,
                                             std::vector<ResourceContents>& /*out*/,
                                             std::string& err) {
        err = "provider has no resources";
        return false;
    }

    // ── Prompts (MCP prompts/*) ──────────────────────────────────────────
    [[nodiscard]] virtual std::vector<Prompt> prompts() const { return {}; }
    // Render a prompt with the given string arguments. `out` receives the
    // message list; returns false (+ `err`) on failure.
    [[nodiscard]] virtual bool get_prompt(const std::string& /*name*/,
                                          const std::vector<std::pair<std::string, std::string>>& /*args*/,
                                          GetPromptResult& /*out*/,
                                          std::string& err) {
        err = "provider has no prompts";
        return false;
    }

    // ── Dynamic list-changed (tools/resources/prompts list_changed) ──────
    // The provider invokes this callback (from its transport reader thread)
    // whenever the server sends a *_list_changed notification, after it has
    // refreshed its own cached list(). A host wires this to invalidate any
    // tool index it built from this provider. Default: store + ignore.
    using ListChangedFn = std::function<void()>;
    virtual void set_on_list_changed(ListChangedFn fn) { on_list_changed_ = std::move(fn); }

protected:
    ListChangedFn on_list_changed_;
};

// ── content helpers (shared by every MCP-backed provider) ──────────────────
//
// Flatten an MCP CallToolResult into a cap::Result: text blocks concatenated,
// non-text blocks summarised by type, structuredContent + raw preserved.
[[nodiscard]] inline Result result_from_call(const CallToolResult& r) {
    Result out;
    out.is_error = r.isError.has_value() && *r.isError;
    out.raw = Json::array();
    for (const auto& block : r.content) {
        Json jb = to_json(block);
        out.raw.push_back(jb);
        if (jb.is_object()) {
            auto type = jb.value("type", std::string{});
            if (type == "text") {
                out.text += jb.value("text", std::string{});
                if (!out.text.empty() && out.text.back() != '\n') out.text += '\n';
                continue;
            }
            out.text += "[" + (type.empty() ? std::string{"content"} : type) + "]";
            if (auto uri = jb.value("uri", std::string{}); !uri.empty())
                out.text += " " + uri;
            out.text += '\n';
            continue;
        }
        out.text += jb.dump();
        out.text += '\n';
    }
    if (r.structuredContent.has_value()) out.structured = *r.structuredContent;
    return out;
}

// Build a one-text-block CallToolResult — what a LocalProvider returns to MCP
// callers, and what a Result lowers to when re-exposed over the wire.
[[nodiscard]] inline CallToolResult call_result_from(const Result& r) {
    CallToolResult out;
    out.content = { text(r.text) };
    if (!(r.structured.is_object() && r.structured.empty()))
        out.structuredContent = r.structured;
    if (r.is_error) out.isError = true;
    return out;
}

} // namespace mcp::cap
