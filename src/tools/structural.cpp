// structural.cpp — register_structural_tools: search_structural.
//
// The STRUCTURAL layer of the lexical → structural → semantic search stack
// (see the field consensus: ast-grep / tree-sitter queries answer "where does
// this code SHAPE appear", the question grep can only approximate). grep's #1
// false-positive source is that it matches inside comments and string literals
// and cannot express "any identifier here" or "any argument list there"; a
// structural matcher fixes both.
//
// DEP-FREE BY DESIGN — no tree-sitter, no per-language grammar. Matching the
// repomap philosophy, we get ~90% of ast-grep's practical value from a
// language-agnostic LEXER plus a token-sequence matcher with metavariables:
//
//   • Tokenizer splits source into tokens tagged {Ident, Keyword-ish (folded
//     into Ident), Number, String, Char, Comment, Punct}, tracking the 1-based
//     line of each. Comments and string/char literals are recognised for the
//     C-family / Python / shell / Rust / Go / JS-TS families with one unified
//     scanner (block /*…*/, line // and #, and ''' """ triple + single/double
//     quotes with backslash escapes). Because comment and string BYTES become
//     Comment/String tokens, the matcher NEVER matches a pattern inside them —
//     the exact thing grep gets wrong.
//
//   • The PATTERN is tokenized the same way, except three metavariable forms
//     are recognised in the pattern token stream:
//         $NAME     — matches exactly ONE identifier/literal token, OR one
//                     balanced group ( … ), [ … ], { … } as a single unit.
//         $$$NAME   — matches ZERO OR MORE tokens, balanced-bracket aware
//                     (a variadic "the rest of the arg list" wildcard).
//         $_        / $$$   — anonymous (don't bind, just consume).
//     A metavariable that appears twice must bind to the SAME text (so
//     `$A == $A` matches `x == x` but not `x == y`) — the classic ast-grep
//     back-reference semantic.
//
//   • Match runs the pattern token stream against the file token stream at
//     every start offset, skipping Comment tokens on the file side so the
//     pattern's whitespace-insensitive shape lines up regardless of layout.
//
// PRE-FILTER: before tokenizing a file we require every LITERAL (non-meta,
// non-punct-run) identifier/number/string token in the pattern to appear as a
// substring of the raw bytes — an O(bytes) gate that rejects the vast majority
// of files without paying the tokenizer. ripgrep, when present, does the same
// shortlisting across the tree so only candidate files are opened.
//
// OUTPUT: file:line + the matched line (± context), deduped, capped, with the
// enclosing symbol when detectable — the same compressed shape as grep so the
// agent spends tokens on answers, not noise.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/glob.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/host.hpp>

#include "tool_shell.hpp"
#include "tool_body.hpp"

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;
using mcp::tools::FileChange;

namespace {

// ── Tunables ────────────────────────────────────────────────────────────
constexpr std::size_t kMaxFileBytes = 2u * 1024 * 1024;   // skip huge files
constexpr std::size_t kMaxScanned   = 20000;              // file-count ceiling
constexpr std::size_t kMaxMatches    = 200;               // result cap
constexpr std::size_t kMaxOutBytes   = 24u * 1024;        // output budget
constexpr int         kContext       = 1;                 // ± lines of context

std::size_t worker_count() {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4;
    return std::min<std::size_t>(hc, 16);
}

// Language families we tokenize. The extension decides comment/string rules;
// an unknown extension falls back to the C-family scanner (block + // + "…"),
// which is a safe superset for most curly-brace languages. The split matters:
// in C/Go/Rust `'…'` is a bounded CHAR literal (and in Rust an unpaired `'`
// is a lifetime), while in JS/Python/Shell/Ruby it's a full string; Go
// backticks are raw (no escapes) while JS template literals escape; Rust has
// r#"…"# raw strings and NESTED block comments. Conflating these corrupts
// bracket balance for whole files.
enum class Lang { CFamily, JsLike, Go, Rust, Python, Shell, Ruby, Lua, Unknown };

Lang lang_of(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    if (ext == ".py" || ext == ".pyi") return Lang::Python;
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh") return Lang::Shell;
    if (ext == ".rb") return Lang::Ruby;
    if (ext == ".lua") return Lang::Lua;
    if (ext == ".rs") return Lang::Rust;
    if (ext == ".go") return Lang::Go;
    static const char* kJs[] = {
        ".js", ".ts", ".jsx", ".tsx", ".mjs", ".cjs", ".dart", ".php",
    };
    for (auto* e : kJs) if (ext == e) return Lang::JsLike;
    static const char* kC[] = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx", ".ino",
        ".java", ".kt", ".kts", ".swift", ".zig", ".cs",
        ".scala", ".m", ".mm", ".proto",
    };
    for (auto* e : kC) if (ext == e) return Lang::CFamily;
    return Lang::Unknown;
}

bool is_source_ext(const fs::path& p) { return lang_of(p) != Lang::Unknown; }

// ── Tokenizer ───────────────────────────────────────────────────────────
enum class Tok : std::uint8_t { Ident, Number, String, Char, Comment, Punct };

struct Token {
    Tok         kind;
    std::string text;   // exact bytes (for Ident/Number/Punct); literals keep quotes
    int         line;   // 1-based line where the token STARTS
    std::size_t pos = 0; // byte offset of the token's first byte in the file
};

bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$'; }
bool is_ident_cont(unsigned char c)  { return std::isalnum(c) || c == '_' || c == '$'; }

// The pattern uses '$' as the metavariable sigil, so pattern identifiers may
// contain '$'. File identifiers rarely do (JS allows it); treating '$' as an
// ident char in BOTH keeps them comparable. The distinction "is this token a
// metavariable" is made later by inspecting the text, not the tokenizer.

// Unified lexer. `line_comment` is the char that starts a line comment for the
// language ('#' for Python/Shell/Ruby, 0 for C-family which uses '//'). Block
// comments /*…*/ and C++ // are always recognised for CFamily; Lua uses --.
std::vector<Token> tokenize(std::string_view s, Lang lang) {
    std::vector<Token> out;
    out.reserve(s.size() / 4 + 8);
    int line = 1;
    const std::size_t n = s.size();
    std::size_t i = 0;

    const bool slash_cmt  = (lang == Lang::CFamily || lang == Lang::JsLike ||
                              lang == Lang::Go || lang == Lang::Rust ||
                              lang == Lang::Unknown);
    const bool hash_line  = (lang == Lang::Python || lang == Lang::Shell || lang == Lang::Ruby);
    const bool lua_cmt    = (lang == Lang::Lua);
    const bool nested_cmt = (lang == Lang::Rust);            // /* /* */ */ nests
    const bool cpp_raw    = (lang == Lang::CFamily || lang == Lang::Unknown);
    const bool rust_lit   = (lang == Lang::Rust);            // r#"…"#, lifetimes
    const bool raw_btick  = (lang == Lang::Go);              // `…` raw: \ is a byte
    const bool raw_squote = (lang == Lang::Shell);           // '…' raw: \ is a byte

    auto count_newlines = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to; ++k) if (s[k] == '\n') ++line;
    };

    while (i < n) {
        char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }

        // Line comments.
        if (slash_cmt && c == '/' && i + 1 < n && s[i+1] == '/') {
            std::size_t b = i; while (i < n && s[i] != '\n') ++i;
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line, b});
            continue;
        }
        if (lua_cmt && c == '-' && i + 1 < n && s[i+1] == '-') {
            // Lua --[[ … ]] block or -- line.
            std::size_t b = i;
            if (i + 3 < n && s[i+2] == '[' && s[i+3] == '[') {
                i += 4; std::size_t start = i;
                while (i + 1 < n && !(s[i] == ']' && s[i+1] == ']')) ++i;
                count_newlines(start, i);
                if (i + 1 < n) i += 2;
            } else {
                while (i < n && s[i] != '\n') ++i;
            }
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line, b});
            continue;
        }
        if (hash_line && c == '#') {
            std::size_t b = i; while (i < n && s[i] != '\n') ++i;
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line, b});
            continue;
        }
        // Block comments (C-family; Rust block comments NEST).
        if (slash_cmt && c == '/' && i + 1 < n && s[i+1] == '*') {
            std::size_t b = i; int start_line = line; i += 2;
            std::size_t body = i;
            int depth = 1;
            while (i + 1 < n && depth > 0) {
                if (s[i] == '*' && s[i+1] == '/') { --depth; i += 2; continue; }
                if (nested_cmt && s[i] == '/' && s[i+1] == '*') { ++depth; i += 2; continue; }
                ++i;
            }
            if (depth > 0) i = n;
            count_newlines(body, i);
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, start_line, b});
            continue;
        }

        // Strings — triple-quoted first (Python), then single/double, then char.
        // `escapes=false` for RAW quoting styles (Go backticks, shell single
        // quotes) where a backslash is an ordinary byte — treating it as an
        // escape there would swallow the real closing quote.
        auto scan_quoted = [&](char q, bool escapes) {
            int start_line = line;
            std::size_t b = i;
            // Triple quote?
            if ((lang == Lang::Python) && i + 2 < n && s[i+1] == q && s[i+2] == q) {
                i += 3; std::size_t body = i;
                while (i + 2 < n && !(s[i] == q && s[i+1] == q && s[i+2] == q)) {
                    if (s[i] == '\\') ++i;
                    ++i;
                }
                count_newlines(body, i);
                i = (i + 3 <= n) ? i + 3 : n;
            } else {
                ++i;
                while (i < n && s[i] != q) {
                    if (escapes && s[i] == '\\' && i + 1 < n) ++i;
                    if (s[i] == '\n') ++line;   // permit multiline (JS backtick, etc.)
                    ++i;
                }
                if (i < n) ++i;   // closing quote
            }
            out.push_back({q == '\'' ? Tok::Char : Tok::String,
                           std::string{s.substr(b, i - b)}, start_line, b});
        };
        if (c == '"') { scan_quoted(c, /*escapes=*/true); continue; }
        if (c == '`')  { scan_quoted(c, /*escapes=*/!raw_btick); continue; }
        if (c == '\'') {
            // Rust: `'a` / `'static` are LIFETIMES — an unpaired quote followed
            // by an identifier. Treating one as a string-open would swallow
            // arbitrary code (including brackets) until the next apostrophe.
            // rustc's own rule: it's a char literal iff the next char is a
            // backslash escape or the char after next is the closing quote;
            // otherwise ident-start means lifetime.
            if (rust_lit && i + 1 < n && is_ident_start((unsigned char)s[i+1])
                && s[i+1] != '\\' && !(i + 2 < n && s[i+2] == '\'')) {
                std::size_t b = i; ++i;                    // the quote
                while (i < n && is_ident_cont((unsigned char)s[i])) ++i;
                out.push_back({Tok::Ident, std::string{s.substr(b, i - b)}, line, b});
                continue;
            }
            // In C/Go/Rust '…' is a char literal; in JS/Python/Ruby a string.
            scan_quoted('\'', /*escapes=*/!raw_squote);
            continue;
        }

        // C++ raw string literal: (prefix)R"delim( ... )delim" — the body is
        // fully literal (no escapes, embedded quotes/braces are just bytes), so
        // a naive ".." scan mis-parses it. Detect an R directly before a " ,
        // optionally after an encoding prefix (u8/u/U/L). Must come BEFORE the
        // identifier scan since the prefix looks like an identifier.
        if (cpp_raw && (c == 'R' || c == 'u' || c == 'U' || c == 'L')) {
            // find the 'R"' anchor within a short prefix.
            std::size_t j = i;
            // allow u8R / uR / UR / LR / R
            if (s[j] == 'u' && j + 1 < n && s[j+1] == '8') j += 2;
            else if (s[j] == 'u' || s[j] == 'U' || s[j] == 'L') j += 1;
            if (j < n && s[j] == 'R' && j + 1 < n && s[j+1] == '"') {
                int start_line = line;
                std::size_t b = i;
                std::size_t k = j + 2;                 // just past R"
                std::string delim;                     // up to 16 chars, no '(' ' '
                while (k < n && s[k] != '(' && s[k] != '\n' && delim.size() < 16
                       && s[k] != ' ' && s[k] != ')' && s[k] != '\\')
                    delim.push_back(s[k++]);
                if (k < n && s[k] == '(') {
                    const std::string term = ")" + delim + "\"";
                    k += 1;                            // past '('
                    std::size_t body = k;
                    // find the terminating )delim"
                    std::size_t found = std::string_view{s}.find(term, k);
                    std::size_t end = (found == std::string_view::npos)
                                          ? n : found + term.size();
                    count_newlines(body, end);
                    out.push_back({Tok::String, std::string{s.substr(b, end - b)}, start_line, b});
                    i = end;
                    continue;
                }
                // Not actually a raw string (no '(') — fall through to ident.
            }
        }

        // Rust raw strings: r"…", r#"…"#, r##"…"##, and byte forms b"…",
        // br#"…"#. The body is fully literal; the terminator is `"` followed
        // by the same number of `#` as the opener. Must precede the ident scan
        // (the r/b prefix lexes as an identifier otherwise).
        if (rust_lit && (c == 'r' || c == 'b')) {
            std::size_t j = i;
            if (s[j] == 'b' && j + 1 < n && s[j+1] == 'r') j += 2;   // br
            else if (s[j] == 'r' || s[j] == 'b') j += 1;             // r or b
            std::size_t hashes = 0;
            while (j + hashes < n && s[j + hashes] == '#' && hashes < 64) ++hashes;
            bool is_raw = (s[i] != 'b' || (i + 1 < n && s[i+1] == 'r'));  // b"…" isn't raw but scan_quoted handles it
            if (j + hashes < n && s[j + hashes] == '"' && (is_raw || hashes == 0)) {
                // b"…" (no r): normal escaped string — let scan_quoted do it by
                // skipping just the prefix byte.
                if (s[i] == 'b' && (i + 1 >= n || s[i+1] != 'r')) {
                    ++i; scan_quoted('"', /*escapes=*/true); continue;
                }
                int start_line = line;
                std::size_t b = i;
                std::size_t k = j + hashes + 1;         // past opening quote
                const std::string term = "\"" + std::string(hashes, '#');
                std::size_t found = std::string_view{s}.find(term, k);
                std::size_t end = (found == std::string_view::npos) ? n : found + term.size();
                count_newlines(k, end);
                out.push_back({Tok::String, std::string{s.substr(b, end - b)}, start_line, b});
                i = end;
                continue;
            }
        }

        // Numbers.
        if (std::isdigit((unsigned char)c)
            || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i+1]))) {
            std::size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.'
                             || s[i] == '_' || s[i] == 'x' || s[i] == 'X')) ++i;
            out.push_back({Tok::Number, std::string{s.substr(b, i - b)}, line, b});
            continue;
        }

        // Identifiers (and metavariables in the pattern, which start with '$').
        if (is_ident_start((unsigned char)c)) {
            std::size_t b = i;
            while (i < n && is_ident_cont((unsigned char)s[i])) ++i;
            out.push_back({Tok::Ident, std::string{s.substr(b, i - b)}, line, b});
            continue;
        }

        // Punctuation — one char per token (keeps bracket balancing simple).
        out.push_back({Tok::Punct, std::string(1, c), line, i});
        ++i;
    }
    return out;
}

// ── Pattern compilation ─────────────────────────────────────────────────
enum class MetaKind { None, One, Many };   // $X / $$$X

// Classify a pattern Ident token as a metavariable. Forms:
//   $NAME      → One,  bind=NAME
//   $$$NAME    → Many, bind=NAME
//   $_         → One,  anonymous
//   $$$        → Many, anonymous
std::optional<std::pair<MetaKind, std::string>> meta_of(std::string_view t) {
    if (t.empty() || t[0] != '$') return std::nullopt;
    if (t.rfind("$$$", 0) == 0) {
        std::string name{t.substr(3)};
        return std::make_pair(MetaKind::Many, name == "_" ? std::string{} : name);
    }
    std::string name{t.substr(1)};
    if (name.empty()) return std::nullopt;   // a bare '$' is literal punctuation
    return std::make_pair(MetaKind::One, name == "_" ? std::string{} : name);
}

bool passes_prefilter(std::string_view bytes, const std::vector<std::string>& gates) {
    for (const auto& g : gates)
        if (bytes.find(g) == std::string_view::npos) return false;
    return true;
}

// ── Matcher: a spacegrep-style NESTED-DOCUMENT model ────────────────────
// Rather than match a flat token list (which forces manual bracket-counting
// and lets one stray brace corrupt the whole file), we fold tokens into a
// tree of Nodes, exactly like Semgrep's generic/spacegrep engine:
//   • Atom  — a single token (ident / number / string / punct).
//   • Group — a bracket-delimited run: opener char + children + (maybe) close.
// Brackets introduce "secondary nesting"; a mismatched close simply ends the
// current group, so a broken brace only perturbs one local group, never the
// rest of the document. Matching then recurses over sibling SEQUENCES, so:
//   $X    binds exactly ONE node (an atom OR a whole balanced group) — the
//         ast-grep "one node" intuition falls out for free, no counting.
//   $$$X  matches a run of zero-or-more SIBLINGS within the current group.
//   a literal group matches a same-bracket group and recurses into children.
// Back-references ($X twice ⇒ same source text) hold across the tree.

enum class NodeKind : std::uint8_t { Atom, Group };

struct Node {
    NodeKind          kind;
    Token             tok;         // Atom: the token. Group: the '(' '[' '{' opener token.
    char              open = 0;    // Group: opener char
    std::vector<Node> kids;        // Group: children
    int               line = 0;    // start line (for hit reporting)
    int               end_line = 0;// last line the node spans (== line for atoms)
    std::size_t       beg = 0;     // byte span [beg,end) in the source file —
    std::size_t       end = 0;     // exact splice coordinates for rewrite
};

constexpr char close_for(char o) {
    return o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : 0;
}
bool is_open(const Token& t) {
    return t.kind == Tok::Punct && t.text.size() == 1 &&
           (t.text[0] == '(' || t.text[0] == '[' || t.text[0] == '{');
}
bool is_close(const Token& t) {
    return t.kind == Tok::Punct && t.text.size() == 1 &&
           (t.text[0] == ')' || t.text[0] == ']' || t.text[0] == '}');
}

// Fold [begin,end) tokens into a sibling list of Nodes, recursing on brackets.
// `i` advances by reference. Stops at a close bracket matching `expect_close`
// (consuming it) or at end. Comments are dropped (layout-insensitive match).
std::vector<Node> build_nodes(const std::vector<Token>& toks, std::size_t& i,
                              char expect_close) {
    std::vector<Node> out;
    while (i < toks.size()) {
        const Token& t = toks[i];
        if (t.kind == Tok::Comment) { ++i; continue; }
        if (is_close(t)) {
            if (expect_close && t.text[0] == expect_close) { ++i; return out; }
            // A close with no matching opener in scope: stop this group WITHOUT
            // consuming it, so the stray bracket doesn't corrupt the parent.
            if (expect_close) return out;
            ++i; continue;   // top-level stray close: skip it
        }
        if (is_open(t)) {
            Node g; g.kind = NodeKind::Group; g.tok = t; g.open = t.text[0];
            g.line = t.line;
            g.beg  = t.pos;
            ++i;
            g.kids = build_nodes(toks, i, close_for(t.text[0]));
            // end_line: the close bracket sits at toks[i-1] when consumed;
            // otherwise fall back to the last child's end.
            const bool consumed_close = (i > 0 && is_close(toks[i-1]));
            g.end_line = consumed_close ? toks[i-1].line
                       : (!g.kids.empty() ? g.kids.back().end_line : g.line);
            if (g.end_line < g.line) g.end_line = g.line;
            g.end = consumed_close ? toks[i-1].pos + toks[i-1].text.size()
                  : (!g.kids.empty() ? g.kids.back().end
                                     : g.beg + t.text.size());
            if (g.end < g.beg) g.end = g.beg;
            out.push_back(std::move(g));
            continue;
        }
        Node a; a.kind = NodeKind::Atom; a.tok = t; a.line = t.line;
        a.end_line = t.line;
        a.beg = t.pos; a.end = t.pos + t.text.size();
        out.push_back(std::move(a));
        ++i;
    }
    return out;
}

std::vector<Node> build_tree(const std::vector<Token>& toks) {
    std::size_t i = 0;
    return build_nodes(toks, i, /*expect_close=*/0);
}

// Pattern node: same tree, but Atoms may be metavariables.
struct PNode {
    NodeKind           kind;
    Token              tok;
    char               open = 0;
    MetaKind           meta = MetaKind::None;
    std::string        bind;
    std::vector<PNode> kids;
};

std::vector<PNode> build_pattern_nodes(const std::vector<Token>& toks,
                                       std::size_t& i, char expect_close) {
    std::vector<PNode> out;
    while (i < toks.size()) {
        const Token& t = toks[i];
        if (t.kind == Tok::Comment) { ++i; continue; }
        if (is_close(t)) {
            if (expect_close && t.text[0] == expect_close) { ++i; return out; }
            if (expect_close) return out;
            ++i; continue;
        }
        if (is_open(t)) {
            PNode g; g.kind = NodeKind::Group; g.tok = t; g.open = t.text[0];
            ++i;
            g.kids = build_pattern_nodes(toks, i, close_for(t.text[0]));
            out.push_back(std::move(g));
            continue;
        }
        PNode a; a.kind = NodeKind::Atom; a.tok = t;
        if (t.kind == Tok::Ident)
            if (auto m = meta_of(t.text)) { a.meta = m->first; a.bind = m->second; }
        out.push_back(std::move(a));
        ++i;
    }
    return out;
}

std::vector<PNode> compile_pattern_tree(std::string_view pattern, Lang lang) {
    auto toks = tokenize(pattern, lang);
    std::size_t i = 0;
    return build_pattern_nodes(toks, i, 0);
}

// Serialize a matched node subtree back to source text (for back-ref compare).
void node_text(const Node& n, std::string& out) {
    if (n.kind == NodeKind::Atom) { out += n.tok.text; return; }
    out += n.open;
    for (auto& k : n.kids) node_text(k, out);
    if (char c = close_for(n.open)) out += c;
}
std::string nodes_text(const std::vector<Node>& ns, std::size_t a, std::size_t b) {
    std::string s;
    for (std::size_t k = a; k < b; ++k) node_text(ns[k], s);
    return s;
}

// A metavariable binding: `norm` is the whitespace-normalized token render
// (used for $X == $X back-reference COMPARISON — '64 * sz' and '64*sz' are
// the same capture); [beg,end) is the VERBATIM byte span in the source file
// (used by rewrite to splice the original bytes, preserving spacing).
struct Bound {
    std::string norm;
    std::size_t beg = 0, end = 0;   // beg==end ⇒ empty capture ($$$→nothing)
};
using Binds = std::unordered_map<std::string, Bound>;

bool atoms_equal(const Token& a, const Token& b) {
    return a.kind == b.kind && a.text == b.text;
}

// Match pattern sibling-sequence pat[pi..] against doc siblings doc[di..].
// On success returns true and sets di_end to the doc index just past the match.
bool match_seq(const std::vector<PNode>& pat, std::size_t pi,
               const std::vector<Node>& doc, std::size_t di,
               Binds& binds, std::size_t& di_end);

// Does pattern node `p` match a single doc node `d` (one-to-one)?
bool match_node(const PNode& p, const Node& d, Binds& binds) {
    if (p.kind == NodeKind::Group) {
        if (d.kind != NodeKind::Group || d.open != p.open) return false;
        std::size_t end = 0;
        Binds trial = binds;
        if (!match_seq(p.kids, 0, d.kids, 0, trial, end)) return false;
        if (end != d.kids.size()) return false;   // group children fully consumed
        binds = std::move(trial);
        return true;
    }
    // p is an Atom (literal, since metavars are handled in match_seq).
    if (d.kind != NodeKind::Atom) return false;
    return atoms_equal(p.tok, d.tok);
}

bool match_seq(const std::vector<PNode>& pat, std::size_t pi,
               const std::vector<Node>& doc, std::size_t di,
               Binds& binds, std::size_t& di_end) {
    while (pi < pat.size()) {
        const PNode& p = pat[pi];

        if (p.meta == MetaKind::One) {
            if (di >= doc.size()) return false;
            std::string cap; node_text(doc[di], cap);
            if (!p.bind.empty()) {
                auto it = binds.find(p.bind);
                if (it != binds.end() && it->second.norm != cap) return false;
                binds[p.bind] = Bound{std::move(cap), doc[di].beg, doc[di].end};
            }
            ++pi; ++di;
            continue;
        }

        if (p.meta == MetaKind::Many) {
            // Match a run of 0+ siblings, greedy with backtracking, so the rest
            // of the pattern still lines up. Bounded by the current group's
            // sibling count — never crosses a bracket boundary (that's why the
            // nested model is O(n), not the flat model's cross-file scan).
            for (std::size_t take = doc.size() - di + 1; take-- > 0;) {
                std::size_t end = di + take;
                Binds trial = binds;
                bool ok = true;
                if (!p.bind.empty()) {
                    std::string cap = nodes_text(doc, di, end);
                    auto it = trial.find(p.bind);
                    if (it != trial.end() && it->second.norm != cap) ok = false;
                    else {
                        Bound b;
                        b.norm = std::move(cap);
                        if (end > di) { b.beg = doc[di].beg; b.end = doc[end-1].end; }
                        trial[p.bind] = std::move(b);
                    }
                }
                std::size_t sub_end = 0;
                if (ok && match_seq(pat, pi + 1, doc, end, trial, sub_end)) {
                    binds = std::move(trial);
                    di_end = sub_end;
                    return true;
                }
            }
            return false;
        }

        // Literal atom or group.
        if (di >= doc.size()) return false;
        if (!match_node(p, doc[di], binds)) return false;
        ++pi; ++di;
    }
    di_end = di;
    return true;
}

struct Hit {
    int         line = 0;
    std::size_t beg  = 0, end = 0;   // byte span of the whole match
    Binds       binds;               // metavariable captures (for rewrite)
};

// Try the pattern sequence starting at each sibling position, recursively into
// groups, collecting hit start-lines. The pattern must match a CONTIGUOUS run
// of siblings beginning at that position.
void match_in_scope(const std::vector<PNode>& pat,
                    const std::vector<Node>& doc,
                    std::vector<Hit>& hits) {
    for (std::size_t i = 0; i < doc.size(); ++i) {
        Binds binds;
        std::size_t end = 0;
        if (match_seq(pat, 0, doc, i, binds, end) && end > i) {
            hits.push_back({doc[i].line, doc[i].beg, doc[end-1].end,
                            std::move(binds)});
        }
        // Recurse into this node's group children so nested calls match too.
        if (doc[i].kind == NodeKind::Group)
            match_in_scope(pat, doc[i].kids, hits);
    }
}

std::vector<Hit> match_file(const std::vector<PNode>& pat,
                            const std::vector<Node>& tree) {
    std::vector<Hit> hits;
    if (pat.empty() || tree.empty()) return hits;
    match_in_scope(pat, tree, hits);
    // De-dup by span start (a nested recursion can re-report the same match).
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) {
                  return a.beg != b.beg ? a.beg < b.beg : a.end > b.end;
              });
    hits.erase(std::unique(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.beg == b.beg; }),
              hits.end());
    return hits;
}

// ── Enclosing-symbol heuristic (mirror of search.cpp's for consistency) ──
// Indent-aware backward walk: only consider lines LESS indented than anything
// seen so far (real ancestors); a definition-keyword line wins outright; a
// keyword-less scope opener like `int main(…) {` is kept as fallback — and
// because best_indent drops to its indent, an OUTER class can't steal the
// attribution afterwards.
std::string enclosing_symbol(const std::vector<std::string>& lines, int hit_line) {
    static const char* kw[] = {
        "class ", "struct ", "def ", "fn ", "func ", "function",
        "impl ", "trait ", "namespace ", "interface ", "module ",
        "enum ", "template",
    };
    static const char* ctrl[] = {
        "for ", "for(", "while ", "while(", "if ", "if(", "else",
        "switch ", "switch(", "do ", "do{", "try", "catch", "} else",
        "} catch", "loop ", "loop{", "match ", "match(", "return ",
    };
    auto indent_of = [](const std::string& s) -> int {
        int w = 0;
        for (char c : s) {
            if (c == ' ') ++w;
            else if (c == '\t') w += 4;
            else return w;
        }
        return -1;   // blank
    };
    if (hit_line < 2 || hit_line > static_cast<int>(lines.size()))
        return {};
    int best_indent = indent_of(lines[hit_line - 1]);
    if (best_indent < 0) best_indent = 0;
    std::string fallback;
    for (int ln = hit_line - 1; ln >= 1 && ln > hit_line - 400; --ln) {
        const std::string& raw = lines[ln - 1];
        int ind = indent_of(raw);
        if (ind < 0) continue;                       // blank
        if (ind >= best_indent && ln != hit_line) continue;   // not an ancestor
        std::size_t b = raw.find_first_not_of(" \t");
        std::string s = raw.substr(b);
        bool is_ctrl = false;
        for (auto* c : ctrl) if (s.rfind(c, 0) == 0) { is_ctrl = true; break; }
        if (!is_ctrl) {
            for (auto* k : kw) {
                auto pos = s.find(k);
                if (pos != std::string::npos && pos <= 8) {
                    if (s.size() > 90) s.resize(90);
                    return s;
                }
            }
            // Keyword-less scope opener (C: `int main(…) {`).
            if (!s.empty() && (s.back() == '{' || s.back() == '(')
                && fallback.empty()) {
                fallback = s;
                if (fallback.size() > 90) fallback.resize(90);
            }
        }
        best_indent = ind;
        if (ind == 0 && !fallback.empty()) break;    // reached a top-level opener
    }
    return fallback;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            std::size_t end = i;
            if (end > start && s[end-1] == '\r') --end;
            out.emplace_back(s.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < s.size()) out.emplace_back(s.substr(start));
    return out;
}

// Find the enclosing brace scope of a hit line so `expand` can return the whole
// function/block the match lives in — the answer, not a fragment the model must
// re-`read` to understand. Walks OUTWARD: backward to the line that opens the
// innermost unclosed '{', forward to its matching '}'. A lightweight per-line
// brace scan skips // and # line comments and quoted strings so braces inside
// them don't unbalance the count. Returns [lo,hi] 1-based, clamped to max_span.
std::pair<int,int> enclosing_block(const std::vector<std::string>& lines,
                                   int hit_line, int max_span) {
    auto line_delta = [](const std::string& s) {
        int d = 0; bool in_str = false; char q = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (in_str) {
                if (c == '\\') { ++i; continue; }
                if (c == q) in_str = false;
                continue;
            }
            // Apostrophe: only a quote if it CLOSES on this line — an
            // unpaired ' (Rust lifetime, contraction) must stay an ordinary
            // byte or every brace after it on the line goes uncounted.
            if (c == '\'') {
                std::size_t k = i + 1;
                while (k < s.size() && s[k] != '\'') { if (s[k] == '\\') ++k; ++k; }
                if (k < s.size()) i = k;
                continue;
            }
            if (c == '"' || c == '`') { in_str = true; q = c; continue; }
            if (c == '/' && i + 1 < s.size() && s[i+1] == '/') break;
            if (c == '#') break;
            if (c == '{') ++d;
            else if (c == '}') --d;
        }
        return d;
    };
    const int n = static_cast<int>(lines.size());
    if (hit_line < 1 || hit_line > n) return {hit_line, hit_line};

    int lo = hit_line, depth = 0;
    for (int ln = hit_line; ln >= 1 && hit_line - ln < max_span; --ln) {
        depth += line_delta(lines[ln - 1]);
        lo = ln;
        if (depth > 0) break;   // this line opened our enclosing block
    }
    int hi = hit_line; int fd = 0; bool started = false;
    for (int ln = lo; ln <= n && ln - lo < max_span; ++ln) {
        fd += line_delta(lines[ln - 1]);
        if (fd > 0) started = true;
        hi = ln;
        if (started && fd <= 0) break;   // matching close brace reached
    }
    if (hi < hit_line) hi = hit_line;
    return {lo, hi};
}

// SOTA enclosing scope: instead of re-scanning lines for braces (which the
// flat approach did, and which one stray brace can derail), query the real
// node TREE. Find the smallest brace `{…}` Group that spans the hit line over
// multiple lines — that's the enclosing block/function, computed from actual
// structure. Then extend UP to the declaration line that opens it (the lines
// between the previous top-level boundary and the brace: the signature). Falls
// back to the line-based enclosing_block when the tree has no covering brace
// group (e.g. Python indent scope). Returns [lo,hi] 1-based inclusive.
std::optional<std::pair<int,int>>
scope_from_tree(const std::vector<Node>& nodes, int hit_line) {
    const Node* best = nullptr;   // smallest covering multi-line '{' group
    std::function<void(const std::vector<Node>&)> walk =
        [&](const std::vector<Node>& ns) {
            for (const auto& n : ns) {
                if (n.kind != NodeKind::Group) continue;
                if (n.line <= hit_line && hit_line <= n.end_line) {
                    if (n.open == '{' && n.end_line > n.line) {
                        if (!best || (n.end_line - n.line) < (best->end_line - best->line))
                            best = &n;
                    }
                    walk(n.kids);   // descend for a tighter inner block
                }
            }
        };
    walk(nodes);
    if (!best) return std::nullopt;
    return std::make_pair(best->line, best->end_line);
}

// Indent-based enclosing scope for Python/indent-scoped languages, where the
// brace-group walk finds nothing. Walk BACKWARD from the hit to the nearest
// header line at strictly smaller indent (Python: ends with ':'; Ruby: starts
// with def/class/module), then FORWARD while lines are blank or indented
// deeper than the header. Returns [lo,hi] 1-based inclusive.
std::pair<int,int> indent_scope(const std::vector<std::string>& lines,
                                int hit_line, int max_span, Lang lang) {
    auto indent_of = [](const std::string& s) -> int {
        int w = 0;
        for (char c : s) {
            if (c == ' ') ++w;
            else if (c == '\t') w += 8;
            else return w;
        }
        return -1;   // blank line: no indent information
    };
    auto is_header = [&](const std::string& s) {
        if (lang == Lang::Ruby) {
            std::size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return false;
            std::string_view v{s.data() + b, s.size() - b};
            return v.starts_with("def ") || v.starts_with("class ")
                || v.starts_with("module ");
        }
        std::size_t e = s.find_last_not_of(" \t\r");
        if (e == std::string::npos) return false;
        return s[e] == ':';
    };
    const int n = static_cast<int>(lines.size());
    if (hit_line < 1 || hit_line > n) return {hit_line, hit_line};

    int hit_ind = indent_of(lines[hit_line - 1]);
    if (hit_ind < 0) hit_ind = 0;

    // Backward: nearest header at smaller indent.
    int lo = hit_line, hdr_ind = -1;
    for (int ln = hit_line; ln >= 1 && hit_line - ln <= max_span; --ln) {
        int ind = indent_of(lines[ln - 1]);
        if (ind < 0) continue;                    // blank
        if (ind < hit_ind && is_header(lines[ln - 1])) {
            lo = ln; hdr_ind = ind; break;
        }
        if (ind == 0 && ln < hit_line) break;      // hit top-level code, stop
    }
    if (hdr_ind < 0) return {hit_line, hit_line};  // no header found

    // Forward: body extends while blank or indented deeper than the header.
    int hi = hit_line;
    for (int ln = hit_line; ln <= n && ln - lo <= max_span; ++ln) {
        int ind = indent_of(lines[ln - 1]);
        if (ind < 0) continue;                    // blank: tentative
        if (ind <= hdr_ind && ln > lo) break;
        hi = ln;
    }
    return {lo, hi};
}

// ── Tool args ───────────────────────────────────────────────────────────
struct StructArgs {
    std::string pattern;
    std::string root;
    std::string glob;    // optional extension/name filter
    bool        expand = false;   // return the whole enclosing block per match
    std::string display_description;
};

std::expected<StructArgs, ToolError> parse_args(const json& j) {
    util::ArgReader ar(j);
    auto pat = ar.require_str("pattern");
    if (!pat) return std::unexpected(ToolError::invalid_args("pattern required"));
    std::string p = *std::move(pat);
    if (p.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::unexpected(ToolError::invalid_args("pattern must not be blank"));
    return StructArgs{
        std::move(p),
        ar.str("path", "."),
        ar.str("glob", ""),
        ar.boolean("expand", false),
        ar.str("display_description", ""),
    };
}

struct FileMatch {
    std::string rel;
    std::vector<Hit> hits;
};

// Literal (non-meta) identifier/number tokens the pattern requires, walked
// recursively over the PNode tree — the O(bytes) substring pre-filter.
void collect_gates(const std::vector<PNode>& pat, std::vector<std::string>& out) {
    for (auto& p : pat) {
        if (p.kind == NodeKind::Group) { collect_gates(p.kids, out); continue; }
        if (p.meta != MetaKind::None) continue;
        if ((p.tok.kind == Tok::Ident || p.tok.kind == Tok::Number)
            && p.tok.text.size() >= 2)
            out.push_back(p.tok.text);
    }
}
std::vector<std::string> literal_gates(const std::vector<PNode>& pat) {
    std::vector<std::string> g; collect_gates(pat, g); return g;
}

// A pattern needs at least one literal anchor (ident/number/string/punct/
// bracket) — a pattern of only metavariables would match everything.
bool has_literal_anchor(const std::vector<PNode>& pat) {
    for (auto& p : pat) {
        if (p.kind == NodeKind::Group) return true;     // a bracket is an anchor
        if (p.meta == MetaKind::None) return true;      // any literal token
    }
    return false;
}

// ── Semantic bridge (RAG proposes, structure disposes) ─────────────────
// The structural matcher is a sound decision procedure; the injected
// DocRetriever (agentty's search_code RAG engine) is an approximate
// suggestion engine. They compose at exactly two seams, neither of which
// weakens soundness:
//   1. ZERO HITS → leads. A dead end becomes a lead list: the retriever's
//      nearest passages, each VERIFIED against the live file before being
//      shown (a stale index can never lie into the result).
//   2. OVER-CAP → ordering. When more files match than the output budget
//      renders, semantic proximity decides which files render FIRST. The
//      hit set is untouched — only the order of presentation.

// Build a natural-language-ish query from the pattern's literal tokens — the
// metavariables carry no meaning ('$X == $X' → 'x == x' would mislead the
// embedder). display_description, when the model provided one, is prepended:
// it IS the natural-language intent.
std::string semantic_query(const StructArgs& a,
                           const std::vector<PNode>& pat) {
    std::string q;
    if (!a.display_description.empty()) {
        q = a.display_description;
        q += ' ';
    }
    std::function<void(const std::vector<PNode>&)> walk =
        [&](const std::vector<PNode>& ns) {
            for (auto& p : ns) {
                if (p.kind == NodeKind::Group) { walk(p.kids); continue; }
                if (p.meta != MetaKind::None) continue;
                if (p.tok.kind == Tok::Ident || p.tok.kind == Tok::Number) {
                    q += p.tok.text; q += ' ';
                }
            }
        };
    walk(pat);
    // Trim the trailing space.
    while (!q.empty() && q.back() == ' ') q.pop_back();
    return q;
}

// A retrieved passage is only shown if the file it names still exists inside
// the workspace and still spans the claimed lines — ground-truth gate against
// index staleness. Returns the (possibly clipped) verified line count, or 0
// to drop the lead.
int verify_lead(const fs::path& ws_root, DocPassage& p) {
    if (p.path.empty()) return 0;
    fs::path f = fs::path{p.path}.is_absolute() ? fs::path{p.path}
                                                : ws_root / p.path;
    std::error_code ec;
    if (!fs::exists(f, ec) || ec) return 0;
    std::string bytes = util::read_file(f);
    if (bytes.empty()) return 0;
    int nlines = 1;
    for (char c : bytes) if (c == '\n') ++nlines;
    if (p.line_start < 1) p.line_start = 1;
    if (p.line_start > nlines) return 0;          // range vanished: stale
    if (p.line_end > nlines) p.line_end = nlines; // clip a shrunk file
    if (p.line_end < p.line_start) p.line_end = p.line_start;
    return nlines;
}

// Zero structural hits: turn the dead end into verified leads. Clearly
// labeled approximate — the model knows these are neighbours, not matches.
std::string semantic_leads(DocRetriever* sem, const StructArgs& a,
                           const std::vector<PNode>& pat,
                           const fs::path& ws_root) {
    if (!sem || !sem->warm()) return {};
    std::string q = semantic_query(a, pat);
    if (q.empty()) return {};
    DocQuery dq; dq.query = q; dq.k = 3;
    std::string mode, err;
    auto passages = sem->retrieve(dq, mode, err);
    if (!err.empty() || passages.empty()) return {};
    std::ostringstream out;
    std::size_t shown = 0;
    for (auto& p : passages) {
        if (!verify_lead(ws_root, p)) continue;   // stale → silently dropped
        if (shown == 0)
            out << "\n\nSemantically nearest code (approximate leads from the "
                   "code index, verified against disk — NOT structural "
                   "matches):\n";
        out << "  " << p.path << ":" << p.line_start << "-" << p.line_end;
        // One-line teaser: first non-blank line of the passage body.
        std::istringstream body(p.text);
        for (std::string ln; std::getline(body, ln);) {
            auto b = ln.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            ln = ln.substr(b);
            if (ln.size() > 88) ln.resize(88);
            out << "  — " << ln;
            break;
        }
        out << "\n";
        if (++shown == 3) break;
    }
    return out.str();
}

// Over-cap: order matched FILES by semantic proximity to the query so the
// rendered subset is the relevant subset. Score by best passage per file;
// unscored files keep their relative walk order after the scored ones.
void semantic_order(DocRetriever* sem, const StructArgs& a,
                    const std::vector<PNode>& pat,
                    std::vector<FileMatch>& results) {
    if (!sem || !sem->warm()) return;
    std::string q = semantic_query(a, pat);
    if (q.empty()) return;
    DocQuery dq; dq.query = q; dq.k = 20;
    std::string mode, err;
    auto passages = sem->retrieve(dq, mode, err);
    if (!err.empty() || passages.empty()) return;
    std::unordered_map<std::string, double> score;
    for (auto& p : passages) {
        // Normalize to generic separators for matching against rel paths.
        std::string key = fs::path{p.path}.generic_string();
        auto& s = score[key];
        s = std::max(s, p.score);
    }
    if (score.empty()) return;
    auto file_score = [&](const FileMatch& fm) -> double {
        if (fm.rel.empty()) return -1.0;
        // Exact rel match, or the index used absolute/differently-rooted
        // paths — fall back to suffix containment.
        if (auto it = score.find(fm.rel); it != score.end()) return it->second;
        for (auto& [k, v] : score)
            if (k.size() > fm.rel.size()
                    ? k.ends_with(fm.rel) : fm.rel.ends_with(k))
                return v;
        return 0.0;
    };
    std::stable_sort(results.begin(), results.end(),
                     [&](const FileMatch& x, const FileMatch& y) {
                         return file_score(x) > file_score(y);
                     });
}

ExecResult run_structural(const StructArgs& a, DocRetriever* sem) {
    auto wp = util::make_workspace_path_checked(a.root, "search_structural");
    if (!wp) return std::unexpected(std::move(wp.error()));

    // A pattern's Lang is inferred from its own syntax weakly; we recompile it
    // per-file-family below, but need a default to validate it's non-trivial.
    auto probe_pat = compile_pattern_tree(a.pattern, Lang::CFamily);
    if (probe_pat.empty())
        return std::unexpected(ToolError::invalid_args(
            "pattern tokenized to nothing — provide a code shape like "
            "'foo($$$ARGS)' or 'if ($C) { $$$ }'"));
    // Reject a pattern that is ALL metavariables (would match everything).
    if (!has_literal_anchor(probe_pat))
        return std::unexpected(ToolError::invalid_args(
            "pattern is too broad (only metavariables) — anchor it with at "
            "least one literal token, operator, or bracket"));

    // ── Collect candidate files (built-in walk; ripgrep shortlist optional) ──
    std::vector<fs::path> files;
    files.reserve(4096);
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 wp->path(), fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::error_code lec;
            bool is_dir = it->is_directory(lec);
            if (it->is_symlink(lec)) { if (is_dir) it.disable_recursion_pending(); continue; }
            if (is_dir) {
                if (util::should_skip_dir(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            const fs::path& p = it->path();
            if (!is_source_ext(p)) continue;
            if (!a.glob.empty() && !util::glob_match(a.glob, p.filename().string()))
                continue;
            files.push_back(p);
            if (files.size() >= kMaxScanned) break;
        }
    }
    if (files.empty())
        return ToolOutput{"no source files under the search root.", std::nullopt};

    // Pre-compile a pattern per language family (comment/string rules differ).
    std::unordered_map<int, std::vector<PNode>> pat_by_lang;
    std::unordered_map<int, std::vector<std::string>> gates_by_lang;
    auto pat_for = [&](Lang l) -> const std::vector<PNode>& {
        int key = static_cast<int>(l);
        auto it = pat_by_lang.find(key);
        if (it == pat_by_lang.end()) {
            auto compiled = compile_pattern_tree(a.pattern, l);
            gates_by_lang[key] = literal_gates(compiled);
            it = pat_by_lang.emplace(key, std::move(compiled)).first;
        }
        return it->second;
    };
    // Warm the cache for all families up front (thread-safe reads afterwards).
    // MUST list every Lang value: a worker touching a missing key would
    // default-construct an empty pattern (silently zero matches) and mutate
    // the map concurrently.
    for (Lang l : {Lang::CFamily, Lang::JsLike, Lang::Go, Lang::Rust,
                   Lang::Python, Lang::Shell, Lang::Ruby, Lang::Lua})
        (void)pat_for(l);

    // ── Parallel scan ────────────────────────────────────────────────────
    std::vector<FileMatch> results(files.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> total_hits{0};

    auto worker = [&] {
        for (;;) {
            std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= files.size()) return;
            if (total_hits.load(std::memory_order_relaxed) >= kMaxMatches) return;
            const fs::path& p = files[idx];

            std::error_code ec;
            auto sz = fs::file_size(p, ec);
            if (ec || sz == 0 || sz > kMaxFileBytes) continue;
            if (util::is_binary_file(p)) continue;

            std::string bytes = util::read_file(p);
            if (bytes.empty()) continue;

            Lang lang = lang_of(p);
            int key = static_cast<int>(lang);
            const auto& gates = gates_by_lang[key];
            if (!passes_prefilter(bytes, gates)) continue;

            auto toks = tokenize(bytes, lang);
            auto tree = build_tree(toks);
            auto hits = match_file(pat_by_lang[key], tree);
            if (hits.empty()) continue;

            std::error_code rec;
            auto rel = fs::relative(p, wp->path(), rec);
            results[idx] = FileMatch{
                rec ? p.string() : rel.generic_string(),
                std::move(hits),
            };
            total_hits.fetch_add(results[idx].hits.size(), std::memory_order_relaxed);
        }
    };

    {
        const std::size_t nw = std::min(worker_count(), files.size());
        std::vector<std::thread> pool;
        pool.reserve(nw);
        for (std::size_t w = 0; w < nw; ++w) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    // ── Render ───────────────────────────────────────────────────────────
    std::size_t match_count = 0, file_count = 0;
    for (auto& fm : results) if (!fm.rel.empty()) { ++file_count; match_count += fm.hits.size(); }

    if (match_count == 0) {
        std::string msg =
            "no structural matches. The pattern must match the code SHAPE "
            "token-for-token (metavariables $X / $$$X match one / many). If you "
            "wanted a text match, use `grep` instead.";
        // RAG proposes: append verified semantic neighbours as leads.
        msg += semantic_leads(sem, a, probe_pat, wp->path());
        return ToolOutput{std::move(msg), std::nullopt};
    }

    // More matches than the output budget renders → semantic proximity picks
    // WHICH files render first. The hit set itself is untouched (soundness);
    // only presentation order changes, and only when truncation is possible.
    if (match_count > 20 || total_hits.load(std::memory_order_relaxed) >= kMaxMatches)
        semantic_order(sem, a, probe_pat, results);

    std::ostringstream out;
    if (!a.display_description.empty()) out << a.display_description << "\n";
    out << "Found " << match_count << " structural match(es) in "
        << file_count << " file(s):\n";

    std::size_t emitted = 0;
    bool truncated = false;
    for (auto& fm : results) {
        if (fm.rel.empty()) continue;
        // Re-read for context rendering (cheap; only matched files).
        std::string bytes = util::read_file(fs::path{a.root} / fm.rel);
        if (bytes.empty()) bytes = util::read_file(fs::path{fm.rel});
        auto lines = split_lines(bytes);

        // For expand mode, rebuild the node tree once per matched file so each
        // hit's enclosing block comes from REAL structure (precise group
        // boundaries), not line-based brace re-scanning.
        std::vector<Node> file_tree;
        if (a.expand) file_tree = build_tree(tokenize(bytes, lang_of(fm.rel)));

        out << "\n" << fm.rel << ":\n";
        int last_ctx_end = -1;
        for (auto& h : fm.hits) {
            if (emitted >= kMaxMatches || out.tellp() > static_cast<std::streamoff>(kMaxOutBytes)) {
                truncated = true; break;
            }
            std::string encl = enclosing_symbol(lines, h.line);
            if (!encl.empty()) out << "  └ in " << encl << "\n";
            // expand=true returns the whole enclosing brace scope so the match
            // is self-sufficient — no follow-up `read` needed. Otherwise ±1
            // context lines. Either way the match line itself is always shown
            // and never swallowed by a neighbour's already-printed context.
            int lo, hi;
            if (a.expand) {
                // Indent-scoped langs FIRST (a multi-line dict literal is a
                // brace group that would win otherwise); then the tree (exact
                // group span); brace line-scan as final fallback.
                Lang flang = lang_of(fm.rel);
                if (flang == Lang::Python || flang == Lang::Ruby) {
                    auto blk = indent_scope(lines, h.line, /*max_span=*/120, flang);
                    lo = blk.first; hi = blk.second;
                } else if (auto sc = scope_from_tree(file_tree, h.line)) {
                    lo = sc->first; hi = sc->second;
                } else {
                    auto blk = enclosing_block(lines, h.line, /*max_span=*/80);
                    lo = blk.first; hi = blk.second;
                }
                // Cap the span so a giant function can't blow the budget.
                if (hi - lo > 120) hi = lo + 120;
            } else {
                lo = std::max(1, h.line - kContext);
                hi = std::min<int>(static_cast<int>(lines.size()), h.line + kContext);
            }
            for (int ln = lo; ln <= hi; ++ln) {
                if (ln != h.line && ln <= last_ctx_end) continue;
                if (ln >= 1 && ln <= static_cast<int>(lines.size())) {
                    const std::string& src = lines[ln - 1];
                    out << (ln == h.line ? "  > " : "    ") << "L" << ln << ": "
                        << (src.size() > 200 ? src.substr(0, 200) + "…" : src) << "\n";
                }
            }
            last_ctx_end = std::max(last_ctx_end, hi);
            ++emitted;
        }
        if (truncated) break;
    }
    if (truncated)
        out << "\n[truncated at " << emitted << " matches / "
            << (kMaxOutBytes / 1024) << " KiB — narrow the pattern or `path`]\n";

    return ToolOutput{out.str(), std::nullopt};
}

// ── rewrite_structural — the sound matcher as a REFACTORING engine ─────
// Same pattern language, plus a `rewrite` template: every match's byte span
// is replaced by the template with $X / $$$Y substituted by the VERBATIM
// source bytes each metavariable captured (original spacing preserved — the
// normalized form is only for back-ref comparison). Because matches carry
// exact [beg,end) spans from the node tree, splices are token-precise:
// comments and strings can never be edited, and overlapping matches are
// resolved deterministically (outermost-first, inner dropped).
//
// dry_run=true (the default) renders a preview — per-file before/after line
// pairs — without touching disk. apply=true performs the splices and writes
// each file atomically, returning per-file replace counts.

struct RewriteArgs {
    std::string pattern;
    std::string rewrite;
    std::string root;
    std::string glob;
    bool        apply = false;      // false ⇒ dry-run preview
    std::string display_description;
};

// Parse the rewrite template ONCE into literal runs + metavariable refs.
struct TplPiece {
    std::string lit;    // literal bytes (may be empty)
    std::string var;    // metavariable name to splice (empty ⇒ none)
};
std::vector<TplPiece> parse_template(std::string_view tpl) {
    std::vector<TplPiece> out;
    std::string lit;
    std::size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] == '$') {
            std::size_t sig = (tpl.compare(i, 3, "$$$") == 0) ? 3 : 1;
            std::size_t j = i + sig;
            std::size_t b = j;
            while (j < tpl.size() && (std::isalnum((unsigned char)tpl[j]) || tpl[j] == '_'))
                ++j;
            if (j > b) {
                out.push_back({std::move(lit), std::string{tpl.substr(b, j - b)}});
                lit.clear();
                i = j;
                continue;
            }
        }
        lit += tpl[i++];
    }
    if (!lit.empty()) out.push_back({std::move(lit), ""});
    return out;
}

// Render the template for one hit. Returns nullopt if the template names a
// metavariable the pattern never bound (caught per-hit, reported once).
std::optional<std::string> render_template(const std::vector<TplPiece>& tpl,
                                           const Binds& binds,
                                           const std::string& file_bytes) {
    std::string out;
    for (const auto& p : tpl) {
        out += p.lit;
        if (p.var.empty()) continue;
        auto it = binds.find(p.var);
        if (it == binds.end()) return std::nullopt;
        const Bound& b = it->second;
        if (b.end > b.beg && b.end <= file_bytes.size())
            out.append(file_bytes, b.beg, b.end - b.beg);   // verbatim bytes
        // empty capture ($$$ matched nothing) splices nothing — correct.
    }
    return out;
}

// Metavariable names the template references (to validate against pattern).
std::vector<std::string> template_vars(const std::vector<TplPiece>& tpl) {
    std::vector<std::string> v;
    for (auto& p : tpl) if (!p.var.empty()) v.push_back(p.var);
    return v;
}

// Metavariable names the PATTERN binds.
void pattern_vars(const std::vector<PNode>& pat, std::vector<std::string>& out) {
    for (auto& p : pat) {
        if (p.kind == NodeKind::Group) { pattern_vars(p.kids, out); continue; }
        if (p.meta != MetaKind::None && !p.bind.empty()) out.push_back(p.bind);
    }
}

std::expected<RewriteArgs, ToolError> parse_rewrite_args(const json& j) {
    util::ArgReader ar(j);
    auto pat = ar.require_str("pattern");
    if (!pat) return std::unexpected(ToolError::invalid_args("pattern required"));
    auto rw = ar.require_str("rewrite");
    if (!rw) return std::unexpected(ToolError::invalid_args("rewrite required"));
    std::string p = *std::move(pat);
    if (p.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::unexpected(ToolError::invalid_args("pattern must not be blank"));
    return RewriteArgs{
        std::move(p),
        *std::move(rw),
        ar.str("path", "."),
        ar.str("glob", ""),
        ar.boolean("apply", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_rewrite(const RewriteArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "rewrite_structural");
    if (!wp) return std::unexpected(std::move(wp.error()));

    auto probe_pat = compile_pattern_tree(a.pattern, Lang::CFamily);
    if (probe_pat.empty())
        return std::unexpected(ToolError::invalid_args(
            "pattern tokenized to nothing — provide a code shape like "
            "'foo($$$ARGS)'"));
    if (!has_literal_anchor(probe_pat))
        return std::unexpected(ToolError::invalid_args(
            "pattern is too broad (only metavariables) — anchor it with at "
            "least one literal token, operator, or bracket"));

    auto tpl = parse_template(a.rewrite);
    // Validate: every template var must be bound by the pattern.
    {
        std::vector<std::string> pv;
        pattern_vars(probe_pat, pv);
        for (auto& v : template_vars(tpl)) {
            if (std::find(pv.begin(), pv.end(), v) == pv.end())
                return std::unexpected(ToolError::invalid_args(
                    "rewrite template references $" + v +
                    " which the pattern never binds"));
        }
    }

    // Collect candidate files (same walk as search).
    std::vector<fs::path> files;
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 wp->path(), fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::error_code lec;
            bool is_dir = it->is_directory(lec);
            if (it->is_symlink(lec)) { if (is_dir) it.disable_recursion_pending(); continue; }
            if (is_dir) {
                if (util::should_skip_dir(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            const fs::path& p = it->path();
            if (!is_source_ext(p)) continue;
            if (!a.glob.empty() && !util::glob_match(a.glob, p.filename().string()))
                continue;
            files.push_back(p);
            if (files.size() >= kMaxScanned) break;
        }
    }
    if (files.empty())
        return ToolOutput{"no source files under the search root.", std::nullopt};

    std::unordered_map<int, std::vector<PNode>> pat_by_lang;
    std::unordered_map<int, std::vector<std::string>> gates_by_lang;
    for (Lang l : {Lang::CFamily, Lang::JsLike, Lang::Go, Lang::Rust,
                   Lang::Python, Lang::Shell, Lang::Ruby, Lang::Lua}) {
        int key = static_cast<int>(l);
        auto compiled = compile_pattern_tree(a.pattern, l);
        gates_by_lang[key] = literal_gates(compiled);
        pat_by_lang[key] = std::move(compiled);
    }

    std::ostringstream out;
    if (!a.display_description.empty()) out << a.display_description << "\n";
    std::size_t total_repl = 0, files_changed = 0;
    bool truncated = false;
    std::optional<FileChange> change;   // carried for single-file applies

    for (const auto& p : files) {
        if (total_repl >= kMaxMatches) { truncated = true; break; }
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (ec || sz == 0 || sz > kMaxFileBytes) continue;
        if (util::is_binary_file(p)) continue;
        std::string bytes = util::read_file(p);
        if (bytes.empty()) continue;

        Lang lang = lang_of(p);
        int key = static_cast<int>(lang);
        if (!passes_prefilter(bytes, gates_by_lang[key])) continue;

        auto toks = tokenize(bytes, lang);
        auto tree = build_tree(toks);
        auto hits = match_file(pat_by_lang[key], tree);
        if (hits.empty()) continue;

        // Drop overlapping matches: hits are sorted by beg (outermost kept
        // first at equal beg). Keep a hit only if it starts at/after the
        // previous kept hit's end — splicing nested matches would corrupt.
        std::vector<const Hit*> kept;
        std::size_t last_end = 0;
        for (const auto& h : hits) {
            if (h.beg < last_end) continue;
            if (h.end > bytes.size() || h.beg >= h.end) continue;   // paranoia
            kept.push_back(&h);
            last_end = h.end;
        }
        if (kept.empty()) continue;

        // Render replacements; a template var unbound for THIS hit (possible
        // when '?'-optional semantics arrive; today pattern_vars guarantees
        // it) drops the hit rather than corrupting.
        std::string rebuilt;
        rebuilt.reserve(bytes.size());
        std::size_t cursor = 0;
        std::size_t applied_here = 0;
        std::error_code rec;
        auto rel = fs::relative(p, wp->path(), rec);
        std::string relname = rec ? p.string() : rel.generic_string();

        for (const Hit* h : kept) {
            auto rendered = render_template(tpl, h->binds, bytes);
            if (!rendered) continue;
            if (total_repl >= kMaxMatches) { truncated = true; break; }
            if (applied_here == 0) out << "\n" << relname << ":\n";
            // Preview: the before/after of the spliced region, single-line
            // clipped — enough to eyeball correctness without a full diff.
            auto clip = [](std::string s) {
                for (auto& c : s) if (c == '\n') c = ' ';
                if (s.size() > 120) { s.resize(119); s += "\xe2\x80\xa6"; }
                return s;
            };
            out << "  L" << h->line << ": - "
                << clip(bytes.substr(h->beg, h->end - h->beg)) << "\n"
                << "  L" << h->line << ": + " << clip(*rendered) << "\n";
            rebuilt.append(bytes, cursor, h->beg - cursor);
            rebuilt += *rendered;
            cursor = h->end;
            ++applied_here;
            ++total_repl;
        }
        if (applied_here == 0) continue;
        rebuilt.append(bytes, cursor, bytes.size() - cursor);
        ++files_changed;

        if (a.apply) {
            if (auto err = util::write_file(p, rebuilt); !err.empty())
                return std::unexpected(ToolError::io(
                    "rewrite_structural: " + err));
            // Surface the LAST file's change for the host diff-review UI
            // (single-file rewrites — the common case — get a full diff).
            int added = 0, removed = 0;
            {
                std::istringstream ib(bytes), ir(rebuilt);
                std::size_t nb = 0, nr = 0;
                for (std::string l; std::getline(ib, l);) ++nb;
                for (std::string l; std::getline(ir, l);) ++nr;
                added   = static_cast<int>(nr > nb ? nr - nb : 0);
                removed = static_cast<int>(nb > nr ? nb - nr : 0);
            }
            change = FileChange{relname, added, removed, bytes, rebuilt};
        }
        if (out.tellp() > static_cast<std::streamoff>(kMaxOutBytes)) {
            truncated = true;
            break;
        }
    }

    if (total_repl == 0)
        return ToolOutput{
            "no structural matches — nothing to rewrite. Run "
            "search_structural with the same pattern to inspect.",
            std::nullopt};

    std::ostringstream head;
    head << (a.apply ? "Rewrote " : "DRY RUN — would rewrite ")
         << total_repl << " occurrence" << (total_repl == 1 ? "" : "s")
         << " across " << files_changed << " file"
         << (files_changed == 1 ? "" : "s") << "."
         << (a.apply ? "" : " Re-run with apply:true to write.") << "\n";
    if (truncated)
        head << "[truncated at " << kMaxMatches << " occurrences — narrow "
                "the pattern or path and repeat]\n";
    return ToolOutput{head.str() + out.str(),
                      files_changed == 1 ? std::move(change) : std::nullopt};
}

json structural_schema() {
    return json{
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description",
                 "A code SHAPE to match on a nested-document model (like "
                 "Semgrep generic / ast-grep), tokenized language-aware so it "
                 "NEVER matches inside comments or string literals. "
                 "Metavariables: $NAME matches exactly ONE node — one "
                 "identifier/literal OR one balanced (…)/[…]/{…} group — and "
                 "binds it (reuse $NAME to require the same text); $$$NAME "
                 "matches a run of ZERO OR MORE nodes (e.g. an argument list or "
                 "a multi-token condition). $_ / $$$ are anonymous. So "
                 "`malloc($N)` matches malloc(n) but NOT malloc(a*b) (2 nodes) "
                 "— use `malloc($$$)` for that. Examples: 'foo($$$)' finds every "
                 "foo call; 'if ($$$C) return $X;' finds guarded returns; "
                 "'$X == $X' finds self-comparisons; 'catch ($$$) {}' finds "
                 "empty catch blocks. Nested calls match too (the matcher "
                 "recurses into groups)."}
            }},
            {"path", {{"type", "string"},
                      {"description", "Directory to search (default: workspace root)."}}},
            {"glob", {{"type", "string"},
                      {"description", "Optional filename glob filter, e.g. '*.ts'."}}},
            {"expand", {{"type", "boolean"},
                      {"description",
                       "When true, return the WHOLE enclosing function/block "
                       "around each match instead of ±1 context line — so the "
                       "result is self-sufficient and you rarely need a "
                       "follow-up `read`. Default false."}}},
            {"display_description", {{"type", "string"},
                      {"description", "One-line summary shown in the UI. Optional."}}},
        }},
        {"required", json::array({"pattern"})},
    };
}

json rewrite_schema() {
    return json{
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description",
                 "The code SHAPE to find — same language as search_structural "
                 "($NAME binds one node, $$$NAME binds a run, back-refs must "
                 "match). Every match's exact byte span is replaced."}
            }},
            {"rewrite", {
                {"type", "string"},
                {"description",
                 "Replacement template. $NAME / $$$NAME splice the VERBATIM "
                 "source bytes that metavariable captured (original spacing "
                 "kept). Example: pattern 'assertEquals($A, $B)' rewrite "
                 "'assertEqual($B, $A)' swaps the arguments at every call "
                 "site. Every $NAME in the template must be bound by the "
                 "pattern."}
            }},
            {"path", {{"type", "string"},
                      {"description", "Directory to rewrite under (default: workspace root)."}}},
            {"glob", {{"type", "string"},
                      {"description", "Optional filename glob filter, e.g. '*.py'."}}},
            {"apply", {{"type", "boolean"},
                      {"description",
                       "false (default) = DRY RUN: show every before/after "
                       "pair without touching disk. true = write the files "
                       "(atomic per file). ALWAYS dry-run first and read the "
                       "preview before applying."}}},
            {"display_description", {{"type", "string"},
                      {"description", "One-line summary shown in the UI. Optional."}}},
        }},
        {"required", json::array({"pattern", "rewrite"})},
    };
}

} // namespace

void register_structural_tools(Shells& sh,
                               const std::shared_ptr<DocRetriever>& sem) {
    sh.add("search_structural",
           "Structural (AST-shape) code search — the layer between `grep` "
           "(text) and `search_code` (meaning). Matches a code SHAPE with "
           "metavariables, NEVER matching inside comments or string literals — "
           "so it has none of grep's false positives from log strings, "
           "docstrings, or same-named-but-unrelated tokens. Reach for it when "
           "the query is a code PATTERN, not a literal string: all calls to a "
           "function regardless of arguments (`foo($$$)`), a specific control-"
           "flow shape (`if ($C) return $X;`), empty catch blocks "
           "(`catch ($$$) {}`), self-assignments (`$X = $X`). For an exact "
           "string/identifier use `grep`; for a concept you can't name use "
           "`search_code`. When the code index is available, zero hits come "
           "back with semantically-nearest leads (verified against disk), and "
           "over-budget result sets render the most task-relevant files first.",
           structural_schema(),
           EffectSet{Effect::ReadFs},
           [sem](const Json& j) -> mcp::cap::Result {
               auto parsed = parse_args(j);
               if (!parsed) return mcp::cap::Result::error(parsed.error().render());
               return lower(run_structural(*parsed, sem.get()));
           },
           /*token_budget=*/30'000);

    sh.add("rewrite_structural",
           "Structural find-and-replace — the search_structural pattern "
           "language as a REFACTORING engine. Matches a code shape and "
           "replaces every occurrence with a template, splicing each "
           "metavariable's VERBATIM captured source back in: pattern "
           "'assertEquals($A, $B)' + rewrite 'assertEqual($B, $A)' swaps "
           "arguments at every call site in one call. Token-precise byte "
           "spans mean comments and string literals are never touched — "
           "unlike sed/regex replace. Back-references constrain matches "
           "('$X = $X' finds only self-assignments). Defaults to a DRY-RUN "
           "preview of every before/after pair; re-run with apply:true to "
           "write. Use `edit` for one-off changes; use THIS for the same "
           "shape change across many sites.",
           rewrite_schema(),
           EffectSet{Effect::ReadFs, Effect::WriteFs},
           body<RewriteArgs>(run_rewrite, parse_rewrite_args),
           /*token_budget=*/30'000);
}

} // namespace mcp::tools::detail
