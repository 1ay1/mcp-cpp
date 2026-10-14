// SPDX-License-Identifier: Apache-2.0
//
// tool_body.hpp — INTERNAL helpers shared by the Tier-1 tool modules. Each
// tool is written in agentty's faithful "parse(json) -> ExecResult" shape
// (util::ExecResult = expected<ToolOutput, ToolError>); this bridges that to
// the cap::Result the Shells handler must return, carrying any FileChange and
// surfacing ToolError as an is_error result.

#pragma once

#include "call_state.hpp"

#include <mcp/cap/capability.hpp>
#include <mcp/tools/state.hpp>
#include <mcp/tools/util/arg_spec.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/utf8.hpp>

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using mcp::Json;

// Lower a tool body's ExecResult into a cap::Result: text, plus the files
// it wrote and images, on the Result's typed fields. On error returns an
// is_error Result whose text is the rendered ToolError.
inline mcp::cap::Result lower(util::ExecResult r) {
    // The one exit for every tool body. A file, a command's output or an
    // error can carry bytes that aren't UTF-8; they are made valid here, so
    // no tool can hand the host a result that fails to encode.
    if (!r) {
        return mcp::cap::Result::error(util::to_valid_utf8(r.error().render()));
    }
    mcp::cap::Result out = mcp::cap::Result::ok(util::to_valid_utf8(std::move(r->text)));
    out.changes.reserve(r->changes.size());
    for (auto& c : r->changes) {
        c.path   = util::to_valid_utf8(std::move(c.path));
        c.before = util::to_valid_utf8(std::move(c.before));
        c.after  = util::to_valid_utf8(std::move(c.after));
        out.changes.push_back(std::move(c));
    }
    out.images = std::move(r->images);
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
        auto parsed = parse(j, detail::bounds(call));
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

// A tool whose arguments are declared by an ArgSpec: the schema and the
// decoder come from the same declaration. `spec` must outlive the provider
// (declare it as a static const). A `check` that needs the call's Bounds
// (resolving paths) runs after decoding.
// The call's bounds, or empty ones for a host without a filesystem (a
// web-only provider): computing them reads the disk.
[[nodiscard]] inline util::Bounds bounds_if_fs(const Call& call) {
    return call.files ? detail::bounds(call) : util::Bounds{};
}

// A tool whose runner builds its own cap::Result (the host-coupled tools:
// todo, skill, task...). The spec is shared, so it can be built at
// registration time (a memory store's scopes, a runner's agent types).
template <class A>
[[nodiscard]] std::shared_ptr<const util::ArgSpec<A>> share(util::ArgSpec<A> s) {
    return std::make_shared<const util::ArgSpec<A>>(std::move(s));
}
template <class Args, class Run>
[[nodiscard]] auto spec_body(std::shared_ptr<const util::ArgSpec<Args>> spec, Run run) {
    return [spec = std::move(spec), run = std::move(run)](const Call& call, const Json& j) mutable
               -> mcp::cap::Result {
        auto parsed = spec->decode(j);
        // Host tools word their errors as plain text ("skill: `name` is required.").
        if (!parsed) return mcp::cap::Result::error(parsed.error().detail);
        if constexpr (std::invocable<Run&, const Call&, const Args&>) return run(call, *parsed);
        else return run(*parsed);
    };
}

template <class Args, class Run>
[[nodiscard]] auto body(Run run, const util::ArgSpec<Args>& spec) {
    return [run = std::move(run), &spec](const Call& call, const Json& j) mutable -> mcp::cap::Result {
        std::optional<util::Bounds> b;
        if (spec.has_tidy() && call.files) b = detail::bounds(call);
        auto parsed = spec.decode(j, b ? &*b : nullptr);
        if (!parsed) return mcp::cap::Result::error(parsed.error().render());
        return lower(run_on<Args>(run, call, *parsed));
    };
}
template <class Args>
using CheckIn = std::expected<Args, util::ToolError> (*)(Args, const util::Bounds&);
template <class Args, class Run>
[[nodiscard]] auto body(Run run, const util::ArgSpec<Args>& spec, CheckIn<Args> check) {
    return [run = std::move(run), &spec, check](const Call& call, const Json& j) mutable
               -> mcp::cap::Result {
        const auto b = bounds_if_fs(call);
        auto parsed = spec.decode(j, call.files ? &b : nullptr);
        if (parsed) parsed = check(std::move(*parsed), b);
        if (!parsed) return mcp::cap::Result::error(parsed.error().render());
        return lower(run_on<Args>(run, call, *parsed));
    };
}

// ── Line-count diff (added/removed) ─────────────────────────────────────────
// Uses the internal Myers diff engine (diff.hpp) for added/removed totals that
// match a real unified diff exactly.
[[nodiscard]] FileChange make_change(std::string path,
                                     std::string before,
                                     std::string after);

} // namespace mcp::tools::detail
