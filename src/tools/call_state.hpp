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
    return c.with([](ToolState& s) { return util::bounds_from(s.workspace_root, s.read_roots, s.home); });
}

// The last snapshot of `path`, if any tool saw it.
[[nodiscard]] inline std::optional<util::FileSnapshot>
last_seen(const Call& c, const std::filesystem::path& path) {
    auto key = util::snapshot_key(path);
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
    auto key = util::snapshot_key(path);
    if (key.empty()) return;
    c.with([&](ToolState& s) { s.files[key] = util::FileSnapshot{mtime, size, content_hash}; });
}

[[nodiscard]] inline util::StaleVerdict staleness(const Call& c, const std::filesystem::path& path) {
    return util::staleness_of(path, last_seen(c, path));
}

}  // namespace mcp::tools::detail
