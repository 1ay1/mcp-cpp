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
#include <mcp/tools/state.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <concepts>
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
    if (r->change || !r->changes.empty() || !r->images.empty()) {
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
        // Images ride the SAME structured-meta channel that carries
        // FileChanges to the host (attach_meta -> the host bridge). Base64 here
        // because structured meta is JSON; the host decodes back to raw bytes
        // and maps onto its ImageContent. Kept out of `text` so a non-vision
        // model just sees the [image ...] note, never a megabyte of base64.
        if (!r->images.empty()) {
            Json arr = Json::array();
            for (const auto& img : r->images)
                arr.push_back(Json{
                    {"media_type", img.media_type},
                    {"data",       mcp::tools::util::b64_encode(img.bytes)},
                });
            m["images"] = std::move(arr);
        }
        out.structured[kMetaKey] = std::move(m);
    }
    return out;
}

// Wrap a (parse, run) pair into a Shells handler. A runner takes the parsed
// arguments, and the Call first when it needs one (state, reader, cancel):
//     ExecResult run_x(const XArgs&)
//     ExecResult run_x(const Call&, const XArgs&)
template <class Args, class Run>
[[nodiscard]] auto run_on(Run& run, const Call& call, const Args& a) {
    if constexpr (std::invocable<Run&, const Call&, const Args&>) return run(call, a);
    else return run(a);
}

// A parser takes the JSON, and the call's Bounds when it resolves paths:
//     expected<XArgs, ToolError> parse_x(const Json&)
//     expected<XArgs, ToolError> parse_x(const Json&, const util::Bounds&)
template <class Args>
using Parse = std::expected<Args, util::ToolError> (*)(const Json&);
template <class Args>
using ParseIn = std::expected<Args, util::ToolError> (*)(const Json&, const util::Bounds&);

template <class Args, class Run>
[[nodiscard]] auto body(Run run, Parse<Args> parse) {
    return [run = std::move(run), parse](const Call& call, const Json& j) mutable -> mcp::cap::Result {
        auto parsed = parse(j);
        if (!parsed) return mcp::cap::Result::error(parsed.error().render());
        return lower(run_on<Args>(run, call, *parsed));
    };
}

template <class Args, class Run>
[[nodiscard]] auto body(Run run, ParseIn<Args> parse) {
    return [run = std::move(run), parse](const Call& call, const Json& j) mutable -> mcp::cap::Result {
        auto parsed = parse(j, call.with([](ToolState& s) {
            return util::bounds_from(s.workspace_root, s.read_roots);
        }));
        if (!parsed) return mcp::cap::Result::error(parsed.error().render());
        return lower(run_on<Args>(run, call, *parsed));
    };
}

// The same, named for a runner that captures a host capability. That
// difference (the capability travels in the closure, not a global) is the
// point of the HostServices seam.
template <class Args, class Run, class P>
[[nodiscard]] auto body_with(Run run, P parse) {
    return body<Args>(std::move(run), parse);
}

// ── Line-count diff (added/removed) ─────────────────────────────────────────
// Uses the internal Myers diff engine (diff.hpp) for added/removed totals that
// match a real unified diff exactly.
[[nodiscard]] FileChange make_change(std::string path,
                                     std::string before,
                                     std::string after);

} // namespace mcp::tools::detail
