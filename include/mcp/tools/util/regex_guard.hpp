// SPDX-License-Identifier: Apache-2.0
//
// regex_guard.hpp — a conservative ReDoS guard for model-supplied std::regex.
//
// std::regex (libstdc++) is a backtracking engine with NO step limit and NO way
// to interrupt a match mid-flight, and it does NOT reliably throw on
// catastrophic backtracking — it HANGS. A pattern with a quantifier applied to
// a group that itself contains an unbounded quantifier — (a+)+, (a*)*, (.+)*,
// (\d+)+ … — backtracks EXPONENTIALLY on a non-matching input (`(a+)+$` vs 28
// 'a's already runs ~30 s). Any tool that compiles an arbitrary user regex
// (grep's builtin backend, extract, aggregate, regex replace) is exposed.
//
// Since the work can't be bounded at runtime, we reject the STRUCTURAL cause at
// compile time: a group closed by `)` and immediately quantified by an
// unbounded quantifier (`*`, `+`, `{n,}`-ish) where that group contained an
// unbounded quantifier at its top level. This is the standard "nested
// quantifier" ReDoS signature. It refuses the dangerous minority while passing
// ordinary patterns (escaped metachars and character classes are skipped so
// `\)` / `[)]` don't trip it). Header-only so every tool shares one copy.

#ifndef MCP_TOOLS_UTIL_REGEX_GUARD_HPP
#define MCP_TOOLS_UTIL_REGEX_GUARD_HPP

#include <cstddef>
#include <expected>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::util {

[[nodiscard]] inline bool has_nested_quantifier(std::string_view p) noexcept {
    // Per-group flag: did THIS group contain a top-level unbounded quantifier?
    std::vector<bool> grp_unbounded;
    bool in_class = false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '\\') { ++i; continue; }                // skip the escaped char
        if (in_class) { if (c == ']') in_class = false; continue; }
        if (c == '[') { in_class = true; continue; }
        if (c == '(') { grp_unbounded.push_back(false); continue; }
        if (c == '*' || c == '+' ||
            (c == '{' && p.find('}', i) != std::string_view::npos)) {
            if (!grp_unbounded.empty()) grp_unbounded.back() = true;
            continue;
        }
        if (c == ')') {
            if (grp_unbounded.empty()) continue;         // unbalanced; ctor rejects
            const bool inner_unbounded = grp_unbounded.back();
            grp_unbounded.pop_back();
            std::size_t j = i + 1;
            if (j < p.size() &&
                (p[j] == '*' || p[j] == '+' ||
                 (p[j] == '{' && p.find('}', j) != std::string_view::npos))) {
                if (inner_unbounded) return true;        // (unbounded)+ → ReDoS
            }
            continue;
        }
    }
    return false;
}

// libstdc++ compiles and matches a regex by recursion, about one stack frame
// per pattern element, so a long enough pattern overflows the stack and
// kills the process (a 200 KB literal does, on an 8 MB stack). No real
// search needs more than this.
inline constexpr std::size_t kMaxRegexBytes = 4096;

// Compile a regex the model wrote: length-capped, ReDoS-guarded, and with
// std::regex_error turned into a sentence. `what` names the argument.
[[nodiscard]] inline std::expected<std::regex, std::string>
compile_model_regex(std::string_view pattern, std::regex::flag_type flags,
                    std::string_view what = "pattern") {
    if (pattern.size() > kMaxRegexBytes)
        return std::unexpected(std::string{what} + " is too long for a regex (" +
                               std::to_string(pattern.size()) + " bytes, max " +
                               std::to_string(kMaxRegexBytes) + ")");
    if (has_nested_quantifier(pattern))
        return std::unexpected(std::string{what} + " has a nested unbounded quantifier "
                               "like (a+)+, which can take forever to match; "
                               "flatten it (a+)");
    try {
        return std::regex(std::string{pattern}, flags);
    } catch (const std::regex_error& e) {
        return std::unexpected(std::string{what} + " is not a valid regex (" + e.what() + ")");
    }
}

} // namespace mcp::tools::util

#endif // MCP_TOOLS_UTIL_REGEX_GUARD_HPP
