// src/tools/call_state.hpp — the few ways tool bodies touch the shared
// state, each a short with() that copies in or out. No lock is held across
// any file or process I/O.
#pragma once

#include <mcp/tools/state.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <optional>
#include <string>

namespace mcp::tools::detail {

// Where this call may reach, copied out of the state.
[[nodiscard]] inline util::Bounds bounds(const Call& c) {
    auto b = c.with([](ToolState& s) {
        util::Bounds out;
        out.workspace = s.workspace_root; out.read_roots = s.read_roots; out.home = s.home;
        out.widen_hint = s.widen_hint;
        out.private_names = s.private_names;
        return out;
    });
    if (!c.scope_workspace.empty()) b.workspace = c.scope_workspace;
    // Canonicalising touches the disk: done outside the state lock.
    auto out = util::bounds_from(c.fs(), b.workspace, std::move(b.read_roots), std::move(b.home));
    out.widen_hint = std::move(b.widen_hint);
    out.private_names = std::move(b.private_names);
    for (const auto& r : c.scope_extra_roots)
        if (!r.empty()) out.extra_roots.push_back(util::canonical_root(c.fs(), r));
    if (!c.scope_project.empty()) out.project = util::canonical_root(c.fs(), c.scope_project);
    return out;
}

// The last snapshot of `path`, if any tool saw it.
[[nodiscard]] inline std::optional<util::FileSnapshot>
last_seen(const Call& c, const std::filesystem::path& path) {
    auto key = util::snapshot_key(c.fs(), path);
    if (key.empty()) return std::nullopt;
    return c.with([&](ToolState& s) -> std::optional<util::FileSnapshot> {
        auto it = s.files.find(key);
        if (it == s.files.end()) return std::nullopt;
        return it->second;
    });
}

// Record what a tool just saw of (or wrote to) `path`.
inline void record_seen(const Call& c, const std::filesystem::path& path,
                        std::filesystem::file_time_type mtime, std::uintmax_t size,
                        std::uint64_t content_hash) {
    auto key = util::snapshot_key(c.fs(), path);
    if (key.empty()) return;
    c.with([&](ToolState& s) { s.files[key] = util::FileSnapshot{mtime, size, content_hash}; });
}

[[nodiscard]] inline util::StaleVerdict staleness(const Call& c, const std::filesystem::path& path) {
    return util::staleness_of(c.fs(), path, last_seen(c, path));
}

}  // namespace mcp::tools::detail
