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

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mcp::tools::detail {

using mcp::Json;

struct Shell {
    mcp::Tool                                              tool;
    std::function<mcp::cap::Result(const Json& args)>      handler;
    EffectSet                                             effects;
    int                                                   output_budget = 0;  // 0 ⇒ toolset default
};

class Shells {
public:
    explicit Shells(const ToolsetConfig& cfg) : cfg_(cfg) {}

    // Register a tool from name + description + raw JSON schema + effects.
    void add(std::string name, std::string description, Json schema,
             EffectSet effects,
             std::function<mcp::cap::Result(const Json&)> handler,
             int output_budget = 0) {
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
        s.handler           = std::move(handler);
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
void register_task_tool(Shells&, const std::shared_ptr<SubagentRunner>&);
// Tier-1 (self-contained) tool families, gated on ToolsetConfig toggles:
void register_fs_tools(Shells&);       // read, write, edit, list_dir
void register_shell_tools(Shells&);    // bash
void register_search_tools(Shells&);   // grep, glob, find_definition
void register_diagnostics_tool(Shells&);
void register_git_tools(Shells&);
void register_web_tools(Shells&, const std::shared_ptr<HttpClient>&);

} // namespace mcp::tools::detail
