// SPDX-License-Identifier: Apache-2.0
//
// effects.cpp — the static effect table for built-in tools, so a host can ask
// "what does `bash` do" without running it. Host-coupled tools (remember/…)
// are pure from the policy's view (their side effects are the host's
// responsibility), so they carry no fs/net/exec tag.

#include <mcp/tools/meta.hpp>

#include <string>
#include <unordered_map>

namespace mcp::tools {

EffectSet effects_for_builtin(const std::string& name) {
    using E = Effect;
    static const std::unordered_map<std::string, EffectSet> table = {
        // Tier-1 (self-contained)
        {"read",            EffectSet{E::ReadFs}},
        {"list_dir",        EffectSet{E::ReadFs}},
        {"grep",            EffectSet{E::ReadFs}},
        {"glob",            EffectSet{E::ReadFs}},
        {"find_definition", EffectSet{E::ReadFs}},
        {"repo_map",        EffectSet{E::ReadFs}},
        {"write",           EffectSet{E::WriteFs}},
        {"edit",            EffectSet{E::WriteFs}},
        {"bash",            EffectSet{E::Exec, E::ReadFs, E::WriteFs, E::Net}},
        {"diagnostics",     EffectSet{E::Exec, E::ReadFs}},
        {"git_status",      EffectSet{E::ReadFs}},
        {"git_diff",        EffectSet{E::ReadFs}},
        {"git_log",         EffectSet{E::ReadFs}},
        {"git_commit",      EffectSet{E::WriteFs, E::Exec}},
        {"web_fetch",       EffectSet{E::Net}},
        {"web_search",      EffectSet{E::Net}},
        // Host-coupled shells: pure from the policy's standpoint.
        {"remember",        EffectSet{}},
        {"forget",          EffectSet{}},
        {"wipe_memory",     EffectSet{}},
        {"todo",            EffectSet{}},
        {"skill",           EffectSet{}},
        {"search_docs",     EffectSet{E::Net}},   // may hit a local embed server
        {"task",            EffectSet{E::ReadFs, E::Net}},
    };
    auto it = table.find(name);
    return it == table.end() ? EffectSet{} : it->second;
}

} // namespace mcp::tools
