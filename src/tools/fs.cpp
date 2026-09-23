// SPDX-License-Identifier: Apache-2.0
//
// fs.cpp — register_fs_tools: read / write / list_dir (edit lives in
// fs_edit.cpp and is registered through register_edit_tool, called here).
// Faithful port of agentty's src/tool/tools/{read,write,list_dir}.cpp;
// FileChange (write) is carried via the detail::lower() meta bridge.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/utf8.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

// Forward — edit tool registers through its own TU.
void register_edit_tool(Shells& sh);

namespace {

// ─────────────────────────────────────────────────────────────────────────
//  read
// ─────────────────────────────────────────────────────────────────────────

struct ReadCacheKey {
    // WHOSE context already holds this read. The dedup sentinel below tells
    // the caller "refer to the earlier tool_result instead" — a claim that is
    // only TRUE for the conversation that actually received those bytes.
    //
    // This cache is a process-global static, so without this field a SUBAGENT
    // (fresh context, own turn budget) inherited the parent's entries and got
    // refusals for files it had never seen. Observed in the wild: a coder
    // subagent burned all 23 of its turns re-requesting overlapping ranges of
    // one file, receiving the sentinel every time, and made zero edits. The
    // dedup is a context-economy optimisation; starving a reader is strictly
    // worse than re-sending bytes, so it must never fire across contexts.
    std::string context_id;
    std::string canonical_path;
    int offset = 1;
    int limit  = 2000;
    bool operator==(const ReadCacheKey&) const noexcept = default;
};
struct ReadCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const ReadCacheKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.context_id);
        h = h * 31u + std::hash<std::string>{}(k.canonical_path);
        h = h * 31u + static_cast<std::size_t>(k.offset);
        h = h * 31u + static_cast<std::size_t>(k.limit);
        return h;
    }
};
struct ReadCache {
    std::mutex mu;
    std::unordered_map<ReadCacheKey, fs::file_time_type, ReadCacheKeyHash> seen;
};
[[nodiscard]] ReadCache& read_cache() {
    static ReadCache c;
    return c;
}

constexpr std::size_t kAutoOutlineSize = 32 * 1024;

[[nodiscard]] inline const std::regex& outline_pattern() {
    static const std::regex re(
        R"(^(\s*)((?:#{1,6}\s+\S.*$)|)"
        R"((?:(?:pub\s+|public\s+|private\s+|protected\s+|static\s+|)"
        R"(inline\s+|virtual\s+|async\s+|export\s+|export\s+default\s+|)"
        R"(extern(?:\s+"[^"]*")?\s+|template\s*<[^>]*>\s*)*)"
        R"((?:fn|def|class|struct|enum|impl|trait|interface|namespace|)"
        R"(function|module|component|service|directive)\b[^=]*)|)"
        R"((?:const|let|var)\s+\w+\s*(?:=|:)|)"
        R"(\w+\s*=\s*(?:async\s+)?(?:function|\([^)]*\)\s*=>)|)"
        R"((?:[\w:~<>\[\]&*\s,]+\s+)?\w+\s*\([^)]*\))"
        R"((?:\s*(?:const|noexcept(?:\s*\([^)]*\))?|override|final|mutable|)"
        R"(=\s*(?:default|delete|0)|->\s*[\w:~<>\[\]&*\s,]+))*)"
        R"(\s*\{?\s*$))",
        std::regex::ECMAScript | std::regex::optimize);
    return re;
}

// Does `line` look like a definition? Same answer as running outline_pattern
// on the whole line, but the leading indent is skipped first. The pattern
// starts with `^(\s*)` and every alternative after it also allows leading
// space via `\s+` / `[\w\s...]` classes, so std::regex (a backtracker) tried
// every split of the indent between those — quadratic in the indent. A deeply
// indented continuation line cost ~2 ms each. Checked identical on 381k lines
// of C++/Rust/TS/Python/Markdown; ~12x faster in total.
[[nodiscard]] inline bool outline_line_matches(std::string_view line) {
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
    return std::regex_search(line.data() + k, line.data() + line.size(),
                             outline_pattern());
}

// Whole-line variant (regex_match semantics) for the callers that used it.
// Same indent skip; `^(\s*)` accepts the empty indent, so matching the
// de-indented tail as a whole is the same as matching the full line.
[[nodiscard]] inline bool outline_line_is_def(std::string_view line) {
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
    return std::regex_match(line.data() + k, line.data() + line.size(),
                            outline_pattern());
}

[[nodiscard]] std::string render_outline(std::string_view content) {
    constexpr std::size_t kMaxEntries = 250;
    std::string out;
    out.reserve(content.size() / 16);
    int line_no = 0;
    std::size_t line_start = 0;
    std::size_t emitted = 0;
    auto emit_line = [&](std::string_view line) {
        if (emitted >= kMaxEntries) return;
        std::size_t l = 0;
        while (l < line.size() && (line[l] == ' ' || line[l] == '\t')) ++l;
        auto trimmed = line.substr(l);
        while (!trimmed.empty() && (trimmed.back() == ' '
                                    || trimmed.back() == '\t'
                                    || trimmed.back() == '\r')) {
            trimmed.remove_suffix(1);
        }
        if (trimmed.empty()) return;
        std::format_to(std::back_inserter(out), "[L{}] {}\n", line_no, trimmed);
        ++emitted;
    };
    const auto& re = outline_pattern();
    for (std::size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n') {
            ++line_no;
            auto line = content.substr(line_start, i - line_start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty()) {
                char c0 = line.front();
                bool maybe = (c0 != '}' && c0 != ')' && c0 != ']'
                              && c0 != ';' && c0 != '/');
                if (maybe) {
                    if (outline_line_is_def(line))
                        emit_line(line);
                }
            }
            line_start = i + 1;
        }
    }
    if (emitted >= kMaxEntries) {
        out += std::format("\n[outline truncated at {} entries; "
                           "use start_line/end_line to read specific regions]\n",
                           kMaxEntries);
    }
    return out;
}

// Find the 1-based line where `symbol` is DEFINED, or -1.
//
// The old matcher required the whole definition to fit on ONE line that the
// outline regex accepts. Real C++ signatures usually wrap:
//     auth::AuthHeader resolve_auth_for(std::string_view spec,
//                                       const auth::AuthHeader& creds) {
// so `read symbol=` missed 71 of 73 real lookups mined from saved sessions.
//
// Here a line is a candidate when the symbol appears as a whole word (or as
// the tail of a `Qualified::name`, so `Runtime::render` and `render` both
// work) and is not inside a comment. What follows the name decides:
//   name(...) <quals> {        → definition with a body   (best)
//   name(...) <quals> ;        → declaration              (fallback)
//   class|struct|enum|... name → type definition
//   using name = / name = ...  → alias / variable / lambda
// The `(...)` group may span lines; parentheses are balanced across lines,
// skipping string and char literals. Calls are rejected because a call
// site's `(...)` is followed by `;` `,` `)` `.` `->` or an operator, and
// its line also has code BEFORE the name that is not a type (e.g. `return`,
// `=`, `(`).
[[nodiscard]] static int find_definition_line(
        const std::vector<std::string_view>& lines, std::string_view symbol,
        bool* decl_only = nullptr) {
    if (decl_only) *decl_only = false;
    if (symbol.empty()) return -1;
    const int n = static_cast<int>(lines.size());
    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    // Leaf name + optional qualifier the caller asked for (`A::b` → "b", "A::").
    std::string_view leaf = symbol, qual;
    if (auto p = symbol.rfind("::"); p != std::string_view::npos) {
        leaf = symbol.substr(p + 2);
        qual = symbol.substr(0, p + 2);
    } else if (auto d = symbol.rfind('.'); d != std::string_view::npos) {
        leaf = symbol.substr(d + 1);   // Python / JS `Class.method`
    }
    if (leaf.empty()) return -1;

    // Strip // and # comments and blank out string literal contents so a name
    // mentioned in a comment or a string is never taken as a definition.
    auto code_of = [](std::string_view s) {
        std::string out{s};
        bool in_str = false; char q = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            char c = out[i];
            if (in_str) {
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) out[++i] = ' '; continue; }
                if (c == q) { in_str = false; continue; }
                out[i] = ' ';
                continue;
            }
            if (c == '"' || c == '`') { in_str = true; q = c; continue; }
            if (c == '\'') {
                // C++14 digit separator (30'000) — not a char literal.
                if (i > 0 && std::isxdigit(static_cast<unsigned char>(out[i - 1]))
                    && i + 1 < out.size()
                    && std::isxdigit(static_cast<unsigned char>(out[i + 1])))
                    continue;
                in_str = true; q = c; continue;
            }
            if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') { out.resize(i); break; }
            if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
                auto e = out.find("*/", i + 2);
                std::size_t stop = e == std::string::npos ? out.size() : e + 2;
                for (std::size_t k = i; k < stop; ++k) out[k] = ' ';
                i = stop - 1;
                continue;
            }
        }
        return out;
    };
    // Lines inside /* ... */ blocks that span lines. Scan the real code of
    // each line (a `/*` inside a `//` comment or a string literal — e.g.
    // `// agents/*.md` — must NOT open a block).
    std::vector<char> in_block(static_cast<std::size_t>(n), 0);
    {
        bool open = false;
        for (int i = 0; i < n; ++i) {
            std::string_view s = lines[i];
            std::size_t k = 0;
            if (open) {
                auto e = s.find("*/");
                if (e == std::string_view::npos) { in_block[i] = 1; continue; }
                open = false;
                if (s.find_first_not_of(" \t*/", e + 2) == std::string_view::npos)
                    in_block[i] = 1;
                k = e + 2;
            }
            // Walk the rest honouring strings and `//`.
            bool in_str = false; char q = 0;
            for (; k < s.size(); ++k) {
                char c = s[k];
                if (in_str) {
                    if (c == '\\') { ++k; continue; }
                    if (c == q) in_str = false;
                    continue;
                }
                if (c == '"') { in_str = true; q = c; continue; }
                if (c == '\'') {
                    if (k > 0 && std::isxdigit(static_cast<unsigned char>(s[k - 1]))
                        && k + 1 < s.size()
                        && std::isxdigit(static_cast<unsigned char>(s[k + 1])))
                        continue;               // digit separator
                    in_str = true; q = c; continue;
                }
                if (c == '/' && k + 1 < s.size() && s[k + 1] == '/') break;
                if (c == '/' && k + 1 < s.size() && s[k + 1] == '*') {
                    auto e = s.find("*/", k + 2);
                    if (e == std::string_view::npos) {
                        if (s.find_first_not_of(" \t") == k) in_block[i] = 1;
                        open = true;
                        break;
                    }
                    k = e + 1;
                }
            }
        }
    }

    // Keywords that, sitting right before the name, make it a type definition.
    static constexpr std::string_view kTypeKw[] = {
        "class", "struct", "enum", "union", "namespace", "concept",
        "interface", "trait", "impl", "type", "typedef", "def", "fn",
        "func", "function", "module", "mod", "record", "object",
    };
    // Tokens that, right before the name, mean it's an expression (a call,
    // a use) and not a declarator.
    static constexpr std::string_view kExprKw[] = {
        "return", "co_return", "co_await", "throw", "case", "new",
        "delete", "sizeof", "decltype", "if", "while", "for", "switch",
        "await", "yield", "not", "and", "or", "in", "is", "else", "do",
    };

    // Scan forward from (line i, col c) across lines, balancing (), until the
    // closing paren of the group that starts at c. Returns {line, col-after}.
    auto close_parens = [&](int i, std::size_t c) -> std::pair<int, std::size_t> {
        int depth = 0;
        for (int li = i; li < n && li < i + 40; ++li) {
            const std::string code = code_of(lines[li]);
            for (std::size_t k = (li == i ? c : 0); k < code.size(); ++k) {
                if (code[k] == '(') ++depth;
                else if (code[k] == ')' && --depth == 0) return {li, k + 1};
            }
        }
        return {-1, 0};
    };
    // First significant character after (line, col), skipping blank space,
    // trailing qualifiers (const, noexcept(...), override, -> Ret, etc.).
    auto after_signature = [&](int li, std::size_t col) -> char {
        for (int l = li; l < n && l < li + 8; ++l) {
            const std::string code = code_of(lines[l]);
            std::size_t k = (l == li ? col : 0);
            while (k < code.size()) {
                char ch = code[k];
                if (ch == ' ' || ch == '\t') { ++k; continue; }
                if (is_ident(ch)) {                     // const / noexcept / override / final / mutable / throws
                    std::size_t e = k;
                    while (e < code.size() && (is_ident(code[e]) || code[e] == ':')) ++e;
                    std::string_view w{code.data() + k, e - k};
                    if (w == "const" || w == "noexcept" || w == "override" || w == "final"
                        || w == "mutable" || w == "volatile" || w == "throws"
                        || w == "try" || w == "requires" || w == "where" || w == "async") {
                        k = e;
                        if (k < code.size() && code[k] == '(') {    // noexcept(...) / requires(...)
                            int d = 0;
                            for (; k < code.size(); ++k) {
                                if (code[k] == '(') ++d;
                                else if (code[k] == ')' && --d == 0) { ++k; break; }
                            }
                        }
                        continue;
                    }
                    return 'w';                              // some other word: e.g. `-> Ret` body, K&R
                }
                if (ch == '-' && k + 1 < code.size() && code[k + 1] == '>') {   // trailing return type
                    k += 2;
                    while (k < code.size() && code[k] != '{' && code[k] != ';' && code[k] != '=') ++k;
                    continue;
                }
                if (ch == '&' ) { ++k; continue; }           // ref-qualifier
                if (ch == '[' && k + 1 < code.size() && code[k + 1] == '[') {  // attribute
                    auto e = code.find("]]", k);
                    k = e == std::string::npos ? code.size() : e + 2;
                    continue;
                }
                return ch;
            }
        }
        return 0;
    };

    int best_body = -1, best_type = -1, best_decl = -1, best_assign = -1;
    for (int i = 0; i < n; ++i) {
        if (in_block[i]) continue;
        std::string_view raw = lines[i];
        auto lead = raw.find_first_not_of(" \t");
        if (lead == std::string_view::npos) continue;
        if (raw.substr(lead).starts_with("*") || raw.substr(lead).starts_with("///")) continue;
        const std::string code = code_of(raw);
        for (std::size_t pos = code.find(leaf); pos != std::string::npos;
             pos = code.find(leaf, pos + 1)) {
            const std::size_t end = pos + leaf.size();
            if (pos > 0 && is_ident(code[pos - 1])) continue;
            if (end < code.size() && is_ident(code[end])) continue;
            // Qualifier the caller gave must match what precedes the name.
            std::size_t name_start = pos;
            while (name_start >= 2 && code[name_start - 1] == ':' && code[name_start - 2] == ':') {
                std::size_t q = name_start - 2;
                while (q > 0 && (is_ident(code[q - 1]) || code[q - 1] == '<' || code[q - 1] == '>')) --q;
                name_start = q;
            }
            if (!qual.empty()) {
                std::string_view have{code.data() + name_start, pos - name_start};
                if (!have.ends_with(qual)) continue;
            }
            // What sits before the (qualified) name on this line.
            std::string_view before{code.data(), name_start};
            while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) before.remove_suffix(1);
            std::string_view prev_word;
            {
                std::size_t e = before.size(), b = e;
                while (b > 0 && is_ident(before[b - 1])) --b;
                prev_word = before.substr(b, e - b);
            }
            const char prev_ch = before.empty() ? 0 : before.back();
            bool type_kw = false;
            for (auto k : kTypeKw) if (prev_word == k) type_kw = true;
            bool expr_ctx = false;
            for (auto k : kExprKw) if (prev_word == k) expr_ctx = true;
            if (prev_ch == '.' || (prev_ch == '>' && before.size() >= 2 && before[before.size() - 2] == '-'))
                expr_ctx = true;                          // obj.name / ptr->name
            if (prev_ch == '=' || prev_ch == '(' || prev_ch == ',' || prev_ch == '!'
                || prev_ch == '+' || prev_ch == '|' || prev_ch == '?' || prev_ch == '{'
                || prev_ch == '[' || prev_ch == ';')
                expr_ctx = true;
            // Skip `name` used as a macro argument etc.
            std::size_t k = end;
            while (k < code.size() && (code[k] == ' ' || code[k] == '\t')) ++k;
            // Template args right after the name: name<T>(...)
            if (k < code.size() && code[k] == '<' && !type_kw) {
                int d = 0; std::size_t t = k;
                for (; t < code.size(); ++t) {
                    if (code[t] == '<') ++d;
                    else if (code[t] == '>' && --d == 0) { ++t; break; }
                }
                k = t;
                while (k < code.size() && (code[k] == ' ' || code[k] == '\t')) ++k;
            }

            if (type_kw) {
                if (best_type < 0) best_type = i + 1;
                break;
            }
            if (prev_word == "using" || prev_word == "alias") {
                if (best_type < 0) best_type = i + 1;
                break;
            }
            if (expr_ctx) continue;

            if (k < code.size() && code[k] == '(') {
                // Something must precede a C-family definition's name (a
                // return type / qualifier) unless it's a constructor-ish
                // `Class::name(` or a line-leading K&R / Go-style name.
                const bool has_decl_prefix = !before.empty() || name_start != pos
                                             || name_start == lead;
                if (!has_decl_prefix) continue;
                auto [cl, cc] = close_parens(i, k);
                if (cl < 0) continue;
                const char nx = after_signature(cl, cc);
                if (nx == '{' || nx == ':' ) {          // body, ctor init list, or Python `def f(...):`
                    if (best_body < 0) best_body = i + 1;
                    break;
                }
                if (nx == '=') {                        // = default / = delete / = 0 / => expr
                    if (best_body < 0) best_body = i + 1;
                    break;
                }
                if (nx == ';') {
                    if (!before.empty() && best_decl < 0) best_decl = i + 1;
                    break;
                }
                if (nx == 'w' && !before.empty()) {     // `-> Ret {` split oddly, K&R
                    if (best_decl < 0) best_decl = i + 1;
                    break;
                }
                continue;
            }
            // `name = ...` / `name := ...` / `name: Type =` (vars, consts,
            // JS arrow functions, lambdas, Python assignments).
            if (k < code.size() && (code[k] == '=' || code[k] == ':')
                && !(k + 1 < code.size() && code[k + 1] == '=')
                && !(code[k] == ':' && k + 1 < code.size() && code[k + 1] == ':')) {
                if (best_assign < 0) best_assign = i + 1;
                break;
            }
        }
        if (best_body > 0) break;          // earliest definition-with-body wins
    }
    if (best_body > 0) return best_body;
    if (best_type > 0) return best_type;

    // Fallback: the original whole-line outline match (covers shapes the
    // scanner above doesn't model, e.g. markdown headings).
    for (int i = 0; i < n; ++i) {
        const auto& ln = lines[i];
        if (ln.empty() || in_block[i]) continue;
        auto pos = ln.find(leaf);
        bool whole = false;
        while (pos != std::string_view::npos) {
            bool lok = pos == 0 || !is_ident(ln[pos - 1]);
            std::size_t after = pos + leaf.size();
            bool rok = after >= ln.size() || !is_ident(ln[after]);
            if (lok && rok) { whole = true; break; }
            pos = ln.find(leaf, pos + 1);
        }
        if (!whole) continue;
        char c0 = ln.front();
        if (c0 == '}' || c0 == ')' || c0 == ']' || c0 == ';' || c0 == '/') continue;
        if (outline_line_is_def(ln)) return i + 1;
    }
    if (best_decl > 0) { if (decl_only) *decl_only = true; return best_decl; }
    if (best_assign > 0) return best_assign;
    return -1;
}

// Resolve `symbol` to a [start,end] 1-based line range within `content`: find
// the line that DEFINES it (via the outline def-pattern, whole-word) and return
// its enclosing brace scope. Powers `read(path, symbol=)` — so the model can
// pull exactly one function's body without line arithmetic or `sed`. Returns
// nullopt when the symbol has no definition line here. Brace scan skips // and
// # line comments and quoted strings.
[[nodiscard]] std::optional<std::pair<int,int>>
resolve_symbol_range(std::string_view content, std::string_view symbol,
                     bool* decl_only = nullptr) {
    std::vector<std::string_view> lines;
    {
        std::size_t start = 0;
        for (std::size_t i = 0; i <= content.size(); ++i) {
            if (i == content.size() || content[i] == '\n') {
                auto ln = content.substr(start, i - start);
                if (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
                lines.push_back(ln);
                start = i + 1;
            }
        }
    }
    int def_line = find_definition_line(lines, symbol, decl_only);
    if (def_line < 0) return std::nullopt;

    // Multi-line signatures: the outline regex matches the line with the NAME,
    // but the return type / attributes / template head may sit on the lines
    // ABOVE (e.g. `[[nodiscard]] std::optional<Foo>` \n `bar(...) {`). Back up
    // over those continuation lines so the returned range starts at the real
    // declaration. A line is a continuation of the def below it when it is
    // non-empty, not itself a statement/block end, and reads as a type head:
    // ends in one of  , < & * :  or is a bare template/attribute/qualifier.
    {
        auto is_sig_head = [](std::string_view s) {
            // trim trailing spaces
            std::size_t e = s.find_last_not_of(" \t");
            if (e == std::string_view::npos) return false;   // blank
            s = s.substr(0, e + 1);
            char last = s.back();
            if (last == ';' || last == '{' || last == '}' || last == ':')
                return false;   // statement / block / label end
            std::size_t b = s.find_first_not_of(" \t");
            std::string_view t = s.substr(b);
            // Attribute or template head, or a return-type/qualifier line that
            // flows into the signature (ends in a type-continuation char).
            if (t.rfind("[[", 0) == 0) return true;
            if (t.rfind("template", 0) == 0) return true;
            if (last == ',' || last == '<' || last == '&' || last == '*'
                || last == '>' ) return true;
            static constexpr std::string_view kQual[] = {
                "static", "inline", "constexpr", "const", "virtual",
                "explicit", "friend", "[[nodiscard]]", "pub", "async",
                "public", "private", "protected", "export",
            };
            for (auto q : kQual)
                if (t.rfind(q, 0) == 0 && t.find('(') == std::string_view::npos)
                    return true;
            return false;
        };
        int start = def_line;
        while (start > 1 && is_sig_head(lines[start - 2])) --start;
        def_line = start;
    }

    const int n = static_cast<int>(lines.size());
    constexpr int kMaxSpan = 400;

    // Decide brace-scope vs indent-scope. A def is INDENT-scoped (Python /
    // Ruby / YAML-ish) when its signature line(s) open the block with `:`
    // rather than `{` — brace-counting there is wrong because the body's
    // dicts/sets `{...}` would close the "block" early. Scan the def line and
    // its immediate continuations for the first block-opener.
    auto trailing = [](std::string_view s) -> char {
        std::size_t e = s.find_last_not_of(" \t");
        // ignore a trailing line comment
        if (auto h = s.find('#'); h != std::string_view::npos) {
            std::size_t e2 = s.find_last_not_of(" \t", h ? h - 1 : 0);
            if (e2 != std::string_view::npos && e2 < h) e = e2;
        }
        return e == std::string_view::npos ? '\0' : s[e];
    };
    bool indent_scoped = false;
    for (int ln = def_line; ln <= n && ln - def_line < 6; ++ln) {
        const auto& s = lines[ln - 1];
        if (s.find('{') != std::string_view::npos) { indent_scoped = false; break; }
        if (trailing(s) == ':') { indent_scoped = true; break; }
    }

    // Enclosing brace scope from the def line downward (brace-language path).
    auto delta = [](std::string_view s) {
        int d = 0; bool in_str = false; char q = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (in_str) { if (c == '\\') { ++i; continue; } if (c == q) in_str = false; continue; }
            // C++ raw string R"delim( ... )delim" (incl. u8R/uR/UR/LR): the body
            // is literal, so skip to the matching )delim" to avoid counting
            // braces (or a stray ") inside it. Single-line handling covers the
            // common regex/SQL literal case.
            if ((c == 'R' || c == 'u' || c == 'U' || c == 'L')) {
                std::size_t j = i;
                if (c == 'u' && j + 1 < s.size() && s[j+1] == '8') j += 2;
                else if (c == 'u' || c == 'U' || c == 'L') j += 1;
                if (j + 1 < s.size() && s[j] == 'R' && s[j+1] == '"') {
                    std::size_t k = j + 2; std::string delim;
                    while (k < s.size() && s[k] != '(' && delim.size() < 16
                           && s[k] != ' ' && s[k] != ')' && s[k] != '\\')
                        delim.push_back(s[k++]);
                    if (k < s.size() && s[k] == '(') {
                        std::string term = ")" + delim + "\"";
                        std::size_t close = s.find(term, k + 1);
                        // Skip the whole raw string (to line end if unterminated).
                        i = (close == std::string_view::npos) ? s.size() - 1
                                                              : close + term.size() - 1;
                        continue;
                    }
                }
            }
            // Apostrophe: only a quote if it CLOSES on this line (char
            // literal or short string). An unpaired ' is a Rust lifetime
            // ('a) or an identifier apostrophe — treating it as an open
            // would hide every brace after it on the line.
            if (c == '\'') {
                std::size_t k = i + 1;
                while (k < s.size() && s[k] != '\'') { if (s[k] == '\\') ++k; ++k; }
                if (k < s.size()) i = k;   // skip the closed literal wholesale
                continue;                  // else: ordinary byte
            }
            if (c == '"' || c == '`') { in_str = true; q = c; continue; }
            if (c == '/' && i + 1 < s.size() && s[i+1] == '/') break;
            if (c == '#') break;
            if (c == '{') ++d; else if (c == '}') --d;
        }
        return d;
    };
    int hi = def_line, fd = 0; bool started = false;
    if (!indent_scoped) {
        for (int ln = def_line; ln <= n && ln - def_line < kMaxSpan; ++ln) {
            fd += delta(lines[ln - 1]); if (fd > 0) started = true; hi = ln;
            if (started && fd <= 0) break;
        }
    }
    // Indent-scoped def, OR a brace-less prototype (never opened a block):
    // include the indented body until the indentation returns to the def's
    // level or shallower.
    if (indent_scoped || !started) {
        std::size_t base_indent = lines[def_line-1].find_first_not_of(" \t");
        hi = def_line;
        for (int ln = def_line + 1; ln <= n && ln - def_line < kMaxSpan; ++ln) {
            const auto& s = lines[ln-1];
            std::size_t ind = s.find_first_not_of(" \t");
            if (ind == std::string_view::npos) { hi = ln; continue; }   // blank
            if (base_indent != std::string_view::npos && ind <= base_indent) break;
            hi = ln;
        }
        // Trim trailing blank lines that the indent walk swallowed.
        while (hi > def_line && lines[hi-1].find_first_not_of(" \t")
                                    == std::string_view::npos) --hi;
    }
    return std::make_pair(def_line, hi);
}

struct ReadArgs {
    util::WorkspacePath path;
    int                 offset;
    int                 limit;
    std::string         symbol;   // read just this symbol's definition + body
    std::string         display_description;
    bool                no_explicit_range = true;
};

std::expected<ReadArgs, ToolError> parse_read_args(const json& j) {
    util::ArgReader ar(j);
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    auto wp = util::make_readable_path_checked(*path_opt, "read");
    if (!wp) return std::unexpected(std::move(wp.error()));
    int offset = ar.integer("offset", 1);
    // NEGATIVE offset = tail semantics: offset:-50 means "the last 50
    // lines" (like `tail -n 50`) — the shape every log/build-output check
    // wants, without a bash round-trip or knowing the file's length first.
    // Resolved against the real line count in run_read.
    if (offset == 0) offset = 1;
    // Did the caller pin a START but leave the END open? Paging with only an
    // `offset`/`start_line` and no `limit`/`end_line` is the common case, and
    // defaulting `limit` to 2000 there dumps the ENTIRE rest of the file into
    // context — the "why is every read huge" footgun. A start-anchored read
    // wants a FOCUSED window, so default it to kPageWindow lines. A read with
    // NO position at all (`read(path)`) keeps the whole-file default (2000,
    // or the >32 KiB auto-outline) — that's the deliberate "show me the file"
    // request. An explicit `limit`/`end_line` always wins.
    constexpr int kPageWindow = 250;
    // has() is alias-aware (raw() consults the synonym tables), so has("offset")
    // already covers start_line/start/from_line and has("limit") covers
    // end_line/num_lines/max_lines/count — no need to spell the aliases out.
    const bool anchored_start = ar.has("offset");
    // `end_line` is an END POSITION, not a line count. It's aliased to `limit`
    // in the arg tables (so has("limit") sees it), but its VALUE must be
    // converted (end_line − offset + 1), not used raw — otherwise
    // end_line=180 with offset=120 reads 180 lines (120..299) instead of the
    // 60-line window 120..180 the outline tells the model to ask for. Detect
    // a raw, literal `end_line`/`to_line` key and honour it as a position.
    const bool has_end_line =
        j.contains("end_line") || j.contains("to_line") || j.contains("endLine");
    const bool explicit_end = ar.has("limit");
    const int default_limit = (anchored_start && !explicit_end)
                                  ? kPageWindow : 2000;
    int limit = ar.integer("limit", default_limit);
    // A literal end_line= wins and is interpreted as a position: window is
    // [offset, end_line] inclusive. (A raw `limit`/`num_lines`/`count` stays a
    // count and is already in `limit` above — we only remap the true end key.)
    if (has_end_line) {
        int end_line = 0;
        for (const char* k : {"end_line", "to_line", "endLine"}) {
            auto it = j.find(k);
            if (it != j.end() && it->is_number()) {
                end_line = it->get<int>();
                break;
            }
        }
        if (end_line >= offset) limit = end_line - offset + 1;
    }
    if (limit <= 0) limit = default_limit;
    const std::string symbol = ar.str("symbol", "");
    // symbol= is itself an explicit selection, so it must NOT trigger the
    // whole-file auto-outline path.
    bool explicit_range = ar.has("offset") || ar.has("limit")
                       || ar.has("start_line") || ar.has("end_line")
                       || !symbol.empty();
    return ReadArgs{
        std::move(*wp), offset, limit, symbol,
        ar.str("display_description", ""),
        /*no_explicit_range=*/ !explicit_range,
    };
}

ExecResult run_read(const ReadArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));

    fs::file_time_type current_mtime{};
    {
        std::error_code mtime_ec;
        current_mtime = fs::last_write_time(p, mtime_ec);
        if (!mtime_ec && a.symbol.empty()) {
            std::error_code canon_ec;
            auto canon = fs::weakly_canonical(p, canon_ec);
            if (!canon_ec) {
                ReadCacheKey key{util::read_context(), canon.string(),
                                 a.offset, a.limit};
                std::lock_guard lk{read_cache().mu};
                auto it = read_cache().seen.find(key);
                if (it != read_cache().seen.end() && it->second == current_mtime) {
                    return ToolOutput{
                        "File unchanged since last read. The content from the "
                        "earlier Read tool_result in this conversation is still "
                        "current \xe2\x80\x94 refer to that instead of re-reading.",
                        std::nullopt};
                }
            }
        }
    }
    constexpr uintmax_t kMaxBytes = 1024u * 1024u;
    uintmax_t sz = fs::file_size(p, ec);

    // Image files: hand the picture to a vision model instead of refusing it
    // as binary. Sniffed by magic bytes (extension-agnostic), capped larger
    // than text since a screenshot is legitimately a few MiB. The host maps
    // ToolOutput.images onto this call's tool_result image blocks; a non-vision
    // model just sees the text note. Checked BEFORE the 1 MiB text cap so a
    // normal-sized image isn't rejected for being "too large".
    if (a.symbol.empty()) {
        if (auto mime = util::sniff_image_media_type(p); !mime.empty()) {
            constexpr uintmax_t kMaxImageBytes = 8u * 1024u * 1024u;
            if (!ec && sz > kMaxImageBytes)
                return std::unexpected(ToolError::too_large(std::format(
                    "image is {} KiB (> 8 MiB cap): {}",
                    sz / 1024, a.path.string())));
            std::string raw = util::read_file(a.path);
            if (raw.empty())
                return std::unexpected(ToolError::io(
                    "cannot read image " + a.path.string()));
            ToolOutput out;
            out.text = std::format(
                "[image {} \xc2\xb7 {} \xc2\xb7 {} KiB] attached to this "
                "result; if you can see images it is shown above.",
                a.path.string(), mime, raw.size() / 1024);
            out.images.push_back(util::ToolImage{std::move(mime), std::move(raw)});
            return out;
        }
    }

    if (!ec && sz > kMaxBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 1 MiB cap). "
            "Read in chunks via offset/limit (or start_line/end_line) — "
            "e.g. {{\"path\":\"{}\",\"offset\":1,\"limit\":500}}. "
            "For a structural overview, run `grep` for the symbols you need.",
            sz / 1024, a.path.string())));
    }
    if (util::is_binary_file(p)) {
        return std::unexpected(ToolError::binary(std::format(
            "cannot read binary file: {} ({} bytes). "
            "Use the `shell` tool with `file`, `hexdump`, or similar.",
            a.path.string(), static_cast<uintmax_t>(ec ? 0 : sz))));
    }
    auto content = util::read_file(a.path);

    // symbol=: resolve to the defining line + enclosing block, then read just
    // that range — no `sed`, no line arithmetic. Overrides offset/limit.
    int eff_offset = a.offset;
    int eff_limit  = a.limit;
    // Tail semantics: offset:-N = the last N lines (limit is ignored — the
    // request IS the window). Count lines once, then convert to a normal
    // forward range.
    if (eff_offset < 0) {
        int total = 0;
        for (char c : content) if (c == '\n') ++total;
        if (!content.empty() && content.back() != '\n') ++total;
        int want = -eff_offset;
        if (want > total) want = total;
        eff_offset = total - want + 1;
        eff_limit  = want;
    }
    std::string symbol_header;
    if (!a.symbol.empty()) {
        bool decl_only = false;
        auto rng = resolve_symbol_range(content, a.symbol, &decl_only);
        // Found only a declaration (`int f(int);` in a header): the body the
        // model wants is usually in the paired source — try there first and
        // fall back to the declaration if it isn't.
        if (!rng || decl_only) {
            // Common miss: the model names the header but the body is in the
            // .cpp (or the reverse), or it's in a sibling file of the same
            // directory. Look there before giving up, and say where we went.
            // Only files next to / paired with the named one: cheap, bounded,
            // and never wanders the tree.
            std::vector<fs::path> near;
            {
                static constexpr std::string_view kSrc[] = {".cpp", ".cc", ".cxx", ".c", ".mm"};
                static constexpr std::string_view kHdr[] = {".hpp", ".h", ".hh", ".hxx"};
                const auto stem = p.stem().string();
                const auto dir  = p.parent_path();
                const auto ext  = p.extension().string();
                auto push = [&](const fs::path& q) {
                    std::error_code qec;
                    if (q != p && fs::is_regular_file(q, qec)
                        && std::find(near.begin(), near.end(), q) == near.end())
                        near.push_back(q);
                };
                const bool is_hdr = std::ranges::find(kHdr, ext) != std::end(kHdr);
                const bool is_src = std::ranges::find(kSrc, ext) != std::end(kSrc);
                // Pair across include/ <-> src/ too: include/x/y/z.hpp <-> src/y/z.cpp
                auto swap_root = [&](std::string_view from, std::string_view to) {
                    std::string s = dir.string();
                    auto at = s.find(std::string{"/"} + std::string{from} + "/");
                    if (at == std::string::npos) return fs::path{};
                    std::string rest = s.substr(at + from.size() + 2);
                    fs::path base = fs::path{s.substr(0, at)} / std::string{to};
                    // include/<project>/a/b -> src/a/b (drop the project dir)
                    if (from == "include") {
                        auto slash = rest.find('/');
                        rest = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);
                    }
                    return rest.empty() ? base : base / rest;
                };
                if (is_hdr) {
                    for (auto e : kSrc) push(dir / (stem + std::string{e}));
                    if (auto d2 = swap_root("include", "src"); !d2.empty())
                        for (auto e : kSrc) push(d2 / (stem + std::string{e}));
                }
                if (is_src) {
                    for (auto e : kHdr) push(dir / (stem + std::string{e}));
                }
                // Siblings with the same extension family, capped.
                std::error_code dec;
                int budget = 60;
                for (const auto& e : fs::directory_iterator(dir, dec)) {
                    if (--budget < 0) break;
                    if (!e.is_regular_file(dec)) continue;
                    const auto x = e.path().extension().string();
                    if (std::ranges::find(kSrc, x) != std::end(kSrc)
                        || std::ranges::find(kHdr, x) != std::end(kHdr)
                        || x == ext)
                        push(e.path());
                }
            }
            for (const auto& q : near) {
                std::error_code qec;
                if (fs::file_size(q, qec) > kMaxBytes || qec) continue;
                std::string other;
                try { other = util::read_file(q); } catch (...) { continue; }
                bool other_decl = false;
                auto r2 = resolve_symbol_range(other, a.symbol, &other_decl);
                if (!r2 || (rng && other_decl)) continue;
                // Render the span from that file, same plain-lines shape as
                // a normal read.
                std::string body;
                int ln = 1; std::size_t st = 0;
                for (std::size_t i = 0; i <= other.size(); ++i) {
                    if (i == other.size() || other[i] == '\n') {
                        if (ln >= r2->first && ln <= r2->second) {
                            std::size_t e = i;
                            if (e > st && other[e - 1] == '\r') --e;
                            body.append(other, st, e - st);
                            body.push_back('\n');
                        }
                        if (ln > r2->second) break;
                        ++ln; st = i + 1;
                    }
                }
                return ToolOutput{util::to_valid_utf8(std::format(
                    "SUCCESS: `{}` is {} in {}; its definition is in {}:{} "
                    "(lines {}\xe2\x80\x93{}).\n\n{}",
                    a.symbol, rng ? "only declared" : "not defined",
                    a.path.string(), q.string(), r2->first,
                    r2->first, r2->second, body)), std::nullopt};
            }
            if (!rng)
            return std::unexpected(ToolError::not_found(std::format(
                "no definition of `{}` found in {} (or its paired header/source "
                "and sibling files). Use `find_definition` to search the "
                "whole tree, or drop `symbol` to read the whole file.",
                a.symbol, a.path.string())));
        }
        eff_offset = rng->first;
        eff_limit  = rng->second - rng->first + 1;
        symbol_header = std::format(
            "SUCCESS: `{}` defined at {}:{} (lines {}\xe2\x80\x93{}).\n\n",
            a.symbol, a.path.string(), rng->first, rng->first, rng->second);
    }

    if (a.no_explicit_range && content.size() > kAutoOutlineSize) {
        std::size_t kib = content.size() / 1024;
        std::string outline = render_outline(content);
        std::string out;
        if (!outline.empty()) {
            out = std::format(
                "SUCCESS: File outline retrieved. This file is {} KiB "
                "and was returned as a structural overview instead "
                "of full content to save context.\n\n"
                "IMPORTANT: Do NOT retry this read without a line range "
                "\xe2\x80\x94 you will get the exact same outline back "
                "and waste a turn. To see real file content you MUST "
                "pass start_line + end_line (or offset + limit).\n\n"
                "# Outline of {}\n\n{}\n"
                "NEXT STEPS: to read a specific symbol's body, call "
                "read again with this path plus start_line and "
                "end_line covering the lines around the symbol "
                "(e.g. for `[L120] fn foo()`, try start_line=120, "
                "end_line=180).",
                kib, a.path.string(), outline);
        } else {
            constexpr std::size_t kPeekBytes = 1024;
            std::size_t cut = std::min(kPeekBytes, content.size());
            while (cut > 0
                   && (static_cast<unsigned char>(content[cut]) & 0xC0) == 0x80)
                --cut;
            std::size_t nl = content.rfind('\n', cut == 0 ? 0 : cut - 1);
            if (nl != std::string::npos && nl > 0) cut = nl + 1;
            int total_lines = 0;
            for (char c : content) if (c == '\n') ++total_lines;
            if (!content.empty() && content.back() != '\n') ++total_lines;
            std::string_view peek{content.data(), cut};
            out = std::format(
                "SUCCESS: First 1 KiB of a {} KiB file with no "
                "recognisable code structure (README / log / data dump). "
                "Returned a leading slice instead of the full body to "
                "save context.\n\n"
                "IMPORTANT: Do NOT retry this read without a line range "
                "\xe2\x80\x94 you will get the exact same slice back. To "
                "see more, pass start_line + end_line (or offset + "
                "limit); the file has {} lines total.\n\n"
                "# First 1 KiB of {}\n\n{}",
                kib, total_lines, a.path.string(), peek);
        }
        if (!a.display_description.empty())
            out = a.display_description + "\n" + out;
        if (current_mtime.time_since_epoch().count() != 0) {
            std::error_code canon_ec;
            auto canon = fs::weakly_canonical(p, canon_ec);
            if (!canon_ec) {
                ReadCacheKey key{util::read_context(), canon.string(),
                                 a.offset, a.limit};
                std::lock_guard lk{read_cache().mu};
                read_cache().seen[std::move(key)] = current_mtime;
            }
        }
        util::record_file_seen(p, current_mtime,
                               static_cast<std::uintmax_t>(content.size()),
                               util::content_fnv1a(content));
        return ToolOutput{std::move(out), std::nullopt};
    }
    std::string out;
    out.reserve(content.size() < 1024 * 1024 ? content.size() : 1024 * 1024);
    int total_lines = 0;
    int shown = 0;
    size_t line_start = 0;
    const size_t N = content.size();
    for (size_t i = 0; i < N; ++i) {
        char c = content[i];
        if (c == '\0') {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path.string(), N)));
        }
        if (c == '\n') {
            ++total_lines;
            int n = total_lines;
            if (n >= eff_offset && shown < eff_limit) {
                size_t end = i;
                if (end > line_start && content[end - 1] == '\r') --end;
                out.append(content.data() + line_start, end - line_start);
                out.push_back('\n');
                ++shown;
            }
            line_start = i + 1;
        }
    }
    if (line_start < N) {
        ++total_lines;
        int n = total_lines;
        if (n >= eff_offset && shown < eff_limit) {
            size_t end = N;
            if (end > line_start && content[end - 1] == '\r') --end;
            out.append(content.data() + line_start, end - line_start);
            out.push_back('\n');
            ++shown;
        }
    }
    if (eff_offset > 1 || shown < total_lines) {
        std::string hint;
        if (shown == 0) {
            hint = std::format(
                "\n[offset {} is past the end of the file, which has {} line{} "
                "— nothing to show. Re-read with an offset ≤ {}.]",
                eff_offset, total_lines, total_lines == 1 ? "" : "s",
                total_lines);
        } else {
            hint = std::format("\n[showing lines {}-{} of {}",
                               eff_offset, eff_offset + shown - 1, total_lines);
            int remaining = total_lines - (eff_offset + shown - 1);
            if (remaining > 0 && a.symbol.empty())
                hint += std::format("; {} more — pass offset={} (or start_line={}) "
                                    "for the next chunk",
                                    remaining, eff_offset + shown, eff_offset + shown);
            hint += "]";
        }
        out += hint;
    }
    // Heads-up prefixes for files a model can otherwise misread as "read
    // failed" or waste turns paging line-by-line through.
    std::string heads_up;
    if (N == 0) {
        heads_up = "(file is empty — 0 bytes)\n";
    } else if (a.symbol.empty()) {
        // Minified / single-huge-line files (bundled JS, one-line JSON/SVG):
        // line-based paging is nearly useless, so say so once and point at a
        // better strategy. Trigger on a very high average line length across a
        // non-trivial file.
        const std::size_t avg_line = N / static_cast<std::size_t>(
            total_lines > 0 ? total_lines : 1);
        if (N > 4096 && avg_line > 800) {
            heads_up = std::format(
                "(heads-up: this file averages ~{} chars/line over {} line(s) "
                "— it looks minified or machine-generated, so line paging is "
                "coarse. `grep` for a token, or read a byte range, to target a "
                "spot.)\n", avg_line, total_lines);
        }
    }
    if (!heads_up.empty()) out = heads_up + out;
    if (!symbol_header.empty()) out = symbol_header + out;
    if (!a.display_description.empty())
        out = a.display_description + "\n" + out;
    if (current_mtime.time_since_epoch().count() != 0) {
        std::error_code canon_ec;
        auto canon = fs::weakly_canonical(p, canon_ec);
        if (!canon_ec) {
            ReadCacheKey key{util::read_context(), canon.string(),
                             eff_offset, eff_limit};
            std::lock_guard lk{read_cache().mu};
            read_cache().seen[std::move(key)] = current_mtime;
        }
    }
    util::record_file_seen(p, current_mtime,
                           static_cast<std::uintmax_t>(content.size()),
                           util::content_fnv1a(content));
    return ToolOutput{std::move(out), std::nullopt};
}

// ─────────────────────────────────────────────────────────────────────────
//  write
// ─────────────────────────────────────────────────────────────────────────

struct WriteArgs {
    util::WorkspacePath path;
    std::string content;
    std::string display_description;
    std::string coercion_note;
};

constexpr std::string_view kMetadataKeys[] = {
    "path", "file_path", "filepath", "filename",
    "display_description", "description",
    "append", "mode", "encoding", "overwrite",
};

bool is_metadata_key(std::string_view k) noexcept {
    for (auto m : kMetadataKeys) if (k == m) return true;
    return false;
}

std::optional<std::string> salvage_largest_string(const json& j, std::string& which) {
    if (!j.is_object()) return std::nullopt;
    const std::string* best_key = nullptr;
    const std::string* best_val = nullptr;
    std::size_t best_len = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (is_metadata_key(it.key())) continue;
        if (!it->is_string()) continue;
        const auto& s = it->get_ref<const std::string&>();
        if (s.size() > best_len) { best_len = s.size(); best_key = &it.key(); best_val = &s; }
    }
    if (!best_val || best_len < 4) return std::nullopt;
    which = *best_key;
    return *best_val;
}

std::string describe_keys(const json& j) {
    if (!j.is_object() || j.empty()) return "(no object / empty)";
    std::string out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!out.empty()) out += ", ";
        out += it.key();
    }
    return out;
}

std::expected<WriteArgs, ToolError> parse_write_args(const json& j) {
    util::ArgReader ar(j);
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args(
            std::format("path required (received keys: {})", describe_keys(j))));
    auto wp = util::make_workspace_path_checked(*raw, "write");
    if (!wp) return std::unexpected(std::move(wp.error()));
    std::string note;
    std::string content;
    if (ar.has("content")) {
        content = ar.str("content", "", &note);
    } else {
        std::string picked_key;
        auto rescued = salvage_largest_string(j, picked_key);
        if (!rescued) {
            return std::unexpected(ToolError::invalid_args(std::format(
                "content required — no `content` field or known alias "
                "(file_text, text, body, data, contents, file_content) "
                "was present. Received keys: {}. "
                "Re-run with the full file body in the `content` field.",
                describe_keys(j))));
        }
        content = std::move(*rescued);
        note = std::format(" (content was pulled from non-standard key "
                           "`{}` — please use `content` next time)", picked_key);
    }
    return WriteArgs{std::move(*wp), std::move(content),
                     ar.str("display_description", ""), std::move(note)};
}

ExecResult run_write(const WriteArgs& a) {
    const auto& p = a.path.path();
    constexpr std::size_t kMaxWriteBytes = 5u * 1024u * 1024u;
    if (a.content.size() > kMaxWriteBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "write body is {} KiB (> 5 MiB cap). Split into multiple writes "
            "or stage the file via bash (cat > file <<EOF).",
            a.content.size() / 1024)));
    }
    std::string original;
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (exists && fs::is_directory(p, ec))
        return std::unexpected(ToolError::not_a_file(
            "'" + a.path.string() + "' is a directory — write needs a file path."));
    auto parent = p.parent_path();
    if (!parent.empty() && fs::exists(parent, ec) && !fs::is_directory(parent, ec))
        return std::unexpected(ToolError::not_a_directory(
            "parent of '" + a.path.string() + "' exists but is not a directory."));
    uintmax_t original_size = 0;
    if (exists) {
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError::not_a_file(
                "not a regular file: " + a.path.string()));
        original_size = fs::file_size(p, ec);
        if (ec) original_size = 0;
        if (original_size <= kMaxWriteBytes)
            original = util::read_file(a.path);
    }
    std::string staleness_warning;
    if (exists && util::staleness_of(p) == util::StaleVerdict::Stale) {
        staleness_warning =
            "\xe2\x9a\xa0  The file has changed on disk since the last time a tool "
            "observed it this session. The write OVERWROTE those changes — "
            "if that's not what you wanted, re-read the file and rewrite "
            "with the intended merged content.\n\n";
    }
    if (exists && !original.empty() && original == a.content)
        return ToolOutput{"File already matches content — no changes written.",
                          std::nullopt};
    FileChange change;
    if (!exists || (!original.empty() && original_size <= kMaxWriteBytes))
        change = make_change(a.path.string(), original, a.content);
    else
        change.path = a.path.string();
    if (auto err = util::write_file(a.path, a.content); !err.empty())
        return std::unexpected(ToolError::io(err));
    {
        std::error_code mt_ec;
        auto new_mtime = fs::last_write_time(p, mt_ec);
        if (!mt_ec) {
            util::record_file_seen(p, new_mtime,
                                   static_cast<std::uintmax_t>(a.content.size()),
                                   util::content_fnv1a(a.content));
        }
    }
    std::string prefix;
    if (!staleness_warning.empty()) prefix += staleness_warning;
    if (!a.display_description.empty()) prefix += a.display_description + "\n";
    auto msg = std::format("{}{} {} ({}+ {}-){}",
                           prefix, exists ? "Overwrote" : "Created",
                           a.path.string(), change.added, change.removed,
                           a.coercion_note);
    // Teaching nudge (never blocks — the write already succeeded): when a
    // model OVERWRITES a substantial existing file but only changed a small
    // fraction of its lines, `edit` would have been the better call — it
    // ships only the changed snippet instead of the whole body, can't clobber
    // an unseen concurrent change, and renders a tighter diff. Mirrors
    // bash_tool_suggestion's philosophy. Silent for new files, whole-file
    // rewrites, and small files where write is genuinely the right tool.
    if (exists && !original.empty()) {
        const std::size_t orig_lines =
            static_cast<std::size_t>(
                std::count(original.begin(), original.end(), '\n')) + 1;
        const int churn = change.added + change.removed;
        if (orig_lines >= 40 && churn > 0
            && static_cast<std::size_t>(churn) * 5 < orig_lines) {
            msg += std::format(
                "\n\ntip: this overwrote a {}-line file but only changed ~{} "
                "line(s). For a targeted change like this, `edit` (or "
                "`apply_patch`) ships just the snippet instead of the whole "
                "file, won't clobber unseen concurrent edits, and produces a "
                "cleaner diff — prefer it over `write` when the file already "
                "exists and you're changing part of it.", orig_lines, churn);
        }
    }
    return ToolOutput{std::move(msg), std::move(change)};
}

// ─────────────────────────────────────────────────────────────────────────
//  list_dir
// ─────────────────────────────────────────────────────────────────────────

struct ListDirArgs {
    std::string root;
    bool recursive;
    int max_depth;
    std::string display_description;
};

std::expected<ListDirArgs, ToolError> parse_list_dir_args(const json& j) {
    util::ArgReader ar(j);
    int max_depth = std::clamp(ar.integer("max_depth", 3), 1, 16);
    return ListDirArgs{
        ar.str("path", "."),
        ar.boolean("recursive", false),
        max_depth,
        ar.str("display_description", ""),
    };
}

ExecResult run_list_dir(const ListDirArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "list_dir");
    if (!wp) return std::unexpected(std::move(wp.error()));
    std::error_code ec;
    if (!fs::exists(wp->path(), ec))
        return std::unexpected(ToolError::not_found("directory not found: " + a.root));
    if (!fs::is_directory(wp->path(), ec))
        return std::unexpected(ToolError::not_a_directory("not a directory: " + a.root));

    std::ostringstream out;
    int count = 0;
    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[32];
        const double b = static_cast<double>(bytes);
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", b/1024.0); return buf; }
        if (bytes < 1024*1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fM", b/(1024.0*1024.0)); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fG", b/(1024.0*1024.0*1024.0)); return buf;
    };
    auto list_entry = [&](const fs::directory_entry& entry, int depth) {
        if (count > 1000) return;
        std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        auto fn = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            out << indent << fn << "/\n";
        } else if (entry.is_regular_file(ec)) {
            auto sz = entry.file_size(ec);
            out << indent << fn << "  " << format_size(ec ? 0 : sz) << "\n";
        } else if (entry.is_symlink(ec)) {
            std::error_code link_ec;
            auto target = fs::read_symlink(entry.path(), link_ec);
            if (link_ec)
                out << indent << fn << " -> <unreadable: " << link_ec.message() << ">\n";
            else
                out << indent << fn << " -> " << target.string() << "\n";
        }
        count++;
    };
    if (a.recursive) {
        for (auto it = fs::recursive_directory_iterator(wp->path(),
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > a.max_depth) { it.disable_recursion_pending(); continue; }
            const auto& entry = *it;
            auto fn = entry.path().filename().string();
            const bool is_dir = entry.is_directory(ec);
            if (is_dir && util::should_skip_dir(fn)) {
                list_entry(entry, it.depth());
                it.disable_recursion_pending();
                if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
                continue;
            }
            if (is_dir && it.depth() > 0 && fn.starts_with(".")) {
                it.disable_recursion_pending();
                continue;
            }
            list_entry(*it, it.depth());
            if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
        }
    } else {
        std::vector<fs::directory_entry> entries;
        for (auto& e : fs::directory_iterator(wp->path(), ec))
            entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) {
            bool da = x.is_directory(), db = y.is_directory();
            if (da != db) return da > db;
            return x.path().filename() < y.path().filename();
        });
        for (auto& e : entries) list_entry(e, 0);
        // list_entry stops appending AND counting past the cap — signal
        // truncation without claiming an exact remainder.
        if (count > 1000)
            out << "[>1000 entries, truncated — use glob with a narrower "
                   "pattern]\n";
    }
    if (count == 0) return ToolOutput{"empty directory", std::nullopt};
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ── Schemas / descriptions ─────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
//  safe move / remove
// ─────────────────────────────────────────────────────────────────────────

struct MoveArgs { fs::path source; fs::path destination; bool overwrite = false; };

std::expected<MoveArgs, ToolError> parse_move_args(const json& j) {
    util::ArgReader r{j};
    auto source = r.require_str("source");
    if (!source || source->empty())
        return std::unexpected(ToolError::invalid_args("source is required"));
    auto destination = r.require_str("destination");
    if (!destination || destination->empty())
        return std::unexpected(ToolError::invalid_args("destination is required"));
    auto src = util::make_workspace_path_checked(*source, "move");
    if (!src) return std::unexpected(src.error());
    auto dst = util::make_workspace_path_checked(*destination, "move");
    if (!dst) return std::unexpected(dst.error());
    return MoveArgs{src->path(), dst->path(), r.boolean("overwrite", false)};
}

ExecResult run_move(const MoveArgs& a) {
    std::error_code ec;
    if (!fs::exists(a.source, ec))
        return std::unexpected(ToolError::not_found(a.source.string()));
    if (fs::exists(a.destination, ec)) {
        if (!a.overwrite)
            return std::unexpected(ToolError::ambiguous(
                "destination exists; pass overwrite:true to replace it"));
        if (fs::is_directory(a.destination, ec) && !fs::is_empty(a.destination, ec))
            return std::unexpected(ToolError::invalid_args(
                "refusing to replace a non-empty destination directory"));
        fs::remove(a.destination, ec);
        if (ec) return std::unexpected(ToolError::io(ec.message()));
    }
    fs::create_directories(a.destination.parent_path(), ec);
    if (ec) return std::unexpected(ToolError::io(ec.message()));
    fs::rename(a.source, a.destination, ec);
    if (ec) {
        // Cross-filesystem/device rename (EXDEV: different volume, tmpfs,
        // bind mount) can't be a rename(2). Fall back to copy+delete — the
        // semantics the caller asked for — instead of surfacing a cryptic
        // "Invalid cross-device link".
        std::error_code copy_ec;
        fs::copy(a.source, a.destination,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 copy_ec);
        if (copy_ec)
            return std::unexpected(ToolError::io(
                "move failed: " + ec.message()
                + "; copy fallback also failed: " + copy_ec.message()));
        fs::remove_all(a.source, copy_ec);
        if (copy_ec)
            return std::unexpected(ToolError::io(
                "moved (copied) to " + a.destination.string()
                + " but could not remove the source: " + copy_ec.message()));
    }
    return ToolOutput{std::format("Moved {} -> {}", a.source.string(), a.destination.string()), std::nullopt};
}

struct RemoveArgs { fs::path path; bool recursive = false; };

std::expected<RemoveArgs, ToolError> parse_remove_args(const json& j) {
    util::ArgReader r{j};
    auto path = r.require_str("path");
    if (!path || path->empty())
        return std::unexpected(ToolError::invalid_args("path is required"));
    auto checked = util::make_workspace_path_checked(*path, "remove");
    if (!checked) return std::unexpected(checked.error());
    if (checked->path() == util::workspace_root())
        return std::unexpected(ToolError::invalid_args("refusing to remove the workspace root"));
    return RemoveArgs{checked->path(), r.boolean("recursive", false)};
}

ExecResult run_remove(const RemoveArgs& a) {
    std::error_code ec;
    if (!fs::exists(a.path, ec))
        return std::unexpected(ToolError::not_found(
            "nothing to remove at " + a.path.string()
            + " — it doesn't exist (already deleted, or a wrong path). If you "
              "intended it gone, the desired state is already met; otherwise "
              "`list_dir` the parent to check the name."));
    const bool directory = fs::is_directory(a.path, ec);
    if (directory && !a.recursive && !fs::is_empty(a.path, ec))
        return std::unexpected(ToolError::invalid_args(
            "directory is not empty; pass recursive:true to remove its contents"));
    const auto count = a.recursive ? fs::remove_all(a.path, ec)
                                   : static_cast<std::uintmax_t>(fs::remove(a.path, ec));
    if (ec) return std::unexpected(ToolError::io("remove failed: " + ec.message()));
    return ToolOutput{std::format("Removed {} ({} filesystem entr{})", a.path.string(), count,
                                  count == 1 ? "y" : "ies"), std::nullopt};
}

json move_schema() {
    return json{{"type","object"}, {"required", {"source","destination"}},
        {"properties", {
            {"source", {{"type","string"}, {"description","Workspace-relative or absolute source path."}}},
            {"destination", {{"type","string"}, {"description","Workspace-relative or absolute destination path."}}},
            {"overwrite", {{"type","boolean"}, {"default",false}}}
        }}};
}

json remove_schema() {
    return json{{"type","object"}, {"required", {"path"}},
        {"properties", {
            {"path", {{"type","string"}, {"description","Workspace path to remove."}}},
            {"recursive", {{"type","boolean"}, {"default",false},
                {"description","Required for non-empty directories."}}}
        }}};
}

json read_schema() {
    return json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",       {{"type","string"}, {"description","Absolute or relative path"}}},
            {"symbol",     {{"type","string"}, {"description","Read ONLY this symbol's definition + body (function/class/etc.), resolved to its enclosing block — no line numbers or `sed` needed. e.g. symbol=\"parse_args\"."}}},
            {"offset",     {{"type","integer"}, {"description","Start line (1-based). NEGATIVE = tail: offset:-50 returns the LAST 50 lines (like tail -n 50) — ideal for logs and build output, no need to know the file length."}}},
            {"limit",      {{"type","integer"}, {"description","Max lines"}}},
            {"start_line", {{"type","integer"}, {"description","Alias for offset (Zed-style)"}}},
            {"end_line",   {{"type","integer"}, {"description","Inclusive last line (Zed-style)"}}},
        }},
    };
}

json write_schema() {
    return json{
        {"type","object"},
        {"required", {"file_path","content"}},
        {"properties", {
            {"file_path", {{"type","string"},
                {"description","The absolute path to the file to write "
                               "(must be absolute, not relative)."}}},
            {"content",   {{"type","string"},
                {"description","The content to write to the file."}}},
        }},
    };
}

json list_dir_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",      {{"type","string"}, {"description","Directory to list (default: cwd)"}}},
            {"recursive", {{"type","boolean"}, {"description","List recursively (default: false)"}}},
            {"max_depth", {{"type","integer"}, {"description","Max depth for recursive listing (default: 3)"}}},
        }},
    };
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  outline — a nested symbol tree for ONE file (no bodies).
// ─────────────────────────────────────────────────────────────────────────
// `read` emits a FLAT outline as a side effect for files >32 KiB; this is the
// first-class, intentional "show me this file's SHAPE" tool: works at any
// size, and NESTS entries by leading-indent depth so a class's methods sit
// under it. Reuses the same def/heading pattern read's outline uses, so what
// you see here is exactly what read would surface — just structured, with a
// line number you can jump to via read(symbol=) or read(offset=).

struct OutlineArgs {
    util::WorkspacePath path;
    int  max_entries = 400;
    std::string display_description;
};

[[nodiscard]] std::string render_outline_nested(std::string_view content,
                                                std::size_t max_entries,
                                                int& total_defs) {
    const auto& re = outline_pattern();
    // Collect (indent_cols, line_no, signature) for each matching line.
    struct Row { int cols; int line; std::string sig; };
    std::vector<Row> rows;
    int line_no = 0;
    std::size_t start = 0;
    total_defs = 0;
    while (start <= content.size()) {
        std::size_t nl = content.find('\n', start);
        std::string_view line = content.substr(
            start, (nl == std::string_view::npos ? content.size() : nl) - start);
        ++line_no;
        if (outline_line_matches(line)) {
            // indent width in columns (tab = 4) for depth bucketing
            int cols = 0;
            for (char c : line) {
                if (c == ' ') ++cols;
                else if (c == '\t') cols += 4;
                else break;
            }
            std::size_t l = 0;
            while (l < line.size() && (line[l] == ' ' || line[l] == '\t')) ++l;
            auto trimmed = line.substr(l);
            while (!trimmed.empty() && (trimmed.back() == ' '
                       || trimmed.back() == '\t' || trimmed.back() == '\r'))
                trimmed.remove_suffix(1);
            if (!trimmed.empty()) {
                ++total_defs;
                if (rows.size() < max_entries)
                    rows.push_back({cols, line_no, std::string{trimmed}});
            }
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }

    // Map distinct indent-column values → tree depth (0,1,2,…) so mixed
    // 2-space / 4-space files still nest sanely: depth = rank of this row's
    // indent among the indents of its enclosing (smaller-indent) ancestors.
    std::string out;
    std::vector<int> stack;   // indent cols of open ancestors
    for (auto& r : rows) {
        while (!stack.empty() && stack.back() >= r.cols) stack.pop_back();
        int depth = static_cast<int>(stack.size());
        stack.push_back(r.cols);
        for (int i = 0; i < depth; ++i) out += "  ";
        std::format_to(std::back_inserter(out), "L{}: {}\n", r.line, r.sig);
    }
    return out;
}

ExecResult run_outline(const OutlineArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found(a.path.string()));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file(a.path.string()));
    constexpr std::uintmax_t kMaxBytes = 8u * 1024u * 1024u;
    if (auto sz = fs::file_size(p, ec); !ec && sz > kMaxBytes)
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 8 MiB outline cap).", sz / 1024)));

    std::string content;
    try { content = util::read_file(a.path); }
    catch (...) { return std::unexpected(ToolError::io("cannot read " + a.path.string())); }
    if (content.find('\0') != std::string::npos)
        return std::unexpected(ToolError::binary(a.path.string()));

    int total = 0;
    std::string tree = render_outline_nested(
        content, static_cast<std::size_t>(a.max_entries), total);

    int lines = static_cast<int>(std::count(content.begin(), content.end(), '\n')) + 1;
    std::ostringstream out;
    if (!a.display_description.empty()) out << a.display_description << "\n";
    if (total == 0)
        return ToolOutput{a.path.string() + " (" + std::to_string(lines)
            + " lines): no top-level definitions or headings detected — "
            "read it directly.", std::nullopt};
    out << a.path.string() << " — " << total << " definition"
        << (total==1?"":"s") << " across " << lines << " lines:\n\n" << tree;
    if (total > a.max_entries)
        out << "\n[" << (total - a.max_entries)
            << " more — raise max_entries]";
    return ToolOutput{util::to_valid_utf8(out.str()), std::nullopt};
}

std::expected<OutlineArgs, ToolError> parse_outline_args(const json& j) {
    util::ArgReader r(j);
    if (!r.is_object())
        return std::unexpected(ToolError::invalid_args("expected a JSON object"));
    auto path = r.require_str("path");
    if (!path || path->empty())
        return std::unexpected(ToolError::invalid_args("`path` is required"));
    auto wp = util::make_readable_path_checked(*path, "outline");
    if (!wp) return std::unexpected(std::move(wp.error()));
    OutlineArgs a{std::move(*wp),
                  std::clamp(r.integer("max_entries", 400), 1, 2000),
                  r.str("display_description")};
    return a;
}

json outline_schema() {
    return json{{"type","object"},{"required",{"path"}},{"properties",{
        {"path",{{"type","string"},{"description","File to outline."}}},
        {"display_description",{{"type","string"},{"description","One-line summary shown in the UI. Optional."}}},
        {"max_entries",{{"type","integer"},{"description","Max symbols shown (default 400)."}}},
    }}};
}

void register_fs_tools(Shells& sh) {
    sh.add("read",
        "Read a file from the filesystem. A plain read (no line range) "
        "returns up to 2000 lines from the top; but when you pass a START "
        "position (offset / start_line) WITHOUT an end, it returns a "
        "FOCUSED ~250-line window (not the whole rest of the file) and the "
        "footer tells you how many lines remain + the exact offset to page "
        "\xe2\x80\x94 so paging stays cheap. For files over 32 KiB, "
        "reading without an explicit line range returns a SYMBOL "
        "OUTLINE (function / class / heading names with line "
        "numbers) \xe2\x80\x94 or, if the file has no code "
        "structure, its first 1 KiB \xe2\x80\x94 instead of the "
        "full content; use start_line + end_line (or offset + "
        "limit) on a follow-up read to fetch the specific section "
        "you want. Best of all, pass `symbol` to read exactly one "
        "function/class/type's definition and body (resolved to its "
        "enclosing block) with no line arithmetic — use that instead "
        "of computing ranges or shelling out to sed/head/tail. Include "
        "a brief `display_description` so the user sees why you're reading.",
        read_schema(), EffectSet{Effect::ReadFs},
        body<ReadArgs>(run_read, parse_read_args), 80'000);

    sh.add("write",
        "Writes a file to the local filesystem.\n\n"
        "Usage:\n"
        "- This tool will overwrite the existing file if there is one at the "
        "provided path.\n"
        "- If this is an existing file, you MUST use the Read tool first to "
        "read the file's contents.\n"
        "- Prefer the Edit tool for modifying existing files — it only sends "
        "the diff. Only use this tool to create new files or for complete "
        "rewrites.",
        write_schema(), EffectSet{Effect::WriteFs},
        body<WriteArgs>(run_write, parse_write_args), 40'000);

    sh.add("list_dir",
        "List the contents of a directory. Shows file type, size, and name. "
        "Use this to explore project structure before reading files.",
        list_dir_schema(), EffectSet{Effect::ReadFs},
        body<ListDirArgs>(run_list_dir, parse_list_dir_args), 25'000);

    sh.add("outline",
        "Show a file's SHAPE \xe2\x80\x94 a nested tree of its definitions "
        "(functions / classes / structs / headings) with line numbers, and "
        "NO bodies. The cheap \"what's in this file\" probe before reading: "
        "one call maps a 2000-line source at a fraction of the context cost, "
        "then jump to a symbol with read(symbol=) or read(offset=). Unlike "
        "read's size-triggered outline, this works at any size and nests "
        "methods under their class by indent depth.",
        outline_schema(), EffectSet{Effect::ReadFs},
        body<OutlineArgs>(run_outline, parse_outline_args), 25'000);

    sh.add("move",
        "Move or rename a file or directory within the workspace without invoking a shell. "
        "The destination is never overwritten unless overwrite:true is explicit.",
        move_schema(), EffectSet{Effect::ReadFs, Effect::WriteFs},
        body<MoveArgs>(run_move, parse_move_args), 4'000);

    sh.add("remove",
        "Remove a file or directory within the workspace without invoking a shell. "
        "Non-empty directories require recursive:true; the workspace root is always refused.",
        remove_schema(), EffectSet{Effect::ReadFs, Effect::WriteFs},
        body<RemoveArgs>(run_remove, parse_remove_args), 4'000);

    // edit (own TU — fuzzy splice logic is large)
    register_edit_tool(sh);
}

} // namespace mcp::tools::detail
