// tests/test_state.hpp — a tool state for tests: one call at a time, with
// the workspace root set (canonicalised the way a host would).
#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/tools/state.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <filesystem>
#include <memory>
#include <optional>

#include "support/disk_files.hpp"

namespace mcp::test {

inline std::shared_ptr<tools::SoleStateAccess> state_at(const std::filesystem::path& root) {
    auto s = std::make_shared<tools::SoleStateAccess>();
    s->state.workspace_root = tools::util::canonical_root(::mcp::test::DiskFiles{}, root);
    return s;
}

}  // namespace mcp::test

// The one file a single-file tool (write, edit, apply_patch) wrote, or nullopt.
inline std::optional<mcp::cap::FileChange> only_change(const mcp::cap::Result& r) {
    if (r.changes.size() != 1) return std::nullopt;
    return r.changes.front();
}
