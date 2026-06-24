// SPDX-License-Identifier: Apache-2.0
#pragma once
// Uniform plumbing for the "parse json -> typed Args -> run(Args)" pattern
// every tool follows. `adapt` is the only place the raw `const json&`
// surface leaks into tool dispatch.

#include <mcp/tools/util/error.hpp>

#include <concepts>
#include <expected>
#include <functional>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcp::tools::util {

template <class T>
concept ToolArgs = std::is_class_v<T> && std::movable<T>;

template <ToolArgs Args>
[[nodiscard]] auto adapt(
        std::expected<Args, ToolError> (*parse)(const nlohmann::json&),
        ExecResult (*run)(const Args&))
    -> std::function<ExecResult(const nlohmann::json&)>
{
    return [parse, run](const nlohmann::json& j) -> ExecResult {
        auto parsed = parse(j);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        return run(*parsed);
    };
}

} // namespace mcp::tools::util
