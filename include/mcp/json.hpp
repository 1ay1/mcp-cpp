// SPDX-License-Identifier: Apache-2.0
#pragma once

// Single re-export point for the JSON dependency. CMake fetches nlohmann/json
// (v3.11.3) automatically; substitute your own copy if you already vendor it.
#if __has_include(<nlohmann/json.hpp>)
#  include <nlohmann/json.hpp>
#else
#  error "mcp-cpp requires nlohmann/json. CMake fetches it automatically."
#endif

namespace mcp {
using Json = nlohmann::json;
} // namespace mcp
