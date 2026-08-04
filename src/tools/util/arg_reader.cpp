// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/arg_reader.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>

namespace mcp::tools::util {

using nlohmann::json;

namespace {
// Alias tables. These are the canonical-key -> {wrong keys a model emits}
// mappings. raw() consults them so a tool reading the canonical key still
// finds a value the model parked under a synonym. This is the SINGLE robust
// layer every tool path flows through (native tool_calls, salvaged leaked
// calls, JSON-protocol, MCP-server tools, the stdio MCP server) — so the weak-
// model vocabulary lives here rather than being re-patched at each call site.
constexpr std::string_view kAliasesPath[]       = {"file_path", "filepath", "filename",
                                                    "file",      "dir",      "directory",
                                                    "target",    "pathname"};
constexpr std::string_view kAliasesFilePath[]   = {"path",      "filepath", "filename",
                                                    "file",      "target",   "pathname"};
constexpr std::string_view kAliasesOldString[]  = {"old_text",  "old_str",  "oldStr",
                                                    "old",       "search",   "find", "from"};
constexpr std::string_view kAliasesNewString[]  = {"new_text",  "new_str",  "newStr",
                                                    "new",       "replace",  "replacement", "to"};
constexpr std::string_view kAliasesContent[]    = {"file_text", "text",
                                                    "file_content", "contents",
                                                    "body", "data", "code"};
constexpr std::string_view kAliasesOffset[]     = {"start_line", "start", "from_line"};
constexpr std::string_view kAliasesLimit[]      = {"end_line",   "num_lines", "max_lines",
                                                    "count",      "line_count"};
constexpr std::string_view kAliasesCd[]         = {"cwd", "workdir", "working_directory",
                                                    "directory"};
constexpr std::string_view kAliasesCommand[]    = {"cmd", "shell_command", "shell",
                                                    "script", "run", "cmdline"};
constexpr std::string_view kAliasesPattern[]    = {"query", "q", "regex", "search",
                                                    "term", "glob", "match", "pat"};
constexpr std::string_view kAliasesQuery[]      = {"q", "search", "term", "text",
                                                    "prompt", "question"};
constexpr std::string_view kAliasesUrl[]        = {"uri", "link", "address", "href"};

std::span<const std::string_view> aliases_for(std::string_view key) noexcept {
    if (key == "path")       return {kAliasesPath};
    if (key == "file_path")  return {kAliasesFilePath};
    if (key == "old_string") return {kAliasesOldString};
    if (key == "new_string") return {kAliasesNewString};
    if (key == "content")    return {kAliasesContent};
    if (key == "offset")     return {kAliasesOffset};
    if (key == "limit")      return {kAliasesLimit};
    if (key == "cd")         return {kAliasesCd};
    if (key == "command")    return {kAliasesCommand};
    if (key == "pattern")    return {kAliasesPattern};
    if (key == "query")      return {kAliasesQuery};
    if (key == "url")        return {kAliasesUrl};
    return {};
}
} // namespace

const json* ArgReader::raw(std::string_view key) const noexcept {
    if (!args_.is_object()) return nullptr;
    if (auto it = args_.find(std::string{key}); it != args_.end())
        return &*it;
    for (auto alt : aliases_for(key)) {
        if (auto it = args_.find(std::string{alt}); it != args_.end())
            return &*it;
    }
    return nullptr;
}

std::string ArgReader::str(std::string_view key,
                           std::string def,
                           std::string* note) const {
    const json* v = raw(key);
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_null()) return def;
    if (v->is_array()) {
        std::string out;
        for (std::size_t i = 0; i < v->size(); ++i) {
            if (i) out += '\n';
            const auto& el = (*v)[i];
            if (el.is_string()) out += el.get<std::string>();
            else                out += el.dump();
        }
        if (note) *note = " (" + std::string{key} + " was an array — joined with newlines)";
        return out;
    }
    std::string out = v->dump();
    if (note) *note = " (" + std::string{key} + " was not a string — coerced)";
    return out;
}

std::optional<std::string> ArgReader::require_str(std::string_view key) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_string()) {
        auto s = v->get<std::string>();
        return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
    }
    auto s = v->dump();
    return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
}

int ArgReader::integer(std::string_view key, int def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    // Read wide, then clamp: a weak model emitting `offset: 9999999999` or
    // `limit: 1e20` must NOT throw json::type_error (surfaces as an opaque
    // "tool crashed") — it should degrade to the int range.
    auto clamp64 = [](long long x) -> int {
        constexpr long long lo = std::numeric_limits<int>::min();
        constexpr long long hi = std::numeric_limits<int>::max();
        return static_cast<int>(x < lo ? lo : (x > hi ? hi : x));
    };
    if (v->is_number_integer()) return clamp64(v->get<long long>());
    if (v->is_number_float()) {
        double d = v->get<double>();
        if (!std::isfinite(d)) return def;          // NaN / ±inf
        if (d <= static_cast<double>(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
        if (d >= static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
        return static_cast<int>(d);
    }
    if (v->is_string()) {
        const auto& s = v->get_ref<const std::string&>();
        long long out = def;
        auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        if (r.ec == std::errc{}) return clamp64(out);
    }
    return def;
}

bool ArgReader::boolean(std::string_view key, bool def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number_integer()) return v->get<int>() != 0;
    if (v->is_string()) {
        std::string s = v->get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (s == "true" || s == "1" || s == "yes")  return true;
        if (s == "false"|| s == "0" || s == "no")   return false;
    }
    return def;
}

} // namespace mcp::tools::util
