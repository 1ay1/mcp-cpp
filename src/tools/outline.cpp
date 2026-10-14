// SPDX-License-Identifier: Apache-2.0
//
// outline.cpp — definition lines and symbol ranges, from text alone.
#include "outline.hpp"

#include <cctype>
#include <format>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::detail::outline {

namespace {

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

// Cheap NECESSARY condition for "this line could be a definition".
//
// std::regex is a backtracker and running it per line is the whole cost of
// outline: measured 17–26 MB/s, ~50x off memory bandwidth, and it runs on
// every read of a file over 32 KiB. Most lines cannot possibly match, so
// rejecting those without entering the engine is most of the win.
//
// WHAT CANNOT MATCH: the pattern's alternatives all begin, after the
// `^(\s*)` indent, with either `#` (markdown heading) or a character that
// can open an identifier / type expression. The C++-signature alternative
// is `(?:[\w:~<>\[\]&*\s,]+\s+)?\w+\s*\(`, whose optional leading class
// admits `[`, `]`, `&`, `*`, `<`, `>`, `:`, `,` — so those must pass too.
//
// This list is NOT guesswork: a differential run over 2,194,256 lines of
// this tree (8,743 files, 161,970 real matches) is what produced it, and
// it is exact — zero true matches dropped. Two rounds were needed:
//   * letters/_/#/@/~ only          → dropped 3,424 (every `[[nodiscard]]`)
//   * adding []&*<>:,               → dropped 8 (`59 lines (37 sloc)`,
//                                      digit-led, matched as a signature)
// Hence digits too. If you touch this, RE-RUN that differential: a
// prefilter that drops a match makes definitions silently vanish from
// outlines, and nothing downstream can tell.
[[nodiscard]] inline bool outline_plausible(std::string_view line) noexcept {
    // A definition never sits on a line this long; a minified or generated
    // line does, and the backtracking regex would spend seconds on it.
    if (line.size() > 4096) return false;
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
    if (k >= line.size()) return false;
    const unsigned char c = static_cast<unsigned char>(line[k]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '_': case '#': case '@': case '~':   // ident / heading / attr
        case '[': case ']':                       // [[nodiscard]], subscripts
        case '&': case '*':                       // ref / pointer returns
        case '<': case '>': case ':': case ',':   // template + qualified types
            return true;
        default:
            return false;
    }
}

// Does `line` look like a definition? Same answer as running outline_pattern
// on the whole line, but the leading indent is skipped first. The pattern
// starts with `^(\s*)` and every alternative after it also allows leading
// space via `\s+` / `[\w\s...]` classes, so std::regex (a backtracker) tried
// every split of the indent between those — quadratic in the indent. A deeply
// indented continuation line cost ~2 ms each. Checked identical on 381k lines
// of C++/Rust/TS/Python/Markdown; ~12x faster in total.
[[nodiscard]] inline bool outline_line_matches(std::string_view line) {
    if (!outline_plausible(line)) return false;
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
    return std::regex_search(line.data() + k, line.data() + line.size(),
                             outline_pattern());
}

// Whole-line variant (regex_match semantics) for the callers that used it.
// Same indent skip; `^(\s*)` accepts the empty indent, so matching the
// de-indented tail as a whole is the same as matching the full line.
[[nodiscard]] inline bool outline_line_is_def(std::string_view line) {
    if (!outline_plausible(line)) return false;
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
    return std::regex_match(line.data() + k, line.data() + line.size(),
                            outline_pattern());
}

}  // namespace

[[nodiscard]] std::string render(std::string_view content) {
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
symbol_range(std::string_view content, std::string_view symbol,
             bool* decl_only) {
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

[[nodiscard]] std::string render_nested(std::string_view content,
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

}  // namespace mcp::tools::detail::outline
