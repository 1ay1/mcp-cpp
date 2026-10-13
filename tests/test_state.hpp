// tests/test_state.hpp — a tool state for tests: one call at a time, with
// the workspace root set (canonicalised the way a host would).
#pragma once

#include <mcp/tools/state.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <filesystem>
#include <memory>

#include <mcp/tools/native_files.hpp>

namespace mcp::test {

inline std::shared_ptr<tools::SoleStateAccess> state_at(const std::filesystem::path& root) {
    auto s = std::make_shared<tools::SoleStateAccess>();
    s->state.workspace_root = tools::util::canonical_root(tools::NativeFileSystem{}, root);
    return s;
}

}  // namespace mcp::test
