// SPDX-License-Identifier: Apache-2.0
//
// provider.cpp — make_provider(): assemble every enabled built-in tool and
// every host-coupled tool whose backend is present into a single
// LocalProvider. The factory wraps each shell's handler so it (1) applies the
// per-tool output budget and (2) attaches effect + file-change meta onto the
// Result — uniformly, so individual tool modules never touch the carry layer.

#include "tool_shell.hpp"

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/cap/local.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace mcp::tools {

namespace detail {
// Defined in their own TUs.
void register_memory_tools(Shells&, const std::shared_ptr<MemoryStore>&);
void register_todo_tool(Shells&, const std::shared_ptr<TodoSink>&);
void register_skill_tool(Shells&, const std::shared_ptr<SkillResolver>&);
void register_search_docs_tool(Shells&, const std::shared_ptr<DocRetriever>&);
void register_task_tool(Shells&, const std::shared_ptr<SubagentRunner>&);
} // namespace detail

namespace {

// UTF-8-safe truncation to `budget` chars with a trailing marker.
std::string apply_budget(std::string text, int budget) {
    if (budget <= 0) return text;
    auto cap = static_cast<std::size_t>(budget);
    if (text.size() <= cap) return text;
    // walk back to a code-point boundary
    std::size_t n = cap;
    for (int i = 0; i < 4 && n > 0; ++i, --n)
        if ((static_cast<unsigned char>(text[n]) & 0xC0) != 0x80) break;
    std::string out = text.substr(0, n);
    out += "\n\n[... " + std::to_string(text.size() - n) + " chars elided ...]";
    return out;
}

} // namespace

std::shared_ptr<mcp::cap::CapabilityProvider>
make_provider(HostServices svc, ToolsetConfig cfg, std::string origin) {
    detail::Shells shells(cfg);

    // Host-coupled tools — registered only when their backend is present.
    detail::register_memory_tools(shells, svc.memory);
    detail::register_todo_tool(shells, svc.todo);
    detail::register_skill_tool(shells, svc.skills);
    detail::register_search_docs_tool(shells, svc.retriever);
    detail::register_task_tool(shells, svc.subagent);

    // Self-contained Tier-1 tools land here, gated on cfg.* toggles, once the
    // bodies are ported:
    if (cfg.filesystem)  detail::register_fs_tools(shells);
    if (cfg.shell)       detail::register_shell_tools(shells);
    if (cfg.search)      detail::register_search_tools(shells);
    if (cfg.search)      detail::register_repo_map_tool(shells);
    if (cfg.diagnostics) detail::register_diagnostics_tool(shells);
    if (cfg.git)         detail::register_git_tools(shells);
    if (cfg.web)         detail::register_web_tools(shells, svc.http);

    auto provider = std::make_shared<mcp::cap::LocalProvider>(std::move(origin));
    const int default_budget = cfg.default_output_budget;

    for (auto& s : shells.items()) {
        EffectSet fx       = s.effects;
        int       budget   = s.output_budget > 0 ? s.output_budget : default_budget;
        auto      handler  = std::move(s.handler);

        provider->add(s.tool,
            [handler = std::move(handler), fx, budget](const mcp::Json& args)
                -> mcp::cap::Result {
                mcp::cap::Result r = handler(args);
                if (!r.is_error) r.text = apply_budget(std::move(r.text), budget);
                // Attach effects + (if the tool put one in structured) the
                // file-change is already there; just stamp effects so the host
                // permission policy + scheduler can read them back.
                auto existing_change = read_change(r);
                attach_meta(r, fx, existing_change);
                return r;
            });
    }
    return provider;
}

} // namespace mcp::tools
