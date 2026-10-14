// SPDX-License-Identifier: Apache-2.0
//
// provider.cpp — make_provider(): assemble every enabled built-in tool and
// every host-coupled tool whose backend is present into a single
// LocalProvider. The factory wraps each shell's handler so it (1) applies the
// per-tool output budget and (2) attaches effect + file-change meta onto the
// Result — uniformly, so individual tool modules never touch the carry layer.

#include <algorithm>
#include "tool_shell.hpp"

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>

#include <mcp/cap/local.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mcp::tools {

namespace detail {
// Defined in their own TUs.
void register_memory_tools(Shells&, const std::shared_ptr<MemoryStore>&);
void register_todo_tool(Shells&, const std::shared_ptr<TodoSink>&);
void register_skill_tool(Shells&, const std::shared_ptr<SkillResolver>&);
void register_search_docs_tool(Shells&, const std::shared_ptr<DocRetriever>&);
void register_search_code_tool(Shells&, const std::shared_ptr<DocRetriever>&);
void register_task_tool(Shells&, const std::shared_ptr<SubagentRunner>&);

// Every tool family, registered in one place. make_provider passes the
// host's services; effects_for_builtin passes declare-only ones. A family
// whose backend is null isn't registered: a tool the host can't serve is
// absent, not present-and-failing.
void register_all(Shells& shells, const HostServices& svc, ToolsetConfig cfg) {
    register_memory_tools(shells, svc.memory);
    register_todo_tool(shells, svc.todo);
    register_skill_tool(shells, svc.skills);
    register_search_docs_tool(shells, svc.retriever);
    register_search_code_tool(shells, svc.code_retriever);
    register_task_tool(shells, svc.subagent);

    // Everything below reads or writes files, which only the host's
    // FileSystem may do.
    if (!svc.files) cfg.filesystem = cfg.shell = cfg.search = cfg.diagnostics = cfg.git = false;

    if (cfg.filesystem) register_fs_tools(shells);
    if (cfg.shell) {
        register_shell_tools(shells, svc.exec);
        register_process_tools(shells, svc.exec);
    }
    if (cfg.search) {
        register_search_tools(shells, svc.exec);
        register_structural_tools(shells, svc.code_retriever);
        register_repo_map_tool(shells);
    }
    // Transforms have their own toggle so a minimal profile can drop them
    // without losing grep/read; each still needs its base capability.
    if (cfg.search && cfg.transforms)     register_textproc_tools(shells);
    if (cfg.filesystem && cfg.transforms) register_data_tools(shells);
    if (cfg.diagnostics) {
        register_diagnostics_tool(shells, svc.exec);
        register_test_tool(shells, svc.exec);
    }
    if (cfg.git) register_git_tools(shells, svc.exec);
    if (cfg.web) register_web_tools(shells, svc.http, cfg.jina, svc.web_search_plan);
}
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

void Call::split(std::size_t n, const std::function<void(std::size_t)>& fn) const {
    if (splitter && splitter->run && n > 1) { splitter->run(n, fn); return; }
    for (std::size_t i = 0; i < n; ++i) fn(i);
}

std::size_t Call::width() const noexcept {
    return splitter && splitter->run ? std::max<std::size_t>(1, splitter->width) : 1;
}

std::shared_ptr<mcp::cap::CapabilityProvider>
make_provider(HostServices svc, ToolsetConfig cfg, std::string origin) {
    detail::Shells shells(cfg);
    detail::register_all(shells, svc, cfg);

    auto provider = std::make_shared<mcp::cap::LocalProvider>(std::move(origin));
    const int default_budget = cfg.default_output_budget;
    // The tools' memory between calls. The host shares it however it likes;
    // without one, this provider keeps its own and expects one call at a time.
    std::shared_ptr<StateAccess> state = svc.state;
    if (!state) state = std::make_shared<SoleStateAccess>();
    auto split = std::make_shared<const Splitter>(std::move(svc.split));
    std::shared_ptr<FileSystem> files = svc.files;

    for (auto& s : shells.items()) {
        EffectSet fx       = s.effects;
        int       budget   = s.output_budget > 0 ? s.output_budget : default_budget;
        auto      handler  = std::move(s.handler);

        provider->add(s.tool,
            [handler = std::move(handler), fx, budget, state, split, files](const mcp::cap::Request& req)
                -> mcp::cap::Result {
                Call call;
                call.state     = state.get();
                call.splitter  = split.get();
                call.files     = files.get();
                call.reader    = req.reader;
                call.cancelled = req.cancelled;
                call.stop      = req.stop;
                call.progress  = req.progress;
                call.host      = req.host;
                call.scope_workspace = req.scope_workspace;
                for (const auto& r : req.scope_extra_roots) call.scope_extra_roots.emplace_back(r);
                call.scope_project = req.scope_project;
                if (!call.cancelled && req.stop.stop_possible())
                    call.cancelled = [st = req.stop] { return st.stop_requested(); };
                mcp::cap::Result r = handler(call, req.args);
                if (!r.is_error) r.text = apply_budget(std::move(r.text), budget);
                // The tool's declaration, stamped by the one place that knows
                // it. A body sets changes and images; it never sets effects.
                r.effects = fx;
                return r;
            });
    }
    return provider;
}

std::optional<EffectSet> effects_for_builtin(std::string_view name) {
    // Read off the same registrations make_provider uses, so the policy, the
    // scheduler and the tools can't disagree. The declare-only services are
    // never called; they only make every family register.
    static const auto table = [] {
        ToolsetConfig all;
        detail::Shells shells(all);
        detail::register_all(shells, detail::declare_only_services(), all);
        std::map<std::string, EffectSet, std::less<>> out;
        for (const auto& s : shells.items()) out.emplace(s.tool.name, s.effects);
        return out;
    }();
    if (auto it = table.find(name); it != table.end()) return it->second;
    return std::nullopt;
}

} // namespace mcp::tools
