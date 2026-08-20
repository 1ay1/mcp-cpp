// SPDX-License-Identifier: Apache-2.0
//
// tool_body.hpp — INTERNAL helpers shared by the Tier-1 tool modules. Each
// tool is written in agentty's faithful "parse(json) -> ExecResult" shape
// (util::ExecResult = expected<ToolOutput, ToolError>); this bridges that to
// the cap::Result the Shells handler must return, carrying any FileChange and
// surfacing ToolError as an is_error result.

#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/error.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using mcp::Json;

// Lower a tool body's ExecResult into a cap::Result. On success copies text
// and, when the body produced a FileChange, stashes it under the meta key so
// the provider's attach_meta carries it to the host's diff-review UI. On
// error returns an is_error Result whose text is the rendered ToolError.
inline mcp::cap::Result lower(util::ExecResult r) {
    if (!r) {
        return mcp::cap::Result::error(r.error().render());
    }
    mcp::cap::Result out = mcp::cap::Result::ok(std::move(r->text));
    if (r->change || !r->changes.empty()) {
        out.structured = Json::object();
        Json m = Json::object();
        auto encode = [](const mcp::tools::FileChange& c) {
            return Json{
                {"path",    c.path},
                {"added",   c.added},
                {"removed", c.removed},
                {"before",  c.before},
                {"after",   c.after},
            };
        };
        if (r->change) m["change"] = encode(*r->change);
        if (!r->changes.empty()) {
            Json arr = Json::array();
            for (const auto& c : r->changes) arr.push_back(encode(c));
            m["changes"] = std::move(arr);
        }
        out.structured[kMetaKey] = std::move(m);
    }
    return out;
}

// Wrap a (parse, run) pair into a Shells handler. Mirrors util::adapt but
// targets cap::Result via lower().
template <class Args>
inline std::function<mcp::cap::Result(const Json&)>
body(util::ExecResult (*run)(const Args&),
     std::expected<Args, util::ToolError> (*parse)(const Json&)) {
    return [run, parse](const Json& j) -> mcp::cap::Result {
        auto parsed = parse(j);
        if (!parsed) return mcp::cap::Result::error(parsed.error().render());
        return lower(run(*parsed));
    };
}

// ── Line-count diff (added/removed) ─────────────────────────────────────────
// Uses the internal Myers diff engine (diff.hpp) for added/removed totals that
// match a real unified diff exactly.
[[nodiscard]] FileChange make_change(std::string path,
                                     std::string before,
                                     std::string after);

} // namespace mcp::tools::detail
