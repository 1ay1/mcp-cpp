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
#include <memory>
#include <string>
#include <vector>

namespace mcp::tools {

// ── Effect tags ────────────────────────────────────────────────────────────
// Bit-packed observable impact. Mirrors agentty's tools::EffectSet so a host
// can map 1:1, but defined here so the library stands alone.
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
    constexpr explicit EffectSet(std::uint8_t bits) : bits_(bits) {}
    [[nodiscard]] constexpr bool has(Effect e) const {
        return (bits_ & static_cast<std::uint8_t>(e)) != 0;
    }
    [[nodiscard]] constexpr std::uint8_t bits() const { return bits_; }
private:
    std::uint8_t bits_ = 0;
};

// ── FileChange carry ───────────────────────────────────────────────────────
// A library-native description of a file mutation, enough for a host to build
// a diff-review UI. The host recomputes its own structured diff from
// before/after if it wants hunks; the line counts are provided for a strip.
struct FileChange {
    std::string path;
    int         added   = 0;
    int         removed = 0;
    std::string before;     // original contents ("" for a new file)
    std::string after;      // new contents
};

// ── Per-tool / toolset configuration ───────────────────────────────────────
struct ToolsetConfig {
    // When false, the corresponding Tier-1 tool family is not registered.
    bool filesystem   = true;   // read, write, edit, list_dir
    bool shell        = true;   // bash
    bool search       = true;   // grep, glob, find_definition
    bool diagnostics  = true;   // diagnostics
    bool git          = true;   // git_status/diff/log/commit
    bool web          = true;   // web_fetch, web_search

    // Per-tool output budget defaults (chars). 0 ⇒ no cap. The shell applies
    // these the same way agentty's dispatcher did.
    int  default_output_budget = 60'000;
};

// ── The provider factory ───────────────────────────────────────────────────
// Build a CapabilityProvider exposing every enabled built-in tool plus every
// host-coupled tool whose backend is present in `svc`. Tools are advertised
// through CapabilityProvider::list() and run through ::execute(); FileChange +
// effects ride in the Result's structured payload (see meta.hpp helpers).
//
// origin defaults to "local" so the Registry namespaces these as the host's
// own tools, indistinguishable on the wire from any other provider.
[[nodiscard]] std::shared_ptr<mcp::cap::CapabilityProvider>
make_provider(HostServices svc,
              ToolsetConfig cfg = {},
              std::string   origin = "local");

// ── Scheduler glue ──────────────────────────────────────────────────────────
// The effect declaration for a built-in tool, by name. DEFINED in the compiled
// toolset (effects.cpp); also declared in meta.hpp. Forward-declared here
// (rather than #include <mcp/tools/meta.hpp>) to avoid a circular include:
// meta.hpp needs EffectSet/FileChange from THIS header.
[[nodiscard]] EffectSet effects_for_builtin(const std::string& name);

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
    // The host-coupled shells are pure from the scheduler's standpoint (their
    // side effects are the host's responsibility) and carry an EffectSet{} in
    // the table — indistinguishable by bits from an UNKNOWN tool. Enumerate
    // the known-pure names so we don't mis-serialise them, and treat any other
    // zero-effect lookup as an unknown third-party tool to serialise.
    static const std::vector<std::string> kKnownPure = {
        "remember", "forget", "wipe_memory", "todo", "skill"};
    return [](const mcp::cap::Request& r) -> mcp::cap::CallFacts {
        mcp::cap::CallFacts f;
        // The registry may namespace the bare name ("local__read"); the effect
        // table is keyed on the bare name, so strip any "<origin>__" prefix.
        std::string name = r.tool;
        if (auto pos = name.rfind("__"); pos != std::string::npos)
            name = name.substr(pos + 2);
        EffectSet fx = effects_for_builtin(name);
        const bool known_pure =
            std::find(kKnownPure.begin(), kKnownPure.end(), name) != kKnownPure.end();
        if (fx.bits() == 0 && !known_pure) {
            // Unknown tool (not in the built-in table, not a known-pure shell):
            // conservative writer so the planner always serialises it.
            f.effects = mcp::cap::Effects{mcp::cap::Eff::WriteFs};
            return f;
        }
        f.effects = mcp::cap::Effects{fx.bits()};   // bit layout is identical
        f.paths   = mcp::cap::extract_paths(name, r.args);
        return f;
    };
}

} // namespace mcp::tools
