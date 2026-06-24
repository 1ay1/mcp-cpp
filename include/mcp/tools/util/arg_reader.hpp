// SPDX-License-Identifier: Apache-2.0
#pragma once
// Robust reader for tool-use args. Tolerates the common shapes a streaming
// model emits when its JSON drifts (missing fields, nulls, numbers in a
// string slot, "42" in an int slot, arrays of strings newline-joined).

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::tools::util {

class ArgReader {
public:
    explicit ArgReader(const nlohmann::json& args) noexcept : args_(args) {}

    [[nodiscard]] bool is_object() const noexcept { return args_.is_object(); }

    [[nodiscard]] bool has(std::string_view key) const {
        return raw(key) != nullptr;
    }

    [[nodiscard]] std::string str(std::string_view key,
                                  std::string def = {},
                                  std::string* note = nullptr) const;

    [[nodiscard]] std::optional<std::string> require_str(std::string_view key) const;

    [[nodiscard]] int integer(std::string_view key, int def) const;

    [[nodiscard]] bool boolean(std::string_view key, bool def) const;

    [[nodiscard]] const nlohmann::json* raw(std::string_view key) const noexcept;

private:
    const nlohmann::json& args_;
};

} // namespace mcp::tools::util
