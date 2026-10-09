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
void register_search_code_tool(Shells&, const std::shared_ptr<DocRetriever>&);
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

    // Host-coupled tools — registered only when their backend is present.
    detail::register_memory_tools(shells, svc.memory);
    detail::register_todo_tool(shells, svc.todo);
    detail::register_skill_tool(shells, svc.skills);
    detail::register_search_docs_tool(shells, svc.retriever);
    detail::register_search_code_tool(shells, svc.code_retriever);
    detail::register_task_tool(shells, svc.subagent);

    // Self-contained Tier-1 tools land here, gated on cfg.* toggles, once the
    // bodies are ported:
    if (cfg.filesystem)  detail::register_fs_tools(shells);
    // Exec-dependent families. Null exec ⇒ not advertised at all, the same
    // rule register_web_tools follows for a null HttpClient: a tool the host
    // cannot serve should be absent, not present-and-failing.
    if (cfg.shell) {
        detail::register_shell_tools(shells, svc.exec);
        detail::register_process_tools(shells, svc.exec);
    }
    if (cfg.search)      detail::register_search_tools(shells, svc.exec);
    if (cfg.search)      detail::register_structural_tools(shells, svc.code_retriever);
    if (cfg.search)      detail::register_repo_map_tool(shells);
    // Transform / aggregate / structured-data family — its own toggle so a
    // minimal profile can drop it without losing grep/read (see
    // ToolsetConfig::transforms). Each still needs its base capability.
    if (cfg.search && cfg.transforms)      detail::register_textproc_tools(shells);
    if (cfg.filesystem && cfg.transforms)  detail::register_data_tools(shells);
    if (cfg.diagnostics) {
        detail::register_diagnostics_tool(shells, svc.exec);
        detail::register_test_tool(shells, svc.exec);
    }
    if (cfg.git)         detail::register_git_tools(shells, svc.exec);
    if (cfg.web)         detail::register_web_tools(shells, svc.http);

    auto provider = std::make_shared<mcp::cap::LocalProvider>(std::move(origin));
    const int default_budget = cfg.default_output_budget;
    // The tools' memory between calls. The host shares it however it likes;
    // without one, this provider keeps its own and expects one call at a time.
    std::shared_ptr<StateAccess> state = svc.state;
    if (!state) state = std::make_shared<SoleStateAccess>();
    auto split = std::make_shared<const Splitter>(std::move(svc.split));

    for (auto& s : shells.items()) {
        EffectSet fx       = s.effects;
        int       budget   = s.output_budget > 0 ? s.output_budget : default_budget;
        auto      handler  = std::move(s.handler);

        provider->add(s.tool,
            [handler = std::move(handler), fx, budget, state, split](const mcp::cap::Request& req)
                -> mcp::cap::Result {
                Call call;
                call.state     = state.get();
                call.splitter  = split.get();
                call.reader    = req.reader;
                call.cancelled = req.cancelled;
                if (!call.cancelled && req.stop.stop_possible())
                    call.cancelled = [st = req.stop] { return st.stop_requested(); };
                mcp::cap::Result r = handler(call, req.args);
                if (!r.is_error) r.text = apply_budget(std::move(r.text), budget);
                // The tool body (via lower()) may have put file change(s) in
                // structured already; re-stamp effects WITHOUT dropping them.
                // read_changes() returns the single `change` (edit/write/
                // apply_patch) AND the `changes` array (multi-file replace);
                // preserve both.
                auto all = read_changes(r);
                std::optional<FileChange> single;
                std::vector<FileChange>   multi;
                if (all.size() == 1) single = std::move(all.front());
                else if (all.size() > 1) multi = std::move(all);
                attach_meta(r, fx, single, multi);
                return r;
            });
    }
    return provider;
}

} // namespace mcp::tools
