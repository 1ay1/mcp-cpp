// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/meta.hpp — the carry protocol for tool metadata (effects +
// file-change) that the bare MCP Result has no native field for.
//
//   A toolset tool packs its EffectSet (and, for write/edit, a FileChange)
//   into cap::Result::structured under the reserved key "_mcp_tools". The
//   producing side calls attach_meta(); the consuming host calls read_*().
//   A host that never looks just sees Result::text — nothing breaks.
//
//   Shape:
//     "_mcp_tools": {
//       "effects": 5,                       // EffectSet bits
//       "change": { "path": "...", "added": 3, "removed": 1,
//                   "before": "...", "after": "..." }   // optional
//     }

#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/tools/toolset.hpp>

#include <optional>
#include <string>

namespace mcp::tools {

inline constexpr const char* kMetaKey = "_mcp_tools";

// Attach effects (+ optional FileChange) onto a Result's structured payload.
// Preserves any structured content the tool already produced.
inline void attach_meta(mcp::cap::Result& r, EffectSet fx,
                        const std::optional<FileChange>& change = std::nullopt) {
    if (!r.structured.is_object()) r.structured = mcp::Json::object();
    mcp::Json m = mcp::Json::object();
    m["effects"] = fx.bits();
    if (change) {
        m["change"] = mcp::Json{
            {"path",    change->path},
            {"added",   change->added},
            {"removed", change->removed},
            {"before",  change->before},
            {"after",   change->after},
        };
    }
    r.structured[kMetaKey] = std::move(m);
}

// Read the effects a tool declared (default empty if none attached).
[[nodiscard]] inline EffectSet read_effects(const mcp::cap::Result& r) {
    if (!r.structured.is_object()) return {};
    auto it = r.structured.find(kMetaKey);
    if (it == r.structured.end() || !it->is_object()) return {};
    auto e = it->find("effects");
    if (e == it->end() || !e->is_number_unsigned()) return {};
    return EffectSet{static_cast<std::uint8_t>(e->get<unsigned>())};
}

// Read the FileChange a write/edit tool produced, if any.
[[nodiscard]] inline std::optional<FileChange> read_change(const mcp::cap::Result& r) {
    if (!r.structured.is_object()) return std::nullopt;
    auto it = r.structured.find(kMetaKey);
    if (it == r.structured.end() || !it->is_object()) return std::nullopt;
    auto c = it->find("change");
    if (c == it->end() || !c->is_object()) return std::nullopt;
    FileChange fc;
    fc.path    = c->value("path", std::string{});
    fc.added   = c->value("added", 0);
    fc.removed = c->value("removed", 0);
    fc.before  = c->value("before", std::string{});
    fc.after   = c->value("after", std::string{});
    return fc;
}

// Strip the carry key so it doesn't leak to a plain MCP client that would
// otherwise see "_mcp_tools" in structuredContent. Call when lowering a
// toolset Result onto the bare wire (the mcp-serve path).
inline void strip_meta(mcp::cap::Result& r) {
    if (r.structured.is_object()) r.structured.erase(kMetaKey);
}

} // namespace mcp::tools

namespace mcp::tools {

// The effect declaration for a built-in tool, looked up by name. Defined in
// the compiled toolset (effects.cpp); declared here so the host can ask "what
// effects does `bash` have" without running it.
[[nodiscard]] EffectSet effects_for_builtin(const std::string& name);

} // namespace mcp::tools
