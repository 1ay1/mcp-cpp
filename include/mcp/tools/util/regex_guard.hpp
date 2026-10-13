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

} // namespace mcp::tools::util

#endif // MCP_TOOLS_UTIL_REGEX_GUARD_HPP
