// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/toolset.hpp — the public surface of mcp-cpp's batteries-included
// toolset: effect tags, a file-change carry type, per-tool config, and the
// factory that produces a cap::CapabilityProvider over the whole set.
//
//   The toolset is a compiled add-on (libmcp_tools), NOT part of the
//   header-only core. A host links it only if it wants the built-in tools.
//
//   Two pieces of metadata travel beside a tool result that the bare MCP wire
//   has no field for, but that a rich host (an interactive agent UI, a
//   permission policy, a parallel scheduler) needs:
//
//     • EffectSet  — what the tool observably does (ReadFs/WriteFs/Net/Exec).
//                    Drives permission prompts + parallel-safety scheduling.
//     • FileChange — the before/after of a file a write/edit produced, for a
//                    diff-review UI.
//
//   Rather than couple the library to a host's types, the toolset carries
//   both through the capability Result's `structured` JSON under a reserved
//   key ("_mcp_tools"). A host that cares decodes it (see meta.hpp); a host
//   that doesn't ignores it and just shows the text. This keeps the library
//   self-contained while losing nothing the host needs.

#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/cap/scheduler.hpp>
#include <mcp/tools/host.hpp>

#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools {

// The effect, file-change and image types are the capability layer's
// (mcp/cap/capability.hpp): one definition, read by the permission policy,
// the scheduler and the host alike.
using mcp::cap::Effect;
using mcp::cap::EffectSet;
using mcp::cap::FileChange;

// ── Per-tool / toolset configuration ───────────────────────────────────────
struct ToolsetConfig {
    // When false, the corresponding Tier-1 tool family is not registered.
    bool filesystem   = true;   // read, write, edit, list_dir
    bool shell        = true;   // bash
    bool search       = true;   // grep, glob, find_definition
    // The transform / aggregate / structured-data family: extract, aggregate,
    // replace, read_filter, json_query, outline. Split from `search` so a
    // minimal or latency-sensitive profile can shed ~5 KB of schema (these
    // 7 tools) without losing grep/read. Registered only when BOTH this and
    // the family's base capability (search or filesystem) are on.
    bool transforms   = true;
    bool diagnostics  = true;   // diagnostics
    bool git          = true;   // git_status/diff/log/commit
    bool web          = true;   // web_fetch, web_search
    // Let web_fetch re-render a JS/SPA shell through r.jina.ai. A request can
    // still opt out with an `x-no-jina` header.
    bool jina         = true;

    // Per-tool output budget defaults (chars). 0 ⇒ no cap.
    int  default_output_budget = 60'000;

    // Steering text is a host's call, not the library's: how strongly to
    // push a model from `shell` toward `read`, which tool to prefer for
    // what. The built-in descriptions say what a tool does. A host that
    // wants to say more appends it here, keyed by tool name; the text is
    // added after the built-in description.
    std::map<std::string, std::string, std::less<>> description_notes;
};

// ── The provider factory ───────────────────────────────────────────────────
// Build a CapabilityProvider exposing every enabled built-in tool plus every
// host-coupled tool whose backend is present in `svc`. Tools are advertised
// through CapabilityProvider::list() and run through ::execute(); each
// Result carries its effects, file changes and images as typed fields.
//
// origin defaults to "local" so the Registry namespaces these as the host's
// own tools, indistinguishable on the wire from any other provider.
[[nodiscard]] std::shared_ptr<mcp::cap::CapabilityProvider>
make_provider(HostServices svc,
              ToolsetConfig cfg = {},
              std::string   origin = "local");

// ── Scheduler glue ──────────────────────────────────────────────────────────────────────
// The effect declaration for a built-in tool, by name (effects.cpp), or
// nullopt for a name the toolset doesn't have.
[[nodiscard]] std::optional<EffectSet> effects_for_builtin(std::string_view name);

// A cap::EffectFn keyed off the RICH built-in effect table (effects_for_builtin)
// + the built-in path-extraction, so the effect-aware parallel scheduler
// (mcp/cap/scheduler.hpp) reasons about the batteries-included tools precisely
// instead of falling back to the coarse standard-annotation reader. A host with
// this toolset wires the scheduler in one line:
//
//     auto fn = mcp::tools::make_effect_fn();
//     auto results = mcp::cap::run(registry, batch, fn);
//
// Tools NOT in the built-in table (third-party MCP servers in the same
// registry) get a conservative writer classification so they always serialise
// — the planner never parallelises a tool it can't reason about.
[[nodiscard]] inline mcp::cap::EffectFn make_effect_fn() {
    return [](const mcp::cap::Request& r) -> mcp::cap::CallFacts {
        mcp::cap::CallFacts f;
        // The registry may namespace the bare name ("local__read"); the effect
        // table is keyed on the bare name, so strip any "<origin>__" prefix.
        std::string_view name = r.tool;
        if (auto pos = name.rfind("__"); pos != std::string_view::npos)
            name = name.substr(pos + 2);
        const auto fx = effects_for_builtin(name);
        if (!fx) {
            // Not a built-in (a third-party server in the same registry):
            // classify as a writer so the planner always serialises it.
            f.effects = EffectSet{Effect::WriteFs};
            return f;
        }
        f.effects = *fx;
        f.paths   = mcp::cap::extract_paths(std::string{name}, r.args);
        return f;
    };
}

} // namespace mcp::tools
