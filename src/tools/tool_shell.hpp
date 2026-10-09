// SPDX-License-Identifier: Apache-2.0
//
// tool_shell.hpp — INTERNAL. The accumulator each tool module registers into.
// A "shell" is one built-in tool: its mcp::Tool spec, its handler, its effect
// tags, and its output budget. make_provider() drains a Shells into a
// LocalProvider, wrapping every handler so it attaches effect/file-change meta
// and applies the output budget uniformly.

#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/state.hpp>

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mcp::tools::detail {

// What a tool gets back from the host's Exec, in the shape its formatting
// code wants.
//
// This used to be util::SubprocessResult, borrowed from the runner this
// library no longer has. Keeping the borrow would have kept the header, and
// with it the implication that a tool might still spawn something itself.
struct RunResult {
    std::string output;
    int         exit_code     = 0;
    bool        started       = true;
    std::string start_error;
    bool        truncated     = false;
    bool        timed_out     = false;
    bool        hit_wall      = false;   // WHICH clock, when timed_out
    bool        stopped_early = false;   // the caller had enough
};

/// Render a run for a model to read. The timeout line names the clock,
/// because "timed out" alone sends the reader hunting for a hang when the
/// command was fine and simply needed longer.
[[nodiscard]] inline std::string format_run(const RunResult& r,
                                            std::chrono::seconds idle) {
    if (!r.started) return "[" + r.start_error + "]";
    std::string o = r.output;
    if (r.truncated) o += "\n[output truncated]";
    if (r.timed_out) {
        o += r.hit_wall
            ? "\n[stopped at the wall-clock ceiling while still producing "
              "output \xe2\x80\x94 it was not stuck; re-run with a larger "
              "`timeout`, or start it with `process_start`]"
            : "\n[no output for " + std::to_string(idle.count())
              + "s, so it was stopped \xe2\x80\x94 the clock measures "
                "SILENCE, not total runtime]";
    } else if (r.exit_code != 0) {
        o += "\n[exit code " + std::to_string(r.exit_code) + "]";
    }
    return o;
}


using mcp::Json;

// A tool body gets the call (state, reader, cancel) and its arguments.
using Handler = std::function<mcp::cap::Result(const Call& call, const Json& args)>;

struct Shell {
    mcp::Tool                                              tool;
    Handler                                                handler;
    EffectSet                                             effects;
    int                                                   output_budget = 0;  // 0 ⇒ toolset default
};

class Shells {
public:
    explicit Shells(const ToolsetConfig& cfg) : cfg_(cfg) {}

    // Register a tool from name + description + raw JSON schema + effects.
    // `handler` takes (const Call&, const Json&), or just (const Json&) for
    // a tool that needs nothing but its arguments.
    template <class F>
        requires std::invocable<F&, const Call&, const Json&> || std::invocable<F&, const Json&>
    void add(std::string name, std::string description, Json schema,
             EffectSet effects, F handler, int output_budget = 0) {
        Shell s;
        s.tool.name = name;
        if (!description.empty()) s.tool.description = description;
        try { s.tool.inputSchema = mcp::from_json<mcp::JsonSchema>(schema); }
        catch (...) {}
        // Annotations mirror the effect tags so a plain MCP client gets the
        // read-only / destructive hints even though it can't read our meta.
        mcp::ToolAnnotations ann;
        const bool mutates = effects.has(Effect::WriteFs) || effects.has(Effect::Exec);
        ann.readOnlyHint    = !mutates;
        ann.destructiveHint = mutates;
        ann.openWorldHint   = effects.has(Effect::Net);
        s.tool.annotations  = ann;
        s.effects           = effects;
        s.output_budget     = output_budget;
        if constexpr (std::invocable<F&, const Call&, const Json&>)
            s.handler = Handler{std::move(handler)};
        else
            s.handler = Handler{[h = std::move(handler)](const Call&, const Json& j) mutable { return h(j); }};
        shells_.push_back(std::move(s));
    }

    [[nodiscard]] const ToolsetConfig& cfg() const { return cfg_; }
    [[nodiscard]] std::vector<Shell>& items() { return shells_; }

private:
    ToolsetConfig      cfg_;
    std::vector<Shell> shells_;
};

// Each tool module exposes one register_* entry the provider factory calls.
void register_memory_tools(Shells&, const std::shared_ptr<MemoryStore>&);
void register_todo_tool(Shells&, const std::shared_ptr<TodoSink>&);
void register_skill_tool(Shells&, const std::shared_ptr<SkillResolver>&);
void register_search_docs_tool(Shells&, const std::shared_ptr<DocRetriever>&);
void register_search_code_tool(Shells&, const std::shared_ptr<DocRetriever>&);
void register_task_tool(Shells&, const std::shared_ptr<SubagentRunner>&);
// Tier-1 (self-contained) tool families, gated on ToolsetConfig toggles:
void register_fs_tools(Shells&);       // read, write, edit, list_dir
void register_shell_tools(Shells&, const std::shared_ptr<Exec>&);  // shell
void register_process_tools(Shells&, const std::shared_ptr<Exec>&);  // process_start/poll/stop
void register_search_tools(Shells&, const std::shared_ptr<Exec>&);   // grep, glob, find_definition
void register_structural_tools(Shells&,
    const std::shared_ptr<DocRetriever>& = nullptr); // search_structural
    // (AST-shape search; optional retriever adds verified semantic leads on
    // zero hits and semantic file ordering when results exceed the budget)
void register_repo_map_tool(Shells&);  // repo_map (PageRank codebase skeleton)
void register_textproc_tools(Shells&); // extract / aggregate / replace / read_filter
void register_data_tools(Shells&);     // json_query (jq-lite for structured data)
void register_diagnostics_tool(Shells&, const std::shared_ptr<Exec>&);
void register_test_tool(Shells&, const std::shared_ptr<Exec>&);
void register_git_tools(Shells&, const std::shared_ptr<Exec>&);
void register_web_tools(Shells&, const std::shared_ptr<HttpClient>&, bool jina);

} // namespace mcp::tools::detail
