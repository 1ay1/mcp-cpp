// SPDX-License-Identifier: Apache-2.0
//
// shape.cpp — lexer, bracket tree and metavariable matcher (see shape.hpp).
#include "shape.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::tools::detail::shape {

namespace fs = std::filesystem;

// Language families we tokenize. The extension decides comment/string rules;
// an unknown extension falls back to the C-family scanner (block + // + "…"),
// which is a safe superset for most curly-brace languages. The split matters:
// in C/Go/Rust `'…'` is a bounded CHAR literal (and in Rust an unpaired `'`
// is a lifetime), while in JS/Python/Shell/Ruby it's a full string; Go
// backticks are raw (no escapes) while JS template literals escape; Rust has
// r#"…"# raw strings and NESTED block comments. Conflating these corrupts
// bracket balance for whole files.

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

static bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$'; }
static bool is_ident_cont(unsigned char c)  { return std::isalnum(c) || c == '_' || c == '$'; }

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

// Classify a pattern Ident token as a metavariable. Forms:
//   $NAME      → One,  bind=NAME
//   $$$NAME    → Many, bind=NAME
//   $_         → One,  anonymous
//   $$$        → Many, anonymous
static std::optional<std::pair<MetaKind, std::string>> meta_of(std::string_view t) {
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


constexpr char close_for(char o) {
    return o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : 0;
}
static bool is_open(const Token& t) {
    return t.kind == Tok::Punct && t.text.size() == 1 &&
           (t.text[0] == '(' || t.text[0] == '[' || t.text[0] == '{');
}
static bool is_close(const Token& t) {
    return t.kind == Tok::Punct && t.text.size() == 1 &&
           (t.text[0] == ')' || t.text[0] == ']' || t.text[0] == '}');
}

// Fold [begin,end) tokens into a sibling list of Nodes, recursing on brackets.
// `i` advances by reference. Stops at a close bracket matching `expect_close`
// (consuming it) or at end. Comments are dropped (layout-insensitive match).
// `depth` caps the recursion: a pathological/generated file with tens of
// thousands of nested brackets would otherwise blow the C++ stack — an
// UNCATCHABLE crash. Past the cap, openers degrade to plain atoms (the match
// still works textually inside the flattened region; only nesting precision
// is lost, and only in a file no human wrote).
constexpr int kMaxBracketDepth = 1500;
static std::vector<Node> build_nodes(const std::vector<Token>& toks, std::size_t& i,
                              char expect_close, int depth = 0) {
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
        if (is_open(t) && depth < kMaxBracketDepth) {
            Node g; g.kind = NodeKind::Group; g.tok = t; g.open = t.text[0];
            g.line = t.line;
            g.beg  = t.pos;
            ++i;
            g.kids = build_nodes(toks, i, close_for(t.text[0]), depth + 1);
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


static std::vector<PNode> build_pattern_nodes(const std::vector<Token>& toks,
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
static void node_text(const Node& n, std::string& out) {
    if (n.kind == NodeKind::Atom) { out += n.tok.text; return; }
    out += n.open;
    for (auto& k : n.kids) node_text(k, out);
    if (char c = close_for(n.open)) out += c;
}
static std::string nodes_text(const std::vector<Node>& ns, std::size_t a, std::size_t b) {
    std::string s;
    for (std::size_t k = a; k < b; ++k) node_text(ns[k], s);
    return s;
}

// A metavariable binding: `norm` is the whitespace-normalized token render
// (used for $X == $X back-reference COMPARISON — '64 * sz' and '64*sz' are
// the same capture); [beg,end) is the VERBATIM byte span in the source file
// (used by rewrite to splice the original bytes, preserving spacing).

static bool atoms_equal(const Token& a, const Token& b) {
    return a.kind == b.kind && a.text == b.text;
}

// Match pattern sibling-sequence pat[pi..] against doc siblings doc[di..].
// On success returns true and sets di_end to the doc index just past the match.
static bool match_seq(const std::vector<PNode>& pat, std::size_t pi,
               const std::vector<Node>& doc, std::size_t di,
               Binds& binds, std::size_t& di_end, std::size_t& steps);

// Global per-file match-operation budget. The `$$$` (MetaKind::Many) matcher
// backtracks over every split point and recurses on the pattern remainder, so
// a pattern with several `$$$` against a long sibling run backtracks
// EXPONENTIALLY (measured ~10× per added `$$$`) — a ReDoS-class DoS reachable
// from one search_structural/rewrite_structural call, since the file-size cap
// (2 MiB) still admits tens of thousands of tokens. We charge every match_seq
// / match_node step against this budget and abort the match (as "no match")
// once it's spent, bounding total work per file to O(kMaxMatchSteps)
// regardless of how adversarial the pattern/document are. Legitimate shapes
// resolve in a few thousand steps; the ceiling is ~1000× that.
constexpr std::size_t kMaxMatchSteps = 500'000;

// Does pattern node `p` match a single doc node `d` (one-to-one)?
static bool match_node(const PNode& p, const Node& d, Binds& binds, std::size_t& steps) {
    if (++steps > kMaxMatchSteps) return false;
    if (p.kind == NodeKind::Group) {
        if (d.kind != NodeKind::Group || d.open != p.open) return false;
        std::size_t end = 0;
        Binds trial = binds;
        if (!match_seq(p.kids, 0, d.kids, 0, trial, end, steps)) return false;
        if (end != d.kids.size()) return false;   // group children fully consumed
        binds = std::move(trial);
        return true;
    }
    // p is an Atom (literal, since metavars are handled in match_seq).
    if (d.kind != NodeKind::Atom) return false;
    return atoms_equal(p.tok, d.tok);
}

static bool match_seq(const std::vector<PNode>& pat, std::size_t pi,
               const std::vector<Node>& doc, std::size_t di,
               Binds& binds, std::size_t& di_end, std::size_t& steps) {
    while (pi < pat.size()) {
        if (++steps > kMaxMatchSteps) return false;
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
                if (ok && match_seq(pat, pi + 1, doc, end, trial, sub_end, steps)) {
                    binds = std::move(trial);
                    di_end = sub_end;
                    return true;
                }
            }
            return false;
        }

        // Literal atom or group.
        if (di >= doc.size()) return false;
        if (!match_node(p, doc[di], binds, steps)) return false;
        ++pi; ++di;
    }
    di_end = di;
    return true;
}


// Try the pattern sequence starting at each sibling position, recursively into
// groups, collecting hit start-lines. The pattern must match a CONTIGUOUS run
// of siblings beginning at that position.
static void match_in_scope(const std::vector<PNode>& pat,
                    const std::vector<Node>& doc,
                    std::vector<Hit>& hits,
                    std::size_t& steps) {
    for (std::size_t i = 0; i < doc.size(); ++i) {
        if (steps > kMaxMatchSteps) return;   // budget spent for this file
        Binds binds;
        std::size_t end = 0;
        if (match_seq(pat, 0, doc, i, binds, end, steps) && end > i) {
            hits.push_back({doc[i].line, doc[i].beg, doc[end-1].end,
                            std::move(binds)});
        }
        // Recurse into this node's group children so nested calls match too.
        if (doc[i].kind == NodeKind::Group)
            match_in_scope(pat, doc[i].kids, hits, steps);
    }
}

std::vector<Hit> match_file(const std::vector<PNode>& pat,
                            const std::vector<Node>& tree) {
    std::vector<Hit> hits;
    if (pat.empty() || tree.empty()) return hits;
    // One match-op budget for the ENTIRE file scan (shared across every start
    // position and nested scope), so an adversarial pattern can't multiply the
    // per-position cost by the sibling count. When it trips we simply stop
    // finding more hits in this file — sound (never a false positive) and
    // bounded.
    std::size_t steps = 0;
    match_in_scope(pat, tree, hits, steps);
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

}  // namespace mcp::tools::detail::shape
