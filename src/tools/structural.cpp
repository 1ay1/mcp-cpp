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

#include "tool_shell.hpp"
#include "tool_body.hpp"

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;

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
// which is a safe superset for most curly-brace languages.
enum class Lang { CFamily, Python, Shell, Ruby, Lua, Unknown };

Lang lang_of(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    if (ext == ".py" || ext == ".pyi") return Lang::Python;
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh") return Lang::Shell;
    if (ext == ".rb") return Lang::Ruby;
    if (ext == ".lua") return Lang::Lua;
    static const char* kC[] = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx", ".ino",
        ".js", ".ts", ".jsx", ".tsx", ".mjs", ".cjs",
        ".go", ".rs", ".java", ".kt", ".kts", ".swift", ".zig", ".cs",
        ".scala", ".dart", ".php", ".m", ".mm", ".proto",
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

    const bool c_family   = (lang == Lang::CFamily || lang == Lang::Unknown);
    const bool hash_line  = (lang == Lang::Python || lang == Lang::Shell || lang == Lang::Ruby);
    const bool lua_cmt    = (lang == Lang::Lua);

    auto count_newlines = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to; ++k) if (s[k] == '\n') ++line;
    };

    while (i < n) {
        char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }

        // Line comments.
        if (c_family && c == '/' && i + 1 < n && s[i+1] == '/') {
            std::size_t b = i; while (i < n && s[i] != '\n') ++i;
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line});
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
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line});
            continue;
        }
        if (hash_line && c == '#') {
            std::size_t b = i; while (i < n && s[i] != '\n') ++i;
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, line});
            continue;
        }
        // Block comments (C-family).
        if (c_family && c == '/' && i + 1 < n && s[i+1] == '*') {
            std::size_t b = i; int start_line = line; i += 2;
            std::size_t body = i;
            while (i + 1 < n && !(s[i] == '*' && s[i+1] == '/')) ++i;
            count_newlines(body, i);
            if (i + 1 < n) i += 2; else i = n;
            out.push_back({Tok::Comment, std::string{s.substr(b, i - b)}, start_line});
            continue;
        }

        // Strings — triple-quoted first (Python), then single/double, then char.
        auto scan_quoted = [&](char q) {
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
                    if (s[i] == '\\' && i + 1 < n) ++i;
                    if (s[i] == '\n') ++line;   // permit multiline (JS backtick, etc.)
                    ++i;
                }
                if (i < n) ++i;   // closing quote
            }
            out.push_back({q == '\'' ? Tok::Char : Tok::String,
                           std::string{s.substr(b, i - b)}, start_line});
        };
        if (c == '"' || c == '`') { scan_quoted(c); continue; }
        if (c == '\'') {
            // In C-family, '\'' is a char literal; in Python/others treat as string.
            scan_quoted('\'');
            continue;
        }

        // C++ raw string literal: (prefix)R"delim( ... )delim" — the body is
        // fully literal (no escapes, embedded quotes/braces are just bytes), so
        // a naive ".." scan mis-parses it. Detect an R directly before a " ,
        // optionally after an encoding prefix (u8/u/U/L). Must come BEFORE the
        // identifier scan since the prefix looks like an identifier.
        if (c_family && (c == 'R' || c == 'u' || c == 'U' || c == 'L')) {
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
                    out.push_back({Tok::String, std::string{s.substr(b, end - b)}, start_line});
                    i = end;
                    continue;
                }
                // Not actually a raw string (no '(') — fall through to ident.
            }
        }

        // Numbers.
        if (std::isdigit((unsigned char)c)
            || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i+1]))) {
            std::size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.'
                             || s[i] == '_' || s[i] == 'x' || s[i] == 'X')) ++i;
            out.push_back({Tok::Number, std::string{s.substr(b, i - b)}, line});
            continue;
        }

        // Identifiers (and metavariables in the pattern, which start with '$').
        if (is_ident_start((unsigned char)c)) {
            std::size_t b = i;
            while (i < n && is_ident_cont((unsigned char)s[i])) ++i;
            out.push_back({Tok::Ident, std::string{s.substr(b, i - b)}, line});
            continue;
        }

        // Punctuation — one char per token (keeps bracket balancing simple).
        out.push_back({Tok::Punct, std::string(1, c), line});
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
            ++i;
            g.kids = build_nodes(toks, i, close_for(t.text[0]));
            // end_line: the close bracket sits at toks[i-1] when consumed;
            // otherwise fall back to the last child's end.
            g.end_line = (i > 0 && is_close(toks[i-1])) ? toks[i-1].line
                       : (!g.kids.empty() ? g.kids.back().end_line : g.line);
            if (g.end_line < g.line) g.end_line = g.line;
            out.push_back(std::move(g));
            continue;
        }
        Node a; a.kind = NodeKind::Atom; a.tok = t; a.line = t.line;
        a.end_line = t.line;
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

using Binds = std::unordered_map<std::string, std::string>;

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
                if (it != binds.end() && it->second != cap) return false;
                binds[p.bind] = cap;
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
                    if (it != trial.end() && it->second != cap) ok = false;
                    else trial[p.bind] = cap;
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

struct Hit { int line = 0; };

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
            hits.push_back({doc[i].line});
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
    // De-dup by line (a nested recursion can re-report the same start line).
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.line < b.line; });
    hits.erase(std::unique(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.line == b.line; }),
              hits.end());
    return hits;
}

// ── Enclosing-symbol heuristic (mirror of search.cpp's for consistency) ──
std::string enclosing_symbol(const std::vector<std::string>& lines, int hit_line) {
    static const char* kw[] = {
        "class ", "struct ", "def ", "fn ", "func ", "function ",
        "impl ", "trait ", "namespace ", "interface ", "module ",
    };
    for (int ln = hit_line - 1; ln >= 0 && ln > hit_line - 400; --ln) {
        if (ln >= static_cast<int>(lines.size())) continue;
        const std::string& s = lines[ln];
        std::size_t indent = s.find_first_not_of(" \t");
        if (indent == std::string::npos) continue;
        for (auto* k : kw) {
            auto pos = s.find(k);
            if (pos != std::string::npos && pos <= indent + 8) {
                std::string trimmed = s.substr(indent);
                if (trimmed.size() > 90) trimmed.resize(90);
                return trimmed;
            }
        }
    }
    return {};
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
            if (c == '"' || c == '\'' || c == '`') { in_str = true; q = c; continue; }
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

ExecResult run_structural(const StructArgs& a) {
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
    for (Lang l : {Lang::CFamily, Lang::Python, Lang::Shell, Lang::Ruby, Lang::Lua})
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

    if (match_count == 0)
        return ToolOutput{
            "no structural matches. The pattern must match the code SHAPE "
            "token-for-token (metavariables $X / $$$X match one / many). If you "
            "wanted a text match, use `grep` instead.",
            std::nullopt};

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
                // Prefer the tree (exact group span); fall back to the line
                // scan for indent-scoped langs with no covering brace group.
                if (auto sc = scope_from_tree(file_tree, h.line)) {
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

} // namespace

void register_structural_tools(Shells& sh) {
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
           "`search_code`.",
           structural_schema(),
           EffectSet{Effect::ReadFs},
           body<StructArgs>(run_structural, parse_args),
           /*token_budget=*/30'000);
}

} // namespace mcp::tools::detail
