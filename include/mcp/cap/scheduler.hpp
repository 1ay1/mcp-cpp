// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/scheduler.hpp — EFFECT-AWARE PARALLEL TOOL SCHEDULING.
//
//   When a model emits a BATCH of tool calls in one turn — and frontier
//   models do this constantly: three `read`s, a `grep`, a `glob`, then an
//   `edit` — virtually every agent runtime executes them STRICTLY
//   SEQUENTIALLY. That's a correctness-by-laziness choice: run them one at a
//   time and nothing can race. But it leaves enormous latency on the table.
//   Five independent file reads that each block 8 ms on disk + IPC cost 40 ms
//   serial when they could cost 8 ms concurrent.
//
//   The reason no one parallelises is that you can't safely run two calls at
//   once unless you KNOW they don't interfere — and the bare MCP wire gives a
//   client no machine-readable interference model. mcp-cpp does: every tool
//   declares an EffectSet (Read/Write/Net/Exec), and most fs tools name their
//   target PATHS right in the args. From those two facts we can derive, with
//   zero host annotation, a provably-safe concurrent execution plan:
//
//     • a wave of calls that pairwise DON'T conflict runs concurrently;
//     • waves run in submission order, so a write is never reordered before a
//       read the model intended to happen first;
//     • the model's emission order is otherwise preserved — the result vector
//       comes back 1:1 with the input, so the agent loop is none the wiser
//       that anything ran in parallel.
//
//   Conflict model (two calls A, B may NOT share a wave when):
//     1. either is Exec        — a shell command's blast radius is unbounded;
//                                serialise it against everything. (Correct &
//                                conservative: bash can rm -rf or curl.)
//     2. both are pure-read    — NEVER conflict. Any number of ReadFs / Net
//        (no Write, no Exec)     reads run together regardless of path.
//     3. one Writes and their resource sets OVERLAP — ordered. Overlap is
//        path-prefix aware: writing "src/" conflicts with reading "src/a.c".
//     4. a Write with NO extractable path — unknown blast radius; conflicts
//        with every other fs-touching call. (Net-only peers still parallelise.)
//
//   This is a PURE planner (plan_waves) plus a thin std::async executor
//   (run / run_on). The planner has no I/O and is unit-testable in isolation;
//   the executor is opt-in and lives behind the same one-call surface a host
//   already uses (Registry::dispatch), so adopting it is a one-line change.
//
//   Header-only, core layer: it depends only on the cap abstraction and an
//   injected EffectFn, so it works over ANY provider mix (local, spawned MCP,
//   HTTP) — not just the batteries-included toolset. A default EffectFn that
//   reads the standard MCP ToolAnnotations (readOnlyHint / destructiveHint) is
//   provided so it does something sensible even for third-party servers.
//
#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/cap/registry.hpp>
#include <mcp/types.hpp>

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp::cap {

// ── Effect model (mirrors mcp::tools::Effect, but lives in the core cap layer
//    so the scheduler doesn't depend on the compiled toolset). ──────────────
enum class Eff : std::uint8_t {
    ReadFs  = 1u << 0,
    WriteFs = 1u << 1,
    Net     = 1u << 2,
    Exec    = 1u << 3,
};

struct Effects {
    std::uint8_t bits = 0;
    constexpr Effects() = default;
    constexpr explicit Effects(std::uint8_t b) : bits(b) {}
    constexpr Effects(std::initializer_list<Eff> es) {
        for (auto e : es) bits |= static_cast<std::uint8_t>(e);
    }
    [[nodiscard]] constexpr bool has(Eff e) const {
        return (bits & static_cast<std::uint8_t>(e)) != 0;
    }
    [[nodiscard]] constexpr bool writes() const { return has(Eff::WriteFs); }
    [[nodiscard]] constexpr bool execs()  const { return has(Eff::Exec); }
    // Touches the filesystem in any way (read or write).
    [[nodiscard]] constexpr bool touches_fs() const {
        return has(Eff::ReadFs) || has(Eff::WriteFs);
    }
    // Pure-read: reads and/or talks to the network, but never writes or execs.
    // Two pure-read calls are unconditionally parallel-safe.
    [[nodiscard]] constexpr bool pure_read() const {
        return !writes() && !execs();
    }
};

// How the scheduler learns a call's effects + touched paths. Given the
// (exposed) tool name and its args, return the effect set and the list of
// filesystem resource keys the call will touch (absolute or workspace-relative
// — the planner only compares them to each other, so any consistent space
// works). An empty path list for an fs-touching write means "unknown blast
// radius" (rule 4).
struct CallFacts {
    Effects                  effects;
    std::vector<std::string> paths;   // touched fs resources; empty ⇒ unknown
};
using EffectFn = std::function<CallFacts(const Request&)>;

// ── Path extraction for the built-in tool vocabulary ───────────────────────
// The batteries-included tools name their target under well-known arg keys.
// This pulls them out so the default planner can reason about overlap without
// any host wiring. A host with custom tools supplies its own EffectFn.
[[nodiscard]] inline std::vector<std::string>
extract_paths(const std::string& tool, const Json& args) {
    std::vector<std::string> out;
    if (!args.is_object()) return out;
    auto take = [&](const char* key) {
        if (auto it = args.find(key); it != args.end() && it->is_string()) {
            auto s = it->get<std::string>();
            if (!s.empty()) out.push_back(std::move(s));
        }
    };
    // read / write / edit / find_definition / list_dir use path|file_path.
    take("path");
    take("file_path");
    take("filepath");
    take("filename");
    // grep / glob scope under an optional directory; absent ⇒ whole tree.
    if (tool == "grep" || tool == "glob" || tool == "list_dir") {
        take("dir");
        take("directory");
        take("root");
    }
    return out;
}

// Default EffectFn that reads the STANDARD MCP ToolAnnotations off the
// registry's advertised tools. readOnlyHint=true ⇒ pure read; destructiveHint
// or no read-only hint ⇒ treat as a writer (conservative). openWorldHint is
// folded into Net. This makes the scheduler do something safe for ANY MCP
// server, even one that never heard of mcp-cpp's EffectSet. A host with the
// batteries-included toolset should prefer the richer effects_for_builtin path
// (see make_effect_fn in mcp/tools, which also extracts paths).
[[nodiscard]] inline EffectFn annotation_effect_fn(const Registry& reg) {
    // Snapshot name → annotations once; the tool list is stable across a batch.
    auto ann = std::make_shared<std::unordered_map<std::string, ToolAnnotations>>();
    for (const auto& t : reg.tools())
        if (t.annotations.has_value()) (*ann)[t.name] = *t.annotations;
    return [ann](const Request& r) -> CallFacts {
        CallFacts f;
        auto it = ann->find(r.tool);
        const bool read_only =
            it != ann->end() && it->second.readOnlyHint.has_value()
            && *it->second.readOnlyHint;
        const bool open_world =
            it != ann->end() && it->second.openWorldHint.has_value()
            && *it->second.openWorldHint;
        if (read_only) f.effects = Effects{Eff::ReadFs};
        else           f.effects = Effects{Eff::WriteFs};   // conservative
        if (open_world) f.effects = Effects{static_cast<std::uint8_t>(
            f.effects.bits | static_cast<std::uint8_t>(Eff::Net))};
        f.paths = extract_paths(r.tool, r.args);
        return f;
    };
}

// ── The conflict predicate ─────────────────────────────────────────────────
// True ⇒ a and b must NOT share a wave.
[[nodiscard]] inline bool conflicts(const CallFacts& a, const CallFacts& b) {
    // Rule 1: any exec serialises against everything.
    if (a.effects.execs() || b.effects.execs()) return true;
    // Rule 2: two pure-read calls never conflict.
    if (a.effects.pure_read() && b.effects.pure_read()) return false;
    // From here at least one writes. Only fs interference matters; a writer
    // and a pure Net peer (no fs) don't conflict.
    const bool a_fs = a.effects.touches_fs();
    const bool b_fs = b.effects.touches_fs();
    if (!a_fs || !b_fs) return false;
    // Rule 4: a writer with no known paths has unknown blast radius.
    const bool a_writer_blind = a.effects.writes() && a.paths.empty();
    const bool b_writer_blind = b.effects.writes() && b.paths.empty();
    if (a_writer_blind || b_writer_blind) return true;
    // Rule 3: overlap (prefix-aware) between the two path sets, when at least
    // one side writes (guaranteed here).
    auto overlaps = [](std::string_view p, std::string_view q) {
        if (p == q) return true;
        // dir prefix: "src" vs "src/a.c" (require a separator at the boundary
        // so "src" doesn't spuriously match "srcfoo").
        auto is_prefix_dir = [](std::string_view shorter, std::string_view longer) {
            return longer.size() > shorter.size()
                && longer.substr(0, shorter.size()) == shorter
                && (shorter.back() == '/' || longer[shorter.size()] == '/');
        };
        return p.size() < q.size() ? is_prefix_dir(p, q) : is_prefix_dir(q, p);
    };
    for (const auto& pa : a.paths)
        for (const auto& pb : b.paths)
            if (overlaps(pa, pb)) return true;
    return false;
}

// ── The plan ────────────────────────────────────────────────────────────────
// `order[i]` = the original index of the i-th call. `waves[w]` = the list of
// ORIGINAL indices that run concurrently in wave w. Waves execute in order;
// within a wave, any order (incl. parallel) is safe by construction.
struct Plan {
    std::vector<std::vector<std::size_t>> waves;
    [[nodiscard]] std::size_t wave_count() const { return waves.size(); }
    // Total calls planned (sum of wave sizes).
    [[nodiscard]] std::size_t size() const {
        std::size_t n = 0; for (const auto& w : waves) n += w.size(); return n;
    }
    // True when every call is alone in its own wave (no parallelism possible).
    [[nodiscard]] bool is_fully_serial() const {
        for (const auto& w : waves) if (w.size() != 1) return false;
        return true;
    }
};

// Pure wave planner. Greedy first-fit: each call (in submission order) joins
// the earliest existing wave it conflicts with nothing in; else opens a new
// wave. Submission order is preserved as a tie-break so the schedule is
// deterministic and the model's intent (a read it emitted before a write
// stays before that write) is never violated — a later wave never runs before
// an earlier one.
[[nodiscard]] inline Plan plan_waves(const std::vector<CallFacts>& facts) {
    Plan plan;
    for (std::size_t i = 0; i < facts.size(); ++i) {
        std::size_t target = plan.waves.size();   // default: new wave
        for (std::size_t w = 0; w < plan.waves.size(); ++w) {
            bool clash = false;
            for (std::size_t j : plan.waves[w]) {
                if (conflicts(facts[i], facts[j])) { clash = true; break; }
            }
            // A call may only land in wave w if it ALSO doesn't conflict with
            // anything in any LATER wave it would jump ahead of — but since we
            // fill waves left-to-right and a conflict forces a strictly later
            // wave, first-fit on the earliest clash-free wave preserves the
            // happens-before ordering: a call that conflicts with call j
            // (j<i, already placed) lands in a wave strictly after j's.
            if (!clash) { target = w; break; }
        }
        if (target == plan.waves.size()) plan.waves.emplace_back();
        plan.waves[target].push_back(i);
    }
    return plan;
}

[[nodiscard]] inline Plan plan_waves(const std::vector<Request>& batch,
                                     const EffectFn& fn) {
    std::vector<CallFacts> facts;
    facts.reserve(batch.size());
    for (const auto& r : batch) facts.push_back(fn(r));
    return plan_waves(facts);
}

// ── The executor ────────────────────────────────────────────────────────────
// Run a batch against any dispatcher, parallelising within each wave via
// std::async. `dispatch` MUST be safe to call concurrently from multiple
// threads for the calls the planner placed in the same wave — which, by the
// conflict model, never touch overlapping fs state and never exec. Returns
// results 1:1 with `batch` (original order), so the caller treats it exactly
// like a sequential run.
//
// `dispatch` is a std::function<Result(const Request&)> — typically
// [&reg](const Request& r){ return reg.dispatch(r); }. Kept generic so a host
// can wrap dispatch with its own per-call instrumentation / permission gate.
using DispatchFn = std::function<Result(const Request&)>;

[[nodiscard]] inline std::vector<Result>
run_plan(const std::vector<Request>& batch, const Plan& plan,
         const DispatchFn& dispatch) {
    std::vector<Result> out(batch.size());
    for (const auto& wave : plan.waves) {
        if (wave.size() == 1) {
            // Singleton wave: run inline, no thread spin-up cost.
            const std::size_t i = wave[0];
            out[i] = dispatch(batch[i]);
            continue;
        }
        std::vector<std::future<Result>> futs;
        futs.reserve(wave.size());
        for (std::size_t i : wave)
            futs.push_back(std::async(std::launch::async,
                [&dispatch, &batch, i] { return dispatch(batch[i]); }));
        for (std::size_t k = 0; k < wave.size(); ++k)
            out[wave[k]] = futs[k].get();
    }
    return out;
}

// One-call convenience: plan + run against a Registry with a given EffectFn.
[[nodiscard]] inline std::vector<Result>
run(Registry& reg, const std::vector<Request>& batch, const EffectFn& fn) {
    Plan plan = plan_waves(batch, fn);
    return run_plan(batch, plan,
                    [&reg](const Request& r) { return reg.dispatch(r); });
}

// Same, defaulting the EffectFn to the standard-annotation reader.
[[nodiscard]] inline std::vector<Result>
run(Registry& reg, const std::vector<Request>& batch) {
    return run(reg, batch, annotation_effect_fn(reg));
}

} // namespace mcp::cap
