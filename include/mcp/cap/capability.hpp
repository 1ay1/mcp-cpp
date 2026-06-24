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

#include <mcp/types.hpp>     // Tool, JsonSchema
#include <mcp/content.hpp>   // ContentBlock, TextContent
#include <mcp/methods.hpp>   // CallToolResult
#include <mcp/codec.hpp>     // to_json

#include <string>
#include <string_view>
#include <vector>

namespace mcp::cap {

// A request to perform one capability. `tool` is the UNQUALIFIED tool name as
// the provider advertised it (the Registry strips any namespace prefix before
// dispatch); `args` is the JSON object matching the tool's inputSchema.
struct Request {
    std::string tool;
    Json        args = Json::object();
};

// The outcome of executing a capability. `text` is the human/model-facing
// rendering (already flattened from MCP content blocks); `structured` carries
// typed output when the provider produced it (MCP structuredContent); raw
// holds the original content-block list for callers that want full fidelity.
struct Result {
    std::string text;
    Json        structured = Json::object();   // empty object == none
    Json        raw        = Json::array();    // original content blocks
    bool        is_error   = false;

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
