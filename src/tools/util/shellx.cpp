// SPDX-License-Identifier: Apache-2.0
//
// shellx — parse (tree-sitter-bash) → lower to a flat typed Script → guard +
// action translation. See the header for the model. Organisation:
//   1. parser + node helpers
//   2. words: tree-sitter word nodes → Lit | Dyn
//   3. lowering: statements → Commands with context, joins, pipelines
//   4. unwrapping: sudo/env/timeout/…, bash -c '…', xargs, find -exec
//   5. guard
//   6. actions (plan)
#include <mcp/tools/util/shellx.hpp>

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

extern "C" const TSLanguage* tree_sitter_bash();

namespace mcp::tools::util::shellx {

namespace {

// ═════════════════════════════════════════════════════════════════════════
//  1. Parser + node helpers
// ═════════════════════════════════════════════════════════════════════════

// Bounds. A command larger than this is not an agent shell-out anymore; we
// still guard the prefix (and the raw text) but don't translate.
constexpr std::size_t kMaxSource   = 1u << 20;  // 1 MiB
constexpr int         kMaxDepth    = 256;       // AST recursion
constexpr int         kMaxNesting  = 8;         // bash -c inside bash -c …
constexpr std::size_t kMaxCommands = 4096;

struct ParserDeleter { void operator()(TSParser* p) const noexcept { ts_parser_delete(p); } };
struct TreeDeleter   { void operator()(TSTree* t)   const noexcept { ts_tree_delete(t); } };
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr   = std::unique_ptr<TSTree, TreeDeleter>;

// One parser per thread: construction is ~10 us, parse is ~20 us — reuse it.
TSParser& thread_parser() {
    thread_local ParserPtr p = [] {
        ParserPtr q{ts_parser_new()};
        ts_parser_set_language(q.get(), tree_sitter_bash());
        return q;
    }();
    ts_parser_reset(p.get());
    return *p;
}

// Node type → small enum, once per node. Comparing C strings at every visit
// is the single hottest thing a naive walker does.
enum class K : std::uint8_t {
    Other, Program, Command, CommandName, Pipeline, List, Redirected, Subshell,
    Compound, If, Elif, Else, While, For, CFor, Case, CaseItem, DoGroup,
    Function, Negated, VarAssign, VarAssigns, Declaration, Unset, Test,
    CmdSubst, ProcSubst, FileRedirect, HeredocRedirect, HerestringRedirect,
    HeredocBody, Word, String, RawString, AnsiCString, TranslatedString,
    Concat, SimpleExp, Expansion, ArithExp, Brace, Number, StringContent,
    FileDescriptor, Comment, VariableName, ExtGlob, Regex,
};

K kind_of(TSNode n) noexcept {
    // Symbol ids are stable per language; build the table once.
    static const auto table = [] {
        const TSLanguage* L = tree_sitter_bash();
        const std::uint32_t count = ts_language_symbol_count(L);
        std::vector<K> t(count, K::Other);
        static constexpr std::pair<std::string_view, K> names[] = {
            {"program", K::Program}, {"command", K::Command}, {"command_name", K::CommandName},
            {"pipeline", K::Pipeline}, {"list", K::List},
            {"redirected_statement", K::Redirected}, {"subshell", K::Subshell},
            {"compound_statement", K::Compound}, {"if_statement", K::If},
            {"elif_clause", K::Elif}, {"else_clause", K::Else},
            {"while_statement", K::While}, {"for_statement", K::For},
            {"c_style_for_statement", K::CFor}, {"case_statement", K::Case},
            {"case_item", K::CaseItem}, {"do_group", K::DoGroup},
            {"function_definition", K::Function}, {"negated_command", K::Negated},
            {"variable_assignment", K::VarAssign}, {"variable_assignments", K::VarAssigns},
            {"declaration_command", K::Declaration}, {"unset_command", K::Unset},
            {"test_command", K::Test}, {"command_substitution", K::CmdSubst},
            {"process_substitution", K::ProcSubst}, {"file_redirect", K::FileRedirect},
            {"heredoc_redirect", K::HeredocRedirect},
            {"herestring_redirect", K::HerestringRedirect},
            {"heredoc_body", K::HeredocBody}, {"word", K::Word}, {"string", K::String},
            {"raw_string", K::RawString}, {"ansi_c_string", K::AnsiCString},
            {"translated_string", K::TranslatedString}, {"concatenation", K::Concat},
            {"simple_expansion", K::SimpleExp}, {"expansion", K::Expansion},
            {"arithmetic_expansion", K::ArithExp}, {"brace_expression", K::Brace},
            {"number", K::Number}, {"string_content", K::StringContent},
            {"file_descriptor", K::FileDescriptor}, {"comment", K::Comment},
            {"variable_name", K::VariableName}, {"extglob_pattern", K::ExtGlob},
            {"regex", K::Regex},
        };
        for (const auto& [name, k] : names) {
            const TSSymbol s = ts_language_symbol_for_name(
                L, name.data(), static_cast<std::uint32_t>(name.size()), /*is_named=*/true);
            if (s != 0 && s < count) t[s] = k;
        }
        return t;
    }();
    const TSSymbol s = ts_node_symbol(n);
    return s < table.size() ? table[s] : K::Other;
}

struct Src {
    std::string_view text;
    [[nodiscard]] std::string_view of(TSNode n) const noexcept {
        const auto b = ts_node_start_byte(n), e = ts_node_end_byte(n);
        if (b > text.size() || e > text.size() || e < b) return {};
        return text.substr(b, e - b);
    }
};

template <class F>
void for_named(TSNode n, F&& f) {
    const std::uint32_t c = ts_node_named_child_count(n);
    for (std::uint32_t i = 0; i < c; ++i) f(ts_node_named_child(n, i));
}

// ═════════════════════════════════════════════════════════════════════════
//  2. Words
// ═════════════════════════════════════════════════════════════════════════
//
// Evaluate a word node to its static value when it has one. Rules follow
// POSIX/bash quoting: '…' is verbatim; "…" is verbatim except \$ \` \" \\ and
// \newline, and any expansion inside makes it Dyn; bare words drop backslashes
// before any char. Globs and a leading ~ are Dyn because their value depends
// on the filesystem / environment even though they look literal.

struct WordEval {
    std::string value;
    bool dynamic = false;
    Dyn::Why why = Dyn::Why::Variable;
    void dyn(Dyn::Why w) noexcept { if (!dynamic) { dynamic = true; why = w; } }
};

bool has_glob_meta(std::string_view s) noexcept {
    // Unquoted * ? [ make the word a pattern (bash filename expansion).
    return s.find_first_of("*?[") != std::string_view::npos;
}

void unescape_bare(std::string_view s, std::string& out) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == '\n') { ++i; continue; }   // line continuation
            out += s[++i];
        } else {
            out += s[i];
        }
    }
}

void unescape_dquote(std::string_view s, std::string& out) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[i + 1];
            if (c == '$' || c == '`' || c == '"' || c == '\\') { out += c; ++i; continue; }
            if (c == '\n') { ++i; continue; }
        }
        out += s[i];
    }
}

// $'…' — ANSI-C quoting. Enough of it to get a literal right; anything exotic
// (\cX, \u with invalid code point) we just keep verbatim, which only matters
// for display.
void unescape_ansi_c(std::string_view s, std::string& out) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char c = s[++i];
        switch (c) {
            case 'n': out += '\n'; break;  case 't': out += '\t'; break;
            case 'r': out += '\r'; break;  case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;  case 'e': case 'E': out += '\x1b'; break;
            case 'f': out += '\f'; break;  case 'v': out += '\v'; break;
            case '\\': out += '\\'; break; case '\'': out += '\''; break;
            case '"': out += '"'; break;   case '?': out += '?'; break;
            case 'x': {
                unsigned v = 0; int k = 0;
                while (k < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
                    v = v * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(s[i + 1]))
                        ? s[i + 1] - '0' : (std::tolower(static_cast<unsigned char>(s[i + 1])) - 'a' + 10));
                    ++i; ++k;
                }
                out += static_cast<char>(v);
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned v = static_cast<unsigned>(c - '0'); int k = 1;
                    while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
                        v = v * 8 + static_cast<unsigned>(s[++i] - '0'); ++k;
                    }
                    out += static_cast<char>(v);
                } else {
                    out += '\\'; out += c;
                }
        }
    }
}

void eval_word(TSNode n, const Src& src, WordEval& w, bool first_piece);

void eval_children(TSNode n, const Src& src, WordEval& w) {
    bool first = true;
    for_named(n, [&](TSNode c) { eval_word(c, src, w, first); first = false; });
}

void eval_word(TSNode n, const Src& src, WordEval& w, bool first_piece) {
    const std::string_view t = src.of(n);
    switch (kind_of(n)) {
        case K::Word:
        case K::CommandName:
        case K::Number:
        case K::VariableName: {
            if (kind_of(n) == K::CommandName) {        // wrapper around the real word
                eval_children(n, src, w);
                return;
            }
            if (kind_of(n) == K::Number && ts_node_named_child_count(n) > 0) {
                eval_children(n, src, w);
                return;
            }
            if (first_piece && !t.empty() && t.front() == '~') w.dyn(Dyn::Why::Tilde);
            // Unquoted glob chars and brace lists expand; escaped ones don't.
            std::string raw;
            unescape_bare(t, raw);
            bool meta = false;
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (t[i] == '\\') { ++i; continue; }
                if (t[i] == '*' || t[i] == '?' || t[i] == '[') meta = true;
            }
            if (meta) w.dyn(Dyn::Why::Glob);
            w.value += raw;
            return;
        }
        case K::RawString:                 // '…'
            if (t.size() >= 2) w.value.append(t.substr(1, t.size() - 2));
            return;
        case K::AnsiCString:               // $'…'
            if (t.size() >= 3) unescape_ansi_c(t.substr(2, t.size() - 3), w.value);
            return;
        case K::TranslatedString:          // $"…" — locale-translated; treat as its string
            eval_children(n, src, w);
            return;
        case K::String: {                  // "…" with possible expansions inside
            // Walk the raw text between the quotes, splicing children: literal
            // stretches are unescaped, expansions make the word dynamic.
            if (t.size() < 2) return;
            const std::uint32_t base = ts_node_start_byte(n);
            std::uint32_t cursor = base + 1;
            const std::uint32_t stop = ts_node_end_byte(n) - 1;
            for_named(n, [&](TSNode c) {
                const auto cb = ts_node_start_byte(c), ce = ts_node_end_byte(c);
                if (cb > cursor) unescape_dquote(src.text.substr(cursor, cb - cursor), w.value);
                if (kind_of(c) == K::StringContent)
                    unescape_dquote(src.of(c), w.value);
                else
                    eval_word(c, src, w, false);   // expansions → dyn
                cursor = ce;
            });
            if (stop > cursor) unescape_dquote(src.text.substr(cursor, stop - cursor), w.value);
            return;
        }
        case K::StringContent:
            unescape_dquote(t, w.value);
            return;
        case K::Concat:
            eval_children(n, src, w);
            return;
        case K::SimpleExp:
        case K::Expansion:
            w.dyn(Dyn::Why::Variable);
            w.value.append(t);
            return;
        case K::CmdSubst:
        case K::ProcSubst:
            w.dyn(Dyn::Why::Substitution);
            w.value.append(t);
            return;
        case K::ArithExp:
            w.dyn(Dyn::Why::Arithmetic);
            w.value.append(t);
            return;
        case K::Brace:
            w.dyn(Dyn::Why::Brace);
            w.value.append(t);
            return;
        case K::ExtGlob:
            w.dyn(Dyn::Why::Glob);
            w.value.append(t);
            return;
        default:
            // Unknown word-ish node (regex in [[ =~ ]], array, …). Be honest.
            w.dyn(Dyn::Why::Variable);
            w.value.append(t);
            return;
    }
}

Word make_word(TSNode n, const Src& src) {
    WordEval w;
    eval_word(n, src, w, /*first_piece=*/true);
    // Brace expansion inside a bare word: `{a,b}` / `{1..3}` that the grammar
    // left as one `word` (it does, outside of brace_expression contexts).
    if (!w.dynamic && kind_of(n) == K::Word) {
        const auto t = src.of(n);
        const auto ob = t.find('{');
        if (ob != std::string_view::npos && t.find('}', ob) != std::string_view::npos
            && (t.find(',', ob) != std::string_view::npos || t.find("..", ob) != std::string_view::npos))
            w.dyn(Dyn::Why::Brace);
    }
    if (w.dynamic) return Dyn{std::string{src.of(n)}, w.why};
    return Lit{std::move(w.value)};
}

// ═════════════════════════════════════════════════════════════════════════
//  3. Lowering
// ═════════════════════════════════════════════════════════════════════════

struct Lowerer {
    Src src;
    Script& out;
    int next_pipeline = 0;
    bool truncated = false;

    void add(Command&& c) {
        if (out.commands.size() >= kMaxCommands) { truncated = true; return; }
        out.commands.push_back(std::move(c));
    }

    Redirect make_redirect(TSNode r) {
        Redirect red;
        const std::string_view t = src.of(r);
        // fd prefix
        if (TSNode d = ts_node_child_by_field_name(r, "descriptor", 10); !ts_node_is_null(d)) {
            int v = -1;
            const auto s = src.of(d);
            std::from_chars(s.data(), s.data() + s.size(), v);
            red.fd = v;
        }
        switch (kind_of(r)) {
            case K::HeredocRedirect:    red.kind = Redirect::Kind::HereDoc; break;
            case K::HerestringRedirect: red.kind = Redirect::Kind::HereString; break;
            default: {
                // operator = the anonymous token(s) before destination
                // (the named file_descriptor child `2` in `2>` is not part of it)
                std::string op;
                const std::uint32_t cc = ts_node_child_count(r);
                for (std::uint32_t i = 0; i < cc; ++i) {
                    TSNode c = ts_node_child(r, i);
                    if (ts_node_is_named(c)) {
                        if (kind_of(c) == K::FileDescriptor) {
                            int v = -1;
                            const auto s = src.of(c);
                            std::from_chars(s.data(), s.data() + s.size(), v);
                            red.fd = v;
                        }
                        continue;
                    }
                    op += src.of(c);
                }
                if (op == "<" || op == "<>")              red.kind = Redirect::Kind::In;
                else if (op == ">>")                       red.kind = Redirect::Kind::Append;
                else if (op == "&>" || op == "&>>")       red.kind = Redirect::Kind::OutErr;
                else if (op == ">&" || op == "<&" || op == ">&-" || op == "<&-")
                                                           red.kind = Redirect::Kind::DupFd;
                else                                       red.kind = Redirect::Kind::Out;
                break;
            }
        }
        if (TSNode d = ts_node_child_by_field_name(r, "destination", 11); !ts_node_is_null(d)) {
            red.target = make_word(d, src);
            // `>&2` parses as DupFd with a number destination; `> &2` too.
            if (red.kind == Redirect::Kind::Out) {
                if (auto* v = lit(red.target); v && !v->empty()
                    && std::all_of(v->begin(), v->end(), [](char ch) { return ch >= '0' && ch <= '9'; })
                    && t.find('&') != std::string_view::npos)
                    red.kind = Redirect::Kind::DupFd;
            }
        } else if (red.kind == Redirect::Kind::HereString) {
            for_named(r, [&](TSNode c) {
                if (kind_of(c) != K::FileDescriptor) red.target = make_word(c, src);
            });
        } else if (red.kind == Redirect::Kind::HereDoc) {
            // delimiter
            for_named(r, [&](TSNode c) {
                if (ts_node_type(c) == std::string_view{"heredoc_start"})
                    red.target = Lit{std::string{src.of(c)}};
            });
        }
        return red;
    }

    // Lower one statement. `pipeline`/`stage` identify its pipeline slot.
    void stmt(TSNode n, Ctx ctx, int depth, int pipeline, int stage,
              std::vector<Redirect>* inherited = nullptr) {
        if (depth > kMaxDepth) { truncated = true; return; }
        switch (kind_of(n)) {
            case K::Program:
            case K::Compound:
            case K::DoGroup:
            case K::Else:
            case K::Elif:
            case K::CaseItem:
                seq(n, ctx | (kind_of(n) == K::Compound ? Ctx::Group : Ctx::None), depth);
                return;
            case K::List: {
                // a && b || c ; the anonymous operator between children is the join
                const std::uint32_t cc = ts_node_child_count(n);
                for (std::uint32_t i = 0; i < cc; ++i) {
                    TSNode c = ts_node_child(n, i);
                    if (!ts_node_is_named(c)) {
                        const auto op = src.of(c);
                        if (!out.commands.empty())
                            out.commands.back().join = op == "&&" ? Join::And
                                                     : op == "||" ? Join::Or : Join::Seq;
                        continue;
                    }
                    if (kind_of(c) == K::Comment) continue;
                    stmt(c, ctx, depth + 1, next_pipeline++, 0);
                }
                return;
            }
            case K::Pipeline: {
                const int id = next_pipeline++;
                int k = 0;
                const std::uint32_t cc = ts_node_child_count(n);
                for (std::uint32_t i = 0; i < cc; ++i) {
                    TSNode c = ts_node_child(n, i);
                    if (!ts_node_is_named(c)) {
                        if (!out.commands.empty() && k > 0)
                            out.commands.back().join = src.of(c) == "|&" ? Join::PipeErr : Join::Pipe;
                        continue;
                    }
                    if (kind_of(c) == K::Comment) continue;
                    stmt(c, k > 0 ? (ctx | Ctx::Piped) : ctx, depth + 1, id, k, inherited);
                    ++k;
                }
                return;
            }
            case K::Redirected: {
                std::vector<Redirect> reds;
                if (inherited) reds = *inherited;
                TSNode body{};
                bool have_body = false;
                for_named(n, [&](TSNode c) {
                    switch (kind_of(c)) {
                        case K::FileRedirect: case K::HeredocRedirect: case K::HerestringRedirect:
                            reds.push_back(make_redirect(c));
                            // heredoc_redirect may carry more redirects
                            // (`cat <<EOF > f`) and `| cmd` / `&& cmd`
                            // continuations after its start line.
                            if (kind_of(c) == K::HeredocRedirect) {
                                for_named(c, [&](TSNode cc2) {
                                    const K k2 = kind_of(cc2);
                                    if (k2 == K::FileRedirect || k2 == K::HerestringRedirect)
                                        reds.push_back(make_redirect(cc2));
                                    else if (k2 == K::Pipeline || k2 == K::Command || k2 == K::List
                                             || k2 == K::Redirected)
                                        stmt(cc2, ctx, depth + 1, next_pipeline++, 0);
                                });
                            }
                            break;
                        default:
                            if (!have_body) { body = c; have_body = true; }
                    }
                });
                if (have_body) {
                    // Where do the redirects bind? `( … ) > f`, `{ …; } > f` and
                    // `while … done < f` redirect the whole body. But tree-sitter
                    // also wraps a LIST or PIPELINE when the redirect trails its
                    // last command (`cd x && python3 - <<EOF`, `a | b > f`): in
                    // bash that redirect belongs to the last command only.
                    const K bk = kind_of(body);
                    if (bk == K::List || bk == K::Pipeline) {
                        const std::size_t before = out.commands.size();
                        stmt(body, ctx, depth + 1, pipeline, stage, inherited);
                        for (std::size_t i = out.commands.size(); i > before; --i) {
                            Command& last = out.commands[i - 1];
                            if (last.ctx == ctx || last.ctx == (ctx | Ctx::Piped)) {
                                last.redirects.insert(last.redirects.end(), reds.begin(), reds.end());
                                break;
                            }
                        }
                    } else {
                        stmt(body, ctx, depth + 1, pipeline, stage, &reds);
                    }
                }
                return;
            }
            case K::Subshell:
                seq(n, ctx | Ctx::Subshell, depth);
                return;
            case K::If: case K::While: case K::For: case K::CFor: case K::Case:
                // conditions and bodies alike: everything runs conditionally
                for_named(n, [&](TSNode c) {
                    const K k = kind_of(c);
                    if (k == K::Comment) return;
                    if (k == K::Word || k == K::String || k == K::RawString || k == K::Concat
                        || k == K::VariableName || k == K::Number || k == K::SimpleExp
                        || k == K::Expansion) {
                        // `for x in $(…)` value list / `case $x` subject: substitutions still run
                        subst_in(c, ctx | Ctx::Conditional, depth + 1);
                        return;
                    }
                    stmt(c, ctx | Ctx::Conditional, depth + 1, next_pipeline++, 0);
                });
                return;
            case K::Function:
                if (TSNode b = ts_node_child_by_field_name(n, "body", 4); !ts_node_is_null(b))
                    stmt(b, ctx | Ctx::Function, depth + 1, next_pipeline++, 0);
                return;
            case K::Negated:
                for_named(n, [&](TSNode c) { stmt(c, ctx | Ctx::Negated, depth + 1, pipeline, stage, inherited); });
                return;
            case K::VarAssign:
            case K::VarAssigns:
                // bare `x=$(…)`: the substitution runs even with no command
                subst_in(n, ctx, depth + 1);
                return;
            case K::Declaration:
            case K::Unset:
            case K::Test:
                // `export X=$(…)`, `[[ $(…) ]]`, `local y=$(…)` — run substitutions;
                // the builtin itself is harmless.
                subst_in(n, ctx, depth + 1);
                return;
            case K::Command:
                command(n, ctx, depth, pipeline, stage, inherited);
                return;
            case K::Comment:
                return;
            default:
                // ERROR recovery nodes and anything unmodelled: descend so a
                // command inside is still seen by the guard.
                for_named(n, [&](TSNode c) { stmt(c, ctx, depth + 1, pipeline, stage); });
                return;
        }
    }

    void seq(TSNode n, Ctx ctx, int depth) {
        const std::uint32_t cc = ts_node_child_count(n);
        for (std::uint32_t i = 0; i < cc; ++i) {
            TSNode c = ts_node_child(n, i);
            if (!ts_node_is_named(c)) {
                const auto op = src.of(c);
                if (op == "&" && !out.commands.empty())
                    out.commands.back().ctx = out.commands.back().ctx | Ctx::Background;
                if ((op == ";" || op == "\n" || op == "&") && !out.commands.empty()
                    && out.commands.back().join == Join::End)
                    out.commands.back().join = Join::Seq;
                continue;
            }
            if (kind_of(c) == K::Comment) continue;
            if (kind_of(c) == K::Word || kind_of(c) == K::String) {
                subst_in(c, ctx, depth + 1);       // e.g. `if` conditions as words
                continue;
            }
            stmt(c, ctx, depth + 1, next_pipeline++, 0);
        }
    }

    // Lower every command/process substitution found anywhere under `n`.
    void subst_in(TSNode n, Ctx ctx, int depth) {
        if (depth > kMaxDepth) { truncated = true; return; }
        const K k = kind_of(n);
        if (k == K::CmdSubst || k == K::ProcSubst) {
            seq(n, ctx | Ctx::Substitution, depth + 1);
            return;
        }
        for_named(n, [&](TSNode c) { subst_in(c, ctx, depth + 1); });
    }

    void command(TSNode n, Ctx ctx, int depth, int pipeline, int stage,
                 const std::vector<Redirect>* inherited) {
        Command c;
        c.ctx = ctx;
        c.pipeline = pipeline;
        c.stage = stage;
        c.begin = ts_node_start_byte(n);
        c.end = ts_node_end_byte(n);
        if (inherited) c.redirects = *inherited;
        for_named(n, [&](TSNode ch) {
            switch (kind_of(ch)) {
                case K::VarAssign: {
                    TSNode nm = ts_node_child_by_field_name(ch, "name", 4);
                    TSNode vl = ts_node_child_by_field_name(ch, "value", 5);
                    std::string name{ts_node_is_null(nm) ? std::string_view{} : src.of(nm)};
                    Word v = ts_node_is_null(vl) ? Word{Lit{}} : make_word(vl, src);
                    c.env.emplace_back(std::move(name), std::move(v));
                    if (!ts_node_is_null(vl)) subst_in(vl, ctx, depth + 1);
                    break;
                }
                case K::FileRedirect: case K::HerestringRedirect: case K::HeredocRedirect:
                    c.redirects.push_back(make_redirect(ch));
                    subst_in(ch, ctx, depth + 1);
                    break;
                case K::Subshell:
                    stmt(ch, ctx, depth + 1, next_pipeline++, 0);
                    break;
                case K::Comment:
                    break;
                default:
                    c.argv.push_back(make_word(ch, src));
                    subst_in(ch, ctx, depth + 1);   // $(…) inside args runs first
                    break;
            }
        });
        if (!c.argv.empty() || !c.env.empty() || !c.redirects.empty()) add(std::move(c));
    }
};

// ═════════════════════════════════════════════════════════════════════════
//  4. Unwrapping
// ═════════════════════════════════════════════════════════════════════════
//
// Commands that run ANOTHER command. The guard must see the inner command as
// if written directly. Each rule returns the inner argv (and, for `sh -c`,
// a script to re-analyze). Applied transitively up to kMaxNesting.

std::string_view basename(std::string_view p) noexcept {
    const auto s = p.find_last_of('/');
    return s == std::string_view::npos ? p : p.substr(s + 1);
}

bool is_shell(std::string_view prog) noexcept {
    static constexpr std::string_view k[] = {"sh", "bash", "zsh", "dash", "ksh", "ash", "busybox"};
    return std::ranges::find(k, prog) != std::end(k);
}

// Split off a known prefix wrapper; returns index of the inner program in argv
// or nullopt if `prog` is not a wrapper we understand.
std::optional<std::size_t> wrapper_skip(const std::vector<Word>& argv) {
    if (argv.empty()) return std::nullopt;
    const std::string* p0 = lit(argv[0]);
    if (!p0) return std::nullopt;
    const std::string_view prog = basename(*p0);
    std::size_t i = 1;
    auto skip_opts = [&](std::initializer_list<std::string_view> with_value) {
        while (i < argv.size()) {
            const std::string* w = lit(argv[i]);
            if (!w || w->empty() || (*w)[0] != '-') break;
            if (*w == "--") { ++i; break; }
            const bool takes = std::ranges::find(with_value, *w) != with_value.end();
            i += takes ? 2 : 1;
        }
    };
    if (prog == "sudo" || prog == "doas") {
        skip_opts({"-u", "-g", "-h", "-p", "-C", "-D", "-R", "-T", "-U", "-r", "-t"});
    } else if (prog == "env") {
        skip_opts({"-u", "-C", "-S", "--unset", "--chdir"});
        while (i < argv.size()) {                       // NAME=value operands
            const std::string* w = lit(argv[i]);
            if (!w || w->find('=') == std::string::npos || (*w)[0] == '=') break;
            ++i;
        }
    } else if (prog == "command" || prog == "builtin" || prog == "exec") {
        // `command -v x` / `-V x` only LOOKS UP x; nothing to unwrap.
        for (std::size_t k = 1; k < argv.size(); ++k) {
            const std::string* w = lit(argv[k]);
            if (!w || w->empty() || (*w)[0] != '-' || *w == "--") break;
            if (w->find('v') != std::string::npos || w->find('V') != std::string::npos)
                return std::nullopt;
        }
        skip_opts({"-a"});
    } else if (prog == "nice") {
        skip_opts({"-n", "--adjustment"});
    } else if (prog == "ionice") {
        skip_opts({"-c", "-n", "-p", "-P", "-u"});
    } else if (prog == "nohup" || prog == "time" || prog == "unbuffer" || prog == "stdbuf") {
        skip_opts({"-i", "-o", "-e"});
    } else if (prog == "timeout") {
        skip_opts({"-s", "-k", "--signal", "--kill-after"});
        if (i < argv.size()) ++i;                        // DURATION
    } else if (prog == "chroot") {
        skip_opts({"--userspec", "--groups"});
        if (i < argv.size()) ++i;                        // NEWROOT
    } else if (prog == "watch") {
        skip_opts({"-n", "-d", "--interval"});
    } else if (prog == "xargs") {
        skip_opts({"-I", "-i", "-L", "-l", "-n", "-P", "-s", "-d", "-E", "-e", "-a",
                   "--replace", "--max-args", "--max-procs", "--delimiter", "--arg-file"});
    } else {
        return std::nullopt;
    }
    if (i >= argv.size()) return std::nullopt;
    return i;
}

// `sh -c SCRIPT` / `bash -lc SCRIPT` → SCRIPT (only when it is a literal).
// `eval A B …` → "A B …" (eval joins its args with spaces and runs that).
std::optional<std::string> shell_c_script(const std::vector<Word>& argv) {
    if (argv.empty()) return std::nullopt;
    const std::string* p0 = lit(argv[0]);
    if (!p0) return std::nullopt;
    if (*p0 == "eval") {
        std::string joined;
        for (std::size_t i = 1; i < argv.size(); ++i) {
            const std::string* w = lit(argv[i]);
            if (!w) return std::nullopt;     // eval "$X": unknowable
            if (i > 1) joined += ' ';
            joined += *w;
        }
        if (joined.empty()) return std::nullopt;
        return joined;
    }
    if (!is_shell(basename(*p0))) return std::nullopt;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string* w = lit(argv[i]);
        if (!w) return std::nullopt;
        if (w->size() >= 2 && (*w)[0] == '-' && (*w)[1] != '-'
            && w->find('c') != std::string::npos) {
            if (i + 1 < argv.size()) {
                if (const std::string* s = lit(argv[i + 1])) return *s;
                // Dynamic script (bash -c "$X") — can't know it.
                return std::nullopt;
            }
            return std::nullopt;
        }
        if (w->empty() || (*w)[0] != '-') return std::nullopt;   // script file, not -c
    }
    return std::nullopt;
}

// find … -exec CMD … {} ; / + — each -exec/-execdir/-ok carries a command.
std::vector<std::vector<Word>> find_exec_commands(const std::vector<Word>& argv) {
    std::vector<std::vector<Word>> out;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string* w = lit(argv[i]);
        if (!w || (*w != "-exec" && *w != "-execdir" && *w != "-ok" && *w != "-okdir")) continue;
        std::vector<Word> inner;
        std::size_t j = i + 1;
        for (; j < argv.size(); ++j) {
            const std::string* x = lit(argv[j]);
            if (x && (*x == ";" || *x == "+" || *x == "\\;")) break;
            inner.push_back(argv[j]);
        }
        if (!inner.empty()) out.push_back(std::move(inner));
        i = j;
    }
    return out;
}

void lower_into(std::string_view source, Script& out, Ctx extra, int nesting,
                std::uint32_t span_b, std::uint32_t span_e);

// Expand the nested commands a lowered command carries, appending them to the
// script with Ctx::Nested and the carrier's span.
void unwrap_nested(Script& s, std::size_t idx, int nesting) {
    if (nesting >= kMaxNesting) { s.truncated = true; return; }
    // Copy: push_back below may reallocate.
    const Command carrier = s.commands[idx];
    const Ctx ctx = carrier.ctx | Ctx::Nested;

    auto push_argv = [&](std::vector<Word> argv) {
        Command c;
        c.argv = std::move(argv);
        c.ctx = ctx;
        c.pipeline = carrier.pipeline;
        c.stage = carrier.stage;
        c.begin = carrier.begin;
        c.end = carrier.end;
        // `timeout 5 python3 - <<EOF`, `sudo tee f > /dev/null`: the wrapper
        // passes its fds straight through, so the inner command has the
        // carrier's redirects and sits in the same pipeline position.
        c.redirects = carrier.redirects;
        c.join = carrier.join;
        s.commands.push_back(std::move(c));
        unwrap_nested(s, s.commands.size() - 1, nesting + 1);
    };

    if (auto k = wrapper_skip(carrier.argv)) {
        push_argv(std::vector<Word>(carrier.argv.begin() + static_cast<std::ptrdiff_t>(*k),
                                    carrier.argv.end()));
        return;
    }
    if (auto script = shell_c_script(carrier.argv)) {
        const std::size_t first = s.commands.size();
        lower_into(*script, s, ctx, nesting + 1, carrier.begin, carrier.end);
        // `sh -c 'a; b' | col` / `sh -c '…' > f`: the inner script's LAST
        // command writes to the carrier's stdout.
        for (std::size_t k = s.commands.size(); k > first; --k) {
            Command& last = s.commands[k - 1];
            if (last.join == Join::End) {
                last.join = carrier.join;
                last.redirects.insert(last.redirects.end(), carrier.redirects.begin(),
                                      carrier.redirects.end());
                break;
            }
        }
        return;
    }
    if (carrier.program() == "find")
        for (auto& inner : find_exec_commands(carrier.argv)) push_argv(std::move(inner));
}

void lower_into(std::string_view source, Script& out, Ctx extra, int nesting,
                std::uint32_t span_b, std::uint32_t span_e) {
    if (source.size() > kMaxSource) { out.truncated = true; source = source.substr(0, kMaxSource); }
    TSParser& p = thread_parser();
    TreePtr tree{ts_parser_parse_string(&p, nullptr, source.data(),
                                        static_cast<std::uint32_t>(source.size()))};
    if (!tree) { out.truncated = true; return; }
    const TSNode root = ts_tree_root_node(tree.get());
    if (nesting == 0) out.clean = !ts_node_has_error(root);

    const std::size_t first = out.commands.size();
    Lowerer lw{Src{source}, out};
    lw.stmt(root, extra, 0, 0, 0);
    if (lw.truncated) out.truncated = true;
    // Spans of nested commands point at their carrier in the ORIGINAL source.
    if (nesting > 0)
        for (std::size_t i = first; i < out.commands.size(); ++i) {
            out.commands[i].begin = span_b;
            out.commands[i].end = span_e;
        }
    const std::size_t last = out.commands.size();
    for (std::size_t i = first; i < last; ++i) unwrap_nested(out, i, nesting);
}

} // namespace

// ── Public: Redirect / Command / Script ─────────────────────────────────

bool Redirect::writes_file() const noexcept {
    if (kind != Kind::Out && kind != Kind::Append && kind != Kind::OutErr) return false;
    if (const std::string* t = lit(target)) return *t != "/dev/null";
    return true;   // dynamic target: assume it writes somewhere real
}

std::string_view Command::program() const noexcept {
    if (argv.empty()) return {};
    if (const std::string* p = lit(argv[0])) return basename(*p);
    return {};
}

bool Command::fully_literal() const noexcept {
    for (const auto& w : argv) if (!lit(w)) return false;
    for (const auto& r : redirects) if (!lit(r.target)) return false;
    for (const auto& [_, v] : env) if (!lit(v)) return false;
    return true;
}

std::vector<const Command*> Script::top_level() const {
    std::vector<const Command*> v;
    for (const auto& c : commands)
        if ((static_cast<std::uint16_t>(c.ctx) & ~static_cast<std::uint16_t>(Ctx::Piped)) == 0)
            v.push_back(&c);
    return v;
}

Script analyze(std::string_view source) {
    Script s;
    s.source = std::string{source};
    try {
        lower_into(s.source, s, Ctx::None, 0, 0, static_cast<std::uint32_t>(s.source.size()));
    } catch (...) {
        s.clean = false;
        s.truncated = true;
    }
    return s;
}

// ═════════════════════════════════════════════════════════════════════════
//  5. Guard
// ═════════════════════════════════════════════════════════════════════════
//
// Rules are functions  Command → optional<Refusal>, run over EVERY command in
// the script (top-level, piped, in $(…), in control flow, and the nested ones
// unwrapped from sudo/env/bash -c/xargs/find -exec). A rule never looks at
// raw text — it sees argv words, and a Dyn word is judged by its spelling only
// where a spelling is conclusive (`$HOME`, `~`, `${HOME}/..`).

namespace {

using Rule = Refusal::Rule;

Refusal refuse(Rule r, std::string msg, const Command& c) {
    return Refusal{r, std::move(msg), c.begin, c.end};
}

// Collapse `.`/`..`/duplicate slashes without touching the filesystem.
std::string normalize_path(std::string_view p) {
    const bool absolute = !p.empty() && p.front() == '/';
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i <= p.size()) {
        const std::size_t j = std::min(p.find('/', i), p.size());
        const auto seg = p.substr(i, j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t k = 0; k < parts.size(); ++k) { if (k) out += '/'; out += parts[k]; }
    if (out.empty()) out = ".";
    return out;
}

// What a path-ish word names, for "is this catastrophic to wipe?".
enum class Wide : std::uint8_t { No, Root, Home, Cwd };

Wide wide_target(const Word& w) {
    std::string_view t = spelling(w);
    // Strip one pair of surrounding quotes left in a Dyn spelling ("$HOME").
    if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front())
        t = t.substr(1, t.size() - 2);
    // A trailing glob on the dangerous dir is still the dangerous dir: /*, ~/*
    std::string s{t};
    while (s.size() >= 2 && (s.ends_with("/*") || s.ends_with("/.*")))
        s.resize(s.size() - (s.ends_with("/.*") ? 3 : 2));
    if (s == "*" || s == ".*") return Wide::Cwd;

    for (std::string_view home : {"$HOME", "${HOME}", "~"}) {
        if (s == home) return Wide::Home;
        if (s.size() > home.size() && s.starts_with(home) && s[home.size()] == '/') {
            const auto rest = normalize_path(std::string_view{s}.substr(home.size()));
            if (rest == "/" || rest == ".") return Wide::Home;
            // ~/../.. climbs out of home: count net upward steps
            int up = 0, down = 0;
            std::string_view r{s};
            r.remove_prefix(home.size());
            std::size_t i = 0;
            while (i < r.size()) {
                const std::size_t j = std::min(r.find('/', i), r.size());
                const auto seg = r.substr(i, j - i);
                if (seg == "..") { if (down > 0) --down; else ++up; }
                else if (!seg.empty() && seg != ".") ++down;
                i = j + 1;
            }
            if (up >= 2) return Wide::Root;    // /home/u/../.. == /
            if (up == 1 && down == 0) return Wide::Root;   // /home — every user's home
            return Wide::No;
        }
    }
    if (s.starts_with('$')) return Wide::No;   // other variables: unknowable
    if (std::holds_alternative<Dyn>(w) && std::get<Dyn>(w).why == Dyn::Why::Substitution)
        return Wide::No;
    const auto norm = normalize_path(s);
    if (norm == "/") return Wide::Root;
    if (norm == "." || norm == "..") return Wide::Cwd;
    return Wide::No;
}

std::optional<Refusal> wide_refusal(Wide k, const Command& c, std::string_view what) {
    switch (k) {
        case Wide::Root:
            return refuse(Rule::RecursiveDeleteRoot,
                std::string{"refusing "} + std::string{what}
                + " that could wipe the filesystem root", c);
        case Wide::Home:
            return refuse(Rule::RecursiveDeleteHome,
                std::string{"refusing "} + std::string{what} + " of the home directory", c);
        case Wide::Cwd:
            return refuse(Rule::RecursiveDeleteCwd,
                std::string{"refusing "} + std::string{what}
                + " of the whole working directory (or its parent)", c);
        case Wide::No: return std::nullopt;
    }
    return std::nullopt;
}

// Operands = argv words after the program that are not options (until `--`).
template <class IsOpt>
std::vector<const Word*> operands(const Command& c, IsOpt&& takes_value) {
    std::vector<const Word*> v;
    bool eoo = false;
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* w = lit(c.argv[i]);
        if (!eoo && w && *w == "--") { eoo = true; continue; }
        if (!eoo && w && w->size() > 1 && (*w)[0] == '-') {
            if (takes_value(*w)) ++i;
            continue;
        }
        v.push_back(&c.argv[i]);
    }
    return v;
}

bool has_short_flag(const Command& c, char f, std::string_view long_name = {}) {
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* w = lit(c.argv[i]);
        if (!w) continue;
        if (*w == "--") break;
        if (!long_name.empty() && *w == long_name) return true;
        if (w->size() >= 2 && (*w)[0] == '-' && (*w)[1] != '-'
            && w->find(f) != std::string::npos) return true;
    }
    return false;
}

std::optional<Refusal> rule_rm(const Command& c) {
    if (c.program() != "rm") return std::nullopt;
    const bool recursive = has_short_flag(c, 'r', "--recursive") || has_short_flag(c, 'R');
    if (!recursive) return std::nullopt;
    for (const Word* w : operands(c, [](std::string_view) { return false; }))
        if (auto r = wide_refusal(wide_target(*w), c, "recursive delete")) return r;
    return std::nullopt;
}

// `find START… EXPR` deletes (via -delete or -exec rm) whatever EXPR selects
// under START. It is catastrophic only when START is wide AND nothing in EXPR
// narrows the selection: `find ~ -delete`, `find / -exec rm -rf {} +` wipe
// everything; `find . -name '*.o' -delete` deletes build artefacts. A test
// (-name, -type, -mtime, -size, -newer, …) narrows; -maxdepth alone does not.
std::optional<Refusal> rule_find_delete(const Command& c) {
    if (c.program() != "find") return std::nullopt;
    static constexpr std::string_view kNarrow[] = {
        "-name", "-iname", "-path", "-ipath", "-wholename", "-iwholename",
        "-regex", "-iregex", "-type", "-xtype", "-mtime", "-mmin", "-atime",
        "-amin", "-ctime", "-cmin", "-newer", "-anewer", "-cnewer", "-size",
        "-user", "-group", "-uid", "-gid", "-perm", "-links", "-inum",
        "-samefile", "-empty", "-lname", "-ilname", "-nouser", "-nogroup",
    };
    bool deletes = false, narrowed = false;
    std::size_t expr = c.argv.size();
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        const std::string* w = lit(c.argv[i]);
        if (!w) continue;
        if (expr == c.argv.size() && !w->empty()
            && ((*w)[0] == '-' || *w == "(" || *w == "!"))
            expr = i;
        if (*w == "-delete") deletes = true;
        if (std::ranges::find(kNarrow, *w) != std::end(kNarrow)) narrowed = true;
        if (*w == "-exec" || *w == "-execdir" || *w == "-ok" || *w == "-okdir") {
            // does the exec'd command remove its argument?
            if (i + 1 < c.argv.size())
                if (const std::string* x = lit(c.argv[i + 1]);
                    x && (basename(*x) == "rm" || basename(*x) == "shred"
                          || basename(*x) == "unlink" || basename(*x) == "rmdir"))
                    deletes = true;
        }
    }
    if (!deletes || narrowed) return std::nullopt;
    for (std::size_t i = 1; i < expr; ++i)
        if (auto r = wide_refusal(wide_target(c.argv[i]), c, "unfiltered `find` delete")) {
            r->rule = Rule::FindDeleteWide;
            return r;
        }
    return std::nullopt;
}

std::optional<Refusal> rule_perms(const Command& c) {
    const auto p = c.program();
    if (p != "chmod" && p != "chown" && p != "chgrp") return std::nullopt;
    if (!has_short_flag(c, 'R', "--recursive")) return std::nullopt;
    const auto ops = operands(c, [](std::string_view w) { return w == "--reference"; });
    // first operand is the mode / owner
    for (std::size_t k = 1; k < ops.size(); ++k)
        if (auto r = wide_refusal(wide_target(*ops[k]), c,
                                  std::string{"recursive `"} + std::string{p} + "`")) {
            r->rule = Rule::RecursivePermsWide;
            return r;
        }
    return std::nullopt;
}

std::optional<Refusal> rule_disk(const Command& c) {
    const auto p = c.program();
    if (p.starts_with("mkfs") || p == "mke2fs" || p == "wipefs" || p == "fdisk"
        || p == "sfdisk" || p == "parted")
        return refuse(Rule::Mkfs, "refusing `" + std::string{p}
            + "` — it would reformat or repartition a disk", c);
    if (p == "dd") {
        for (std::size_t i = 1; i < c.argv.size(); ++i) {
            const std::string_view w = spelling(c.argv[i]);
            if (w.starts_with("of=/dev/") && w != "of=/dev/null" && w != "of=/dev/stdout"
                && w != "of=/dev/stderr")
                return refuse(Rule::DiskWrite, "refusing `dd` onto a device (" + std::string{w}
                    + ") — it would overwrite a disk", c);
        }
        return std::nullopt;
    }
    for (const auto& r : c.redirects) {
        if (!r.writes_file()) continue;
        const std::string_view t = spelling(r.target);
        if (t.starts_with("/dev/sd") || t.starts_with("/dev/nvme") || t.starts_with("/dev/hd")
            || t.starts_with("/dev/vd") || t.starts_with("/dev/mmcblk") || t.starts_with("/dev/disk"))
            return refuse(Rule::DiskWrite, "refusing to write to a raw disk device ("
                + std::string{t} + ")", c);
    }
    return std::nullopt;
}

std::optional<Refusal> rule_power(const Command& c) {
    const auto p = c.program();
    if (p == "shutdown" || p == "reboot" || p == "poweroff" || p == "halt")
        return refuse(Rule::Power, "refusing `" + std::string{p} + "`", c);
    if (p == "systemctl" && c.argv.size() > 1)
        if (const std::string* a = lit(c.argv[1]);
            a && (*a == "poweroff" || *a == "reboot" || *a == "halt" || *a == "kexec"))
            return refuse(Rule::Power, "refusing `systemctl " + *a + "`", c);
    // Rebooting an attached device (phone/board) mid-session loses its state
    // too — the old guard refused these, keep refusing.
    if ((p == "fastboot" || p == "adb")
        && std::ranges::any_of(c.argv, [](const Word& w) {
               const std::string* s = lit(w); return s && (*s == "reboot" || *s == "reboot-bootloader"); }))
        return refuse(Rule::Power, "refusing `" + std::string{p} + " reboot` — ask the user first", c);
    return std::nullopt;
}

std::optional<Refusal> rule_git(const Command& c) {
    if (c.program() != "git") return std::nullopt;
    // Skip global options (-C dir, -c k=v, --git-dir=…) to find the subcommand.
    std::size_t i = 1;
    while (i < c.argv.size()) {
        const std::string* w = lit(c.argv[i]);
        if (!w || w->empty() || (*w)[0] != '-') break;
        i += (*w == "-C" || *w == "-c") ? 2 : 1;
    }
    if (i >= c.argv.size()) return std::nullopt;
    const std::string* sub = lit(c.argv[i]);
    if (!sub) return std::nullopt;
    auto has = [&](std::initializer_list<std::string_view> names) {
        for (std::size_t k = i + 1; k < c.argv.size(); ++k)
            if (const std::string* w = lit(c.argv[k]))
                for (auto n : names) if (*w == n) return true;
        return false;
    };
    if (*sub == "push") {
        bool force = has({"--force", "-f"});
        for (std::size_t k = i + 1; k < c.argv.size(); ++k)   // +refspec is a force push
            if (const std::string* w = lit(c.argv[k]); w && w->size() > 1 && (*w)[0] == '+')
                force = true;
        for (std::size_t k = i + 1; k < c.argv.size(); ++k)   // combined short flags: -fu
            if (const std::string* w = lit(c.argv[k]);
                w && w->size() > 2 && (*w)[0] == '-' && (*w)[1] != '-' && w->find('f') != std::string::npos)
                force = true;
        if (force && !has({"--force-with-lease", "--force-if-includes"}))
            return refuse(Rule::ForcePush, "refusing `git push --force`; use "
                "--force-with-lease and ask the user first", c);
    }
    if (*sub == "rebase" && has({"-i", "--interactive"}))
        return refuse(Rule::Interactive, "refusing interactive rebase (`git rebase -i`) — "
            "the editor would block the agent. Use non-interactive rebase options instead.", c);
    if (*sub == "add" && has({"-i", "-p", "--interactive", "--patch"}))
        return refuse(Rule::Interactive,
            "refusing interactive git add — use explicit file paths.", c);
    if (*sub == "commit" && !has({"-m", "-F", "--message", "--file", "--amend", "-C",
                                   "--no-edit", "--fixup", "--squash"})) {
        bool attached = false;   // -m"msg" / --message=msg / -qm / -am / -F -
        for (std::size_t k = i + 1; k < c.argv.size(); ++k)
            if (const std::string* w = lit(c.argv[k]); w) {
                if (w->starts_with("--message=") || w->starts_with("--file=")
                    || w->starts_with("--reuse-message=") || w->starts_with("--fixup="))
                    attached = true;
                // combined short flags: any of m/F/C in a -xyz cluster
                if (w->size() >= 2 && (*w)[0] == '-' && (*w)[1] != '-'
                    && w->find_first_of("mFC") != std::string::npos)
                    attached = true;
            }
        if (!attached)
            return refuse(Rule::Interactive, "refusing `git commit` without -m/-F — it would "
                "open an editor. Pass -m \"<message>\" to commit non-interactively, or use the "
                "git_commit tool.", c);
    }
    return std::nullopt;
}

std::optional<Refusal> rule_interactive(const Command& c) {
    static constexpr std::string_view always[] = {
        "vim", "vi", "nvim", "nano", "emacs", "pico", "ed", "joe", "mcedit",
        "less", "more", "man", "top", "htop", "btop", "tmux", "screen",
        "mysql", "psql", "sqlite3", "redis-cli", "mongo", "mongosh",
        "ghci", "ocaml", "irb", "pry", "lua", "tclsh", "gdb", "lldb",
        "fzf", "dialog", "whiptail",
    };
    static constexpr std::string_view if_bare[] = {
        "python", "python3", "node", "deno", "ruby", "php", "iex", "bash",
        "sh", "zsh", "fish", "pwsh", "powershell", "cmd",
    };
    const auto p = c.program();
    if (p.empty()) return std::nullopt;
    // A program invoked by a RELATIVE or non-system path is the user's own
    // binary that happens to share a name (`/tmp/sh`, `./build/top`), not the
    // interactive tool.
    if (const std::string* a0 = lit(c.argv[0]); a0 && a0->find('/') != std::string::npos) {
        static constexpr std::string_view sys[] = {"/bin/", "/usr/bin/", "/usr/local/bin/",
                                                   "/sbin/", "/usr/sbin/", "/opt/homebrew/bin/"};
        if (std::ranges::none_of(sys, [&](std::string_view d) { return a0->starts_with(d); }))
            return std::nullopt;
    }
    auto has_arg = [&](std::initializer_list<std::string_view> names) {
        for (std::size_t i = 1; i < c.argv.size(); ++i)
            if (const std::string* w = lit(c.argv[i]))
                for (auto n : names)
                    if (*w == n || (n.ends_with('=') && w->starts_with(n))) return true;
        return false;
    };
    // Info flags print and exit, whatever the program: `tmux -V`,
    // `python3 -VV`, `emacs --version`, `gdb --help`.
    for (std::size_t i = 1; i < c.argv.size(); ++i)
        if (const std::string* w = lit(c.argv[i]);
            w && (*w == "--version" || *w == "-V" || *w == "-VV" || *w == "-v" || *w == "--help"
                  || *w == "-h" || *w == "-help" || *w == "-version"))
            return std::nullopt;
    // stdout into a pipe or a file: a pager/TUI detects the non-tty and
    // degrades to plain output (`man x | grep`, `top -bn1 > f` …). Editors
    // and REPLs still read the terminal, so only for these.
    const bool stdout_redirected =
        c.join == Join::Pipe || c.join == Join::PipeErr
        || std::ranges::any_of(c.redirects, [](const Redirect& r) {
               return (r.kind == Redirect::Kind::Out || r.kind == Redirect::Kind::Append
                       || r.kind == Redirect::Kind::OutErr) && (r.fd == -1 || r.fd == 1); });
    if (stdout_redirected && (p == "man" || p == "less" || p == "more" || p == "top"
                              || p == "htop" || p == "btop"))
        return std::nullopt;
    // Explicit non-interactive modes. The old first-token check never saw
    // these because it never saw past argv[0]; an AST guard must honour them
    // or it refuses `gdb -batch`, `emacs --batch`, `tmux show -gv x` — all
    // common, all non-blocking.
    if ((p == "gdb" && has_arg({"-batch", "--batch", "-x", "--command", "-ex", "--eval-command"}))
        || (p == "lldb" && has_arg({"--batch", "-b", "-o", "--one-line", "-s", "--source"}))
        || (p == "emacs" && has_arg({"--batch", "-batch", "--script", "-script"}))
        || ((p == "vim" || p == "vi") && has_arg({"-es", "-Es"}))
        || (p == "nvim" && has_arg({"--headless", "-es", "-Es"}))
        || ((p == "top" || p == "htop" || p == "btop") && has_arg({"-b", "-bn1", "-n", "--batch"}))
        || (p == "man" && has_arg({"-P", "--pager", "-f", "-k", "-w", "--path", "--where", "--whatis", "--apropos"}))
        || ((p == "mysql" || p == "psql" || p == "sqlite3" || p == "redis-cli" || p == "mongosh")
            && (has_arg({"-e", "-c", "--command", "--eval", "-f", "--file", "-batch", "-cmd"})
                || c.argv.size() > (p == "sqlite3" ? 2u : 99u)))
        || ((p == "lua" || p == "tclsh" || p == "ghci" || p == "irb" || p == "ocaml" || p == "pry")
            && c.argv.size() > 1))
        return std::nullopt;
    if (p == "tmux" || p == "screen") {
        // tmux with a subcommand that just queries or drives a server returns
        // at once; only bare `tmux` / `tmux attach` / `new-session` without -d
        // take over the terminal.
        std::string_view sub;
        for (std::size_t i = 1; i < c.argv.size(); ++i)
            if (const std::string* w = lit(c.argv[i]); w && !w->empty() && (*w)[0] != '-') { sub = *w; break; }
        const bool attaches = sub.empty() || sub == "attach" || sub == "attach-session" || sub == "a"
            || ((sub == "new" || sub == "new-session") && !has_arg({"-d"}))
            || (p == "screen" && !has_arg({"-dm", "-dmS", "-ls", "-list", "-X", "-Q"}));
        if (!attaches) return std::nullopt;
    }
    // Piped INTO the program (`git log | less`) still blocks on a tty; reading
    // a pipe or heredoc on stdin (`python3 - <<EOF`, `cat x | python3`) does not.
    const bool stdin_fed = has(c.ctx, Ctx::Piped)
        || std::ranges::any_of(c.redirects, [](const Redirect& r) {
               return r.kind == Redirect::Kind::In || r.kind == Redirect::Kind::HereDoc
                   || r.kind == Redirect::Kind::HereString; });
    if (std::ranges::find(always, p) != std::end(always)) {
        // A REPL/debugger/pager fed on stdin (`echo 'bt' | gdb -p N`,
        // `echo 'puts 1' | tclsh`, `cat f | less`) runs its input and exits.
        // Only programs that take over the TERMINAL itself still block: full-
        // screen editors and TUIs read /dev/tty, not stdin.
        static constexpr std::string_view owns_tty[] = {
            "vim", "vi", "nvim", "nano", "emacs", "pico", "joe", "mcedit",
            "top", "htop", "btop", "tmux", "screen", "fzf", "dialog", "whiptail",
        };
        if (stdin_fed && std::ranges::find(owns_tty, p) == std::end(owns_tty))
            return std::nullopt;
        return refuse(Rule::Interactive, "refusing to run interactive command '" + std::string{p}
            + "' — it would block waiting for stdin. Use a non-interactive alternative "
              "(e.g. for editors: use the write/edit tools).", c);
    }
    if (std::ranges::find(if_bare, p) != std::end(if_bare) && !stdin_fed) {
        // Bare REPL = no operands at all. `python3 -` / `node -` read the
        // program from stdin (a heredoc on this or a trailing command) and so
        // do not block; `python3 -i` forces the REPL.
        bool bare = true, forced = false;
        for (std::size_t i = 1; i < c.argv.size(); ++i) {
            const std::string* w = lit(c.argv[i]);
            if (!w) { bare = false; break; }
            if (*w == "-i" || *w == "--interactive") { forced = true; continue; }
            if (*w == "-" ) { bare = false; break; }
            // Program-text flags: -c (python/sh/fish), -e/-p (node/ruby/perl),
            // -r (php), -m (python module), --eval.
            if (*w == "-c" || *w == "-e" || *w == "-p" || *w == "-r" || *w == "-m"
                || *w == "--eval" || *w == "--print" || *w == "--command"
                || w->starts_with("-c") || w->starts_with("-e")) { bare = false; break; }
            if (!w->empty() && (*w)[0] == '-') continue;   // -u, -O, -B: still a REPL
            bare = false;
            break;
        }
        // `fish -i -c '…'`: -i with a -c program runs the program, then exits.
        if (forced && !bare) forced = false;
        if (bare || forced)
            return refuse(Rule::Interactive, "refusing to start interactive " + std::string{p}
                + " REPL — it would block waiting for stdin. Provide a script path or use "
                  "`-c \"…\"` to run a snippet.", c);
    }
    return std::nullopt;
}

constexpr std::optional<Refusal> (*kRules[])(const Command&) = {
    rule_rm, rule_find_delete, rule_perms, rule_disk, rule_power, rule_git, rule_interactive,
};

// Cross-command rules need the pipeline, not one command.
std::optional<Refusal> rule_pipe_to_shell(const Script& s) {
    for (std::size_t i = 0; i + 1 < s.commands.size(); ++i) {
        const Command& a = s.commands[i];
        const Command& b = s.commands[i + 1];
        if (b.pipeline != a.pipeline || b.stage != a.stage + 1) continue;
        const auto pa = a.program(), pb = b.program();
        if ((pa == "curl" || pa == "wget" || pa == "fetch") && is_shell(pb))
            return refuse(Rule::CurlPipeShell, "refusing `curl|sh` / `wget|sh` — download "
                "the script, inspect it, then run explicitly.", b);
    }
    // bash <(curl …) / sh -c "$(curl …)"
    for (const auto& c : s.commands) {
        if (!has(c.ctx, Ctx::Substitution)) continue;
        const auto p = c.program();
        if (p != "curl" && p != "wget") continue;
        for (const auto& d : s.commands)
            if (!has(d.ctx, Ctx::Substitution) && is_shell(d.program())
                && d.begin <= c.begin && c.end <= d.end)
                return refuse(Rule::CurlPipeShell, "refusing to execute a downloaded script "
                    "(`sh <(curl …)`) — download it, inspect it, then run explicitly.", d);
    }
    return std::nullopt;
}

bool is_fork_bomb(std::string_view src) {
    // :(){ :|:& };:  and renamed variants: a function whose body pipes a call
    // to itself into itself in the background.
    std::string s;
    for (char ch : src) if (ch != ' ' && ch != '\t' && ch != '\n') s += ch;
    for (std::size_t p = s.find("(){"); p != std::string::npos; p = s.find("(){", p + 1)) {
        std::size_t b = p;
        while (b > 0 && s[b - 1] != ';' && s[b - 1] != '&' && s[b - 1] != '|' && s[b - 1] != '{') --b;
        const std::string name = s.substr(b, p - b);
        if (name.empty()) continue;
        if (s.find(name + "|" + name + "&", p) != std::string::npos) return true;
    }
    return false;
}

} // namespace

std::optional<Refusal> guard(const Script& s) {
    if (is_fork_bomb(s.source))
        return Refusal{Rule::ForkBomb, "fork-bomb pattern refused", 0,
                       static_cast<std::uint32_t>(s.source.size())};
    for (const auto& c : s.commands)
        for (auto rule : kRules)
            if (auto r = rule(c)) return r;
    if (auto r = rule_pipe_to_shell(s)) return r;
    return std::nullopt;
}

// ═════════════════════════════════════════════════════════════════════════
//  6. Actions
// ═════════════════════════════════════════════════════════════════════════
//
// Each top-level pipeline becomes a Step: its first stage is translated into
// an Action by a per-program parser (flags → typed fields); later stages are
// Shapes. A parser that meets a flag it does not model returns Other, never a
// guessed action — so every typed action is an honest reading of argv.
//
// `exact` means "a native implementation of this action + shapes, given the
// same files, produces the same bytes the shell would". It needs: a literal
// argv, no redirects that write, no dynamic words, a top-level position, and
// every flag modelled. Display and telemetry ignore it; native execution
// must require it.

namespace {

// Flag cursor over a literal argv: understands clustered short flags
// (-rnE), attached values (-A3, -n20, --max-count=5), separate values
// (-A 3), `--` end-of-options, and operands interleaved with flags (GNU).
// A literal argv, except that GLOB words are allowed as path operands: the
// shell expands them before the program runs, so `grep -rn x src/*.hpp` is
// still a search — just over a set of files we can't name without the
// filesystem. Such an action is typed but never `exact`.
class Flags {
public:
    explicit Flags(const Command& c, bool allow_globs = false) {
        for (std::size_t i = 1; i < c.argv.size(); ++i) {
            if (const std::string* w = lit(c.argv[i])) { words_.push_back(*w); continue; }
            const auto* d = std::get_if<Dyn>(&c.argv[i]);
            if (allow_globs && d && (d->why == Dyn::Why::Glob || d->why == Dyn::Why::Brace)) {
                words_.push_back(d->text);
                globbed_ = true;
                continue;
            }
            ok_ = false;
            return;
        }
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool globbed() const noexcept { return globbed_; }

    // Walk. `short_val` = short flags that take a value; `long_val` = long
    // flags that take a value. on_flag(name, value) returns false for an
    // unmodelled flag (→ whole parse fails). Operands go to `operands`.
    template <class F>
    bool walk(std::string_view short_val, std::initializer_list<std::string_view> long_val,
              F&& on_flag, std::vector<std::string>& operands) {
        bool eoo = false;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            const std::string& w = words_[i];
            if (eoo || w == "-" || w.empty() || w[0] != '-') { operands.push_back(w); continue; }
            if (w == "--") { eoo = true; continue; }
            if (w.starts_with("--")) {
                const auto eq = w.find('=');
                std::string name = w.substr(0, eq);
                std::optional<std::string> val;
                if (eq != std::string::npos) val = w.substr(eq + 1);
                else if (std::ranges::find(long_val, name) != long_val.end()) {
                    if (i + 1 >= words_.size()) return false;
                    val = words_[++i];
                }
                if (!on_flag(std::string_view{name}, val)) return false;
                continue;
            }
            // -N (head/tail style numeric flag)
            if (w.size() > 1 && std::all_of(w.begin() + 1, w.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                if (!on_flag(std::string_view{"-N"}, std::optional<std::string>{w.substr(1)})) return false;
                continue;
            }
            for (std::size_t k = 1; k < w.size(); ++k) {
                const std::string name{'-', w[k]};
                if (short_val.find(w[k]) != std::string_view::npos) {
                    std::optional<std::string> val;
                    if (k + 1 < w.size()) val = w.substr(k + 1);
                    else if (i + 1 < words_.size()) val = words_[++i];
                    else return false;
                    if (!on_flag(std::string_view{name}, val)) return false;
                    break;
                }
                if (!on_flag(std::string_view{name}, std::nullopt)) return false;
            }
        }
        return true;
    }
private:
    std::vector<std::string> words_;
    bool ok_ = true;
    bool globbed_ = false;
};

std::optional<std::int64_t> to_int(std::string_view s) {
    std::int64_t v = 0;
    if (s.starts_with('+')) s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::string join_path(const std::string& cwd, const std::string& p) {
    if (cwd.empty() || p.empty() || p.front() == '/' || p.front() == '~') return p;
    if (p == ".") return cwd;
    std::string out = cwd;
    if (!out.ends_with('/')) out += '/';
    out += p.starts_with("./") ? p.substr(2) : p;
    return out;
}

struct Parsed { Action action; bool exact = true; };

// ── read-like: cat, head, tail, sed -n, nl ───────────────────────────────────────────

std::optional<Parsed> parse_cat(const Command& c) {
    Flags f{c};
    if (!f.ok()) return std::nullopt;
    std::vector<std::string> ops;
    bool numbered = false;
    if (!f.walk("", {}, [&](std::string_view n, const std::optional<std::string>&) {
            if (n == "-n" || n == "--number") { numbered = true; return true; }
            return false;
        }, ops)) return std::nullopt;
    if (ops.size() != 1 || ops[0] == "-") return std::nullopt;   // stdin / concatenation
    return Parsed{ReadAction{ops[0], {}}, !numbered};
}

std::optional<Parsed> parse_head_tail(const Command& c, bool tail) {
    Flags f{c};
    if (!f.ok()) return std::nullopt;
    std::vector<std::string> ops;
    std::optional<std::int64_t> n;
    bool from_start = false;          // tail -n +K
    if (!f.walk("nc", {"--lines", "--bytes"}, [&](std::string_view k, const std::optional<std::string>& v) {
            if (k == "-n" || k == "--lines" || k == "-N") {
                if (!v) return false;
                if (tail && v->starts_with('+')) from_start = true;
                auto x = to_int(*v);
                if (!x) return false;
                n = *x < 0 ? -*x : *x;
                return true;
            }
            return k == "-q" || k == "--quiet";
        }, ops)) return std::nullopt;
    if (ops.size() != 1) return std::nullopt;
    const std::int64_t k = n.value_or(10);
    LineRange r;
    if (!tail) { r.first = 1; r.last = k; }
    else if (from_start) { r.first = std::max<std::int64_t>(1, k); }
    else { r.first = k; r.from_end = true; }
    return Parsed{ReadAction{ops[0], r}, true};
}

// sed -n 'A,Bp' / 'Ap' / 'A,$p' FILE — the print-a-range idiom (85% of sed).
// Also typed (never exact): address forms we can't evaluate statically —
// `/start/,/end/p`, `A,+Np`, `$(…),+14p` — they are still "print part of
// this one file", which is what the card and telemetry need to know.
std::optional<Parsed> parse_sed(const Command& c) {
    // A dynamic script word (`sed -n "$(grep -n x f | cut -d: -f1),+14p" f`)
    // is common: allow it here, then require a literal FILE operand.
    std::vector<std::string> words;
    bool dyn_script = false;
    for (std::size_t i = 1; i < c.argv.size(); ++i) {
        if (const std::string* w = lit(c.argv[i])) { words.push_back(*w); continue; }
        const auto* d = std::get_if<Dyn>(&c.argv[i]);
        if (!d || i + 1 >= c.argv.size()) return std::nullopt;   // the file must be literal
        words.push_back("\x01" + d->text);                        // marked dynamic
        dyn_script = true;
    }
    bool quiet = false;
    std::vector<std::string> scripts, ops;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i];
        if (w == "-n" || w == "--quiet" || w == "--silent") { quiet = true; continue; }
        if (w == "-e" || w == "--expression") { if (i + 1 >= words.size()) return std::nullopt; scripts.push_back(words[++i]); continue; }
        if (w.starts_with("--expression=")) { scripts.push_back(w.substr(13)); continue; }
        if (w.size() > 1 && w[0] == '-') {
            // clustered: -ne 'x' / -nE — only n and e are read-only
            for (std::size_t k = 1; k < w.size(); ++k) {
                if (w[k] == 'n') quiet = true;
                else if (w[k] == 'e') { if (i + 1 >= words.size()) return std::nullopt; scripts.push_back(words[++i]); break; }
                else if (w[k] == 'E' || w[k] == 'r') { /* regex dialect: fine for a read */ }
                else return std::nullopt;                      // -i, -s, -z, -u …
            }
            continue;
        }
        ops.push_back(w);
    }
    if (!quiet) return std::nullopt;
    if (scripts.empty()) { if (ops.empty()) return std::nullopt; scripts.push_back(ops.front()); ops.erase(ops.begin()); }
    if (scripts.size() != 1 || ops.size() != 1 || ops[0].starts_with('\x01')) return std::nullopt;

    std::string_view s = scripts[0];
    const bool dyn = dyn_script && s.starts_with('\x01');
    if (dyn) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == ';' || s.back() == '"' || s.back() == '\'')) s.remove_suffix(1);
    if (s.empty() || s.back() != 'p') return std::nullopt;
    // Must be ONE print command over an address: `ADDR p` / `ADDR,ADDR p`.
    // Reject any `;`/newline-separated second command and any
    // write/execute/modify command (w W e r R s y i a c d D q Q = etc.)
    // outside of an address regex. Addresses: numbers, $, +N, ~N, /re/.
    if (!dyn) {
        std::size_t i = 0;
        auto addr = [&]() -> bool {
            if (i >= s.size()) return false;
            if (s[i] == '/') {                       // /regex/ with \/ escapes
                ++i;
                while (i < s.size() && s[i] != '/') { if (s[i] == '\\') ++i; ++i; }
                if (i >= s.size()) return false;
                ++i;
                if (i < s.size() && s[i] == 'I') ++i;
                return true;
            }
            if (s[i] == '$') { ++i; return true; }
            if (s[i] == '+' || s[i] == '~') ++i;
            const std::size_t d0 = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            return i > d0;
        };
        if (!addr()) return std::nullopt;
        if (i < s.size() && s[i] == ',') { ++i; if (!addr()) return std::nullopt; }
        while (i < s.size() && s[i] == ' ') ++i;
        if (i + 1 != s.size()) return std::nullopt;   // exactly `p` remains
    }
    s.remove_suffix(1);

    LineRange r;
    r.clamp_inverted = true;             // GNU sed: 'A,Bp' with B<A prints A
    if (!dyn) {
        const auto comma = s.find(',');
        const auto a = to_int(s.substr(0, comma));
        if (a && *a >= 1) {
            r.first = *a;
            const auto rest = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
            if (comma == std::string_view::npos) { r.last = *a; return Parsed{ReadAction{ops[0], r}, true}; }
            if (rest == "$") return Parsed{ReadAction{ops[0], r}, true};
            if (auto b = to_int(rest); b && !rest.starts_with('+')) { r.last = *b; return Parsed{ReadAction{ops[0], r}, true}; }
            if (rest.starts_with('+')) if (auto k = to_int(rest.substr(1))) {
                r.last = *a + *k;
                return Parsed{ReadAction{ops[0], r}, true};
            }
        }
    }
    // Pattern / computed address: a read of an unknown part of the file.
    return Parsed{ReadAction{ops[0], LineRange{}}, false};
}

// ── search: grep / rg ────────────────────────────────────────────────────────────

std::optional<Parsed> parse_grep(const Command& c, bool rg) {
    Flags f{c, /*allow_globs=*/true};
    if (!f.ok()) return std::nullopt;
    std::vector<std::string> ops;
    SearchAction a;
    a.recursive = rg;             // rg recurses by default
    a.line_numbers = false;
    std::optional<std::string> e_pattern;
    bool exact = true;
    const std::string_view short_val = rg ? "ABCemgtT" : "ABCemf";
    const bool ok = f.walk(short_val,
        {"--include", "--exclude", "--exclude-dir", "--regexp", "--max-count", "--glob",
         "--type", "--context", "--after-context", "--before-context", "--type-not"},
        [&](std::string_view k, const std::optional<std::string>& v) {
            if (k == "-n" || k == "--line-number") { a.line_numbers = true; return true; }
            if (k == "-N" && rg) { a.line_numbers = false; return true; }
            if (k == "-i" || k == "--ignore-case") { a.ignore_case = true; return true; }
            if (k == "-w" || k == "--word-regexp") { a.word = true; return true; }
            if (k == "-F" || k == "--fixed-strings") { a.fixed = true; a.regex = false; return true; }
            if (k == "-E" || k == "--extended-regexp") { a.extended = true; return true; }
            if (k == "-P" || k == "--perl-regexp") { exact = false; return true; }
            if (k == "-r" || k == "-R" || k == "--recursive") { a.recursive = true; return true; }
            if (k == "-c" || k == "--count") { a.count = true; return true; }
            if (k == "-l" || k == "--files-with-matches") { a.files_only = true; return true; }
            if (k == "-H" || k == "--with-filename" || k == "-h" || k == "--no-filename"
                || k == "-s" || k == "--no-messages" || k == "-I" || k == "--color"
                || k == "--no-heading" || k == "--heading" || k == "-S" || k == "--smart-case"
                || k == "--hidden" || k == "--no-ignore" || k == "-o" || k == "--only-matching"
                || k == "-v" || k == "--invert-match" || k == "-x" || k == "--line-regexp"
                || k == "-q" || k == "--quiet" || k == "-a" || k == "--text" || k == "-u"
                || k == "-uu" || k == "-z") {
                // Accepted for translation, but output/semantics differ from
                // our native search → not exact.
                exact = false; return true;
            }
            if (k == "-A" || k == "--after-context")  { auto n = v ? to_int(*v) : std::nullopt; if (!n) return false; a.context_after = static_cast<int>(*n); return true; }
            if (k == "-B" || k == "--before-context") { auto n = v ? to_int(*v) : std::nullopt; if (!n) return false; a.context_before = static_cast<int>(*n); return true; }
            if (k == "-C" || k == "--context")       { auto n = v ? to_int(*v) : std::nullopt; if (!n) return false; a.context_before = a.context_after = static_cast<int>(*n); return true; }
            if (k == "-e" || k == "--regexp") { if (!v || e_pattern) return false; e_pattern = *v; return true; }
            if (k == "--include" || (rg && (k == "-g" || k == "--glob"))) { if (!v) return false; a.include_globs.push_back(*v); return true; }
            if (k == "--exclude" || k == "--exclude-dir" || k == "-t" || k == "--type"
                || k == "-T" || k == "--type-not" || k == "-m" || k == "--max-count") {
                exact = false; return v.has_value();
            }
            return false;
        }, ops);
    if (!ok) return std::nullopt;
    if (e_pattern) a.pattern = *e_pattern;
    else { if (ops.empty()) return std::nullopt; a.pattern = ops.front(); ops.erase(ops.begin()); }
    a.paths = std::move(ops);
    // grep with no path and no -r reads STDIN: not a search of files.
    if (!rg && a.paths.empty() && !a.recursive) return std::nullopt;
    // The pattern itself must not be a glob the shell would expand.
    if (f.globbed() && a.pattern.find_first_of("*?[") != std::string::npos
        && !e_pattern && std::ranges::none_of(c.argv, [&](const Word& w) {
               return lit(w) && *lit(w) == a.pattern; }))
        return std::nullopt;
    return Parsed{std::move(a), exact && !f.globbed()};
}

// ── list: ls / find / fd / tree ─────────────────────────────────────────────────

std::optional<Parsed> parse_ls(const Command& c) {
    Flags f{c, /*allow_globs=*/true};
    if (!f.ok()) return std::nullopt;
    std::vector<std::string> ops;
    ListAction a;
    bool exact = true;
    if (!f.walk("", {"--time-style", "--sort", "--color", "--format"},
            [&](std::string_view k, const std::optional<std::string>&) {
                if (k == "-l") { a.long_format = true; exact = false; return true; }
                if (k == "-a" || k == "-A" || k == "--all" || k == "--almost-all") { a.all = true; exact = false; return true; }
                if (k == "-R" || k == "--recursive") { a.recursive = true; exact = false; return true; }
                if (k == "-1") return true;
                if (k == "-h" || k == "-t" || k == "-r" || k == "-S" || k == "-d" || k == "-F"
                    || k == "-p" || k == "-G" || k == "-N" || k == "--time-style" || k == "--sort"
                    || k == "--color" || k == "--group-directories-first" || k == "-i" || k == "-s"
                    || k == "--human-readable" || k == "-X" || k == "-v" || k == "-U" || k == "--format") {
                    exact = false; return true;
                }
                return false;
            }, ops)) return std::nullopt;
    // `ls a b` lists several paths: typed as a listing of the first, joined
    // for display, never exact.
    a.path = ops.empty() ? "." : ops[0];
    for (std::size_t k = 1; k < ops.size(); ++k) a.path += " " + ops[k];
    if (ops.size() > 1) exact = false;
    return Parsed{std::move(a), exact && !f.globbed()};
}

// wc -l FILE: "how long is this file" — a read that returns a count.
std::optional<Parsed> parse_wc(const Command& c) {
    Flags f{c, /*allow_globs=*/true};
    if (!f.ok()) return std::nullopt;
    std::vector<std::string> ops;
    bool lines = false;
    if (!f.walk("", {}, [&](std::string_view k, const std::optional<std::string>&) {
            if (k == "-l" || k == "--lines") { lines = true; return true; }
            return k == "-c" || k == "-w" || k == "-m" || k == "--bytes" || k == "--words"
                || k == "--chars";
        }, ops)) return std::nullopt;
    if (ops.empty()) return std::nullopt;         // counting stdin
    ListAction a;                                 // stat-like: size of each path
    a.path = ops[0];
    for (std::size_t k = 1; k < ops.size(); ++k) a.path += " " + ops[k];
    a.long_format = true;
    (void)lines;
    return Parsed{std::move(a), false};
}

std::optional<Parsed> parse_find(const Command& c) {
    // find [start] [-maxdepth N] [-type f|d] [-name G | -iname G] [-print]
    // Anything else (‐exec, ‐delete, -o, -newer, …) → not a plain listing.
    ListAction a;
    a.recursive = true;
    std::size_t i = 1;
    std::vector<std::string> starts;
    for (; i < c.argv.size(); ++i) {
        const std::string* w = lit(c.argv[i]);
        if (!w) return std::nullopt;
        if (!w->empty() && ((*w)[0] == '-' || *w == "(" || *w == "!")) break;
        starts.push_back(*w);
    }
    if (starts.size() > 1) return std::nullopt;
    a.path = starts.empty() ? "." : starts[0];
    for (; i < c.argv.size(); ++i) {
        const std::string* w = lit(c.argv[i]);
        if (!w) return std::nullopt;
        auto next = [&]() -> const std::string* {
            return i + 1 < c.argv.size() ? lit(c.argv[++i]) : nullptr;
        };
        if (*w == "-name" || *w == "-iname") { const std::string* g = next(); if (!g || a.name_glob) return std::nullopt; a.name_glob = *g; }
        else if (*w == "-type") { const std::string* t = next(); if (!t || t->size() != 1) return std::nullopt; a.type = (*t)[0]; }
        else if (*w == "-maxdepth" || *w == "-mindepth") { if (!next()) return std::nullopt; }
        else if (*w == "-print" || *w == "-L" || *w == "-follow") {}
        else return std::nullopt;
    }
    return Parsed{std::move(a), false};   // find's output order is directory order
}

// ── git (read-only subcommands) ──────────────────────────────────────────────────

std::optional<Parsed> parse_git(const Command& c) {
    std::size_t i = 1;
    while (i < c.argv.size()) {           // global options
        const std::string* w = lit(c.argv[i]);
        if (!w) return std::nullopt;
        if (w->empty() || (*w)[0] != '-') break;
        if (*w == "--no-pager" || *w == "-P" || *w == "--no-optional-locks") { ++i; continue; }
        return std::nullopt;              // -C dir / -c k=v: context we don't model
    }
    if (i >= c.argv.size()) return std::nullopt;
    const std::string* sub = lit(c.argv[i]);
    if (!sub) return std::nullopt;
    static constexpr std::string_view ro[] = {
        "status", "log", "diff", "show", "blame", "rev-parse", "ls-files", "describe",
        "shortlog", "reflog", "cat-file", "ls-tree", "grep", "merge-base", "rev-list",
        "name-rev", "whatchanged", "diff-tree", "show-ref", "for-each-ref",
    };
    // branch/tag/stash/remote are read-only only when listing.
    GitReadAction a;
    a.sub = *sub;
    for (std::size_t k = i + 1; k < c.argv.size(); ++k) {
        const std::string* w = lit(c.argv[k]);
        if (!w) return std::nullopt;
        a.args.push_back(*w);
    }
    auto only_flags = [&](std::initializer_list<std::string_view> ok) {
        for (const auto& w : a.args)
            if (!(w.starts_with('-') && std::ranges::find(ok, std::string_view{w}) != ok.end()))
                return false;
        return true;
    };
    const bool listing =
        (*sub == "branch" && only_flags({"-a", "-r", "-v", "-vv", "--list", "-l", "--show-current", "--all"}))
        || (*sub == "tag" && only_flags({"-l", "--list", "-n"}))
        || (*sub == "remote" && only_flags({"-v", "--verbose"}))
        || (*sub == "stash" && !a.args.empty() && (a.args[0] == "list" || a.args[0] == "show"));
    if (!listing && std::ranges::find(ro, *sub) == std::end(ro)) return std::nullopt;
    // --output=FILE on diff/log writes a file.
    for (const auto& w : a.args) if (w.starts_with("--output")) return std::nullopt;
    return Parsed{std::move(a), false};
}

std::optional<Parsed> translate(const Command& c) {
    const auto p = c.program();
    if (p == "cat")                        return parse_cat(c);
    if (p == "head")                       return parse_head_tail(c, false);
    if (p == "tail")                       return parse_head_tail(c, true);
    if (p == "sed")                        return parse_sed(c);
    if (p == "grep" || p == "egrep" || p == "fgrep") {
        auto r = parse_grep(c, false);
        if (r && p == "fgrep") if (auto* s = std::get_if<SearchAction>(&r->action)) { s->fixed = true; s->regex = false; }
        if (r && p == "egrep") if (auto* s = std::get_if<SearchAction>(&r->action)) s->extended = true;
        // GNU egrep/fgrep print "warning: egrep is obsolescent" on stderr,
        // which the shell tool captures — a native answer can't match that.
        if (r && p != "grep") r->exact = false;
        return r;
    }
    if (p == "rg")                         return parse_grep(c, true);
    if (p == "ls")                         return parse_ls(c);
    if (p == "wc")                         return parse_wc(c);
    if (p == "find")                       return parse_find(c);
    if (p == "git")                        return parse_git(c);
    return std::nullopt;
}

// A later pipeline stage: modelled shaping, or opaque.
Shape shape_of(const Command& c) {
    const auto p = c.program();
    Flags f{c};
    std::vector<std::string> ops;
    if (f.ok()) {
        if (p == "head" || p == "tail") {
            std::optional<std::int64_t> n;
            bool plus = false;
            const bool ok = f.walk("n", {"--lines"}, [&](std::string_view k, const std::optional<std::string>& v) {
                if ((k == "-n" || k == "--lines" || k == "-N") && v) {
                    plus = v->starts_with('+');
                    n = to_int(*v);
                    return n.has_value();
                }
                return false;
            }, ops);
            if (ok && ops.empty() && !plus)
                return {p == "head" ? Shape::Kind::Head : Shape::Kind::Tail,
                        n ? (*n < 0 ? -*n : *n) : 10, {}};
        }
        if (p == "wc") {
            bool lines = false;
            const bool ok = f.walk("", {}, [&](std::string_view k, const std::optional<std::string>&) {
                lines = lines || k == "-l" || k == "--lines";
                return k == "-l" || k == "--lines";
            }, ops);
            if (ok && lines && ops.empty()) return {Shape::Kind::CountLines, 0, {}};
        }
        if (p == "sort") {
            const bool ok = f.walk("", {}, [](std::string_view, const std::optional<std::string>&) { return false; }, ops);
            if (ok && ops.empty()) return {Shape::Kind::Sort, 0, {}};
        }
        if (p == "uniq") {
            const bool ok = f.walk("", {}, [](std::string_view, const std::optional<std::string>&) { return false; }, ops);
            if (ok && ops.empty()) return {Shape::Kind::Uniq, 0, {}};
        }
        if (p == "grep" || p == "rg" || p == "egrep") {
            // `| grep [-E|-v|-i|-F] PAT`: a line filter. Only the flag-free,
            // non-regex form is modelled exactly; flagged forms are typed
            // Filter but carry the flags in `arg` so display is honest and
            // `exact` is off (the arg isn't a plain substring).
            std::string fl;
            bool plain = true;
            const bool ok = f.walk("e", {"--regexp"}, [&](std::string_view k, const std::optional<std::string>& v) {
                if (k == "-e" || k == "--regexp") { if (v) ops.insert(ops.begin(), *v); return v.has_value(); }
                if (k == "-E" || k == "-v" || k == "-i" || k == "-F" || k == "-w" || k == "-x" || k == "-o"
                    || k == "-c" || k == "-n" || k == "-a" || k == "--color" || k == "-P") {
                    fl += std::string{k} + " "; plain = false; return true;
                }
                return false;
            }, ops);
            if (ok && ops.size() == 1)
                return {plain ? Shape::Kind::Filter : Shape::Kind::Opaque, 0,
                        plain ? ops[0] : "grep " + fl + ops[0]};
        }
    }
    return {Shape::Kind::Opaque, 0, std::string{p}};
}

bool is_noop(const Command& c) {
    static constexpr std::string_view noop[] = {
        "echo", "printf", "true", ":", "pwd", "sleep", "date", "clear", "which", "type",
    };
    const auto p = c.program();
    if (p == "command" && c.argv.size() > 1)
        if (const std::string* w = lit(c.argv[1]); w && (*w == "-v" || *w == "-V")) return true;
    return std::ranges::find(noop, p) != std::end(noop);
}

} // namespace

Plan plan(const Script& s) {
    Plan out;
    // Walk top-level commands grouped by pipeline id, in source order. `cd X`
    // (literal, top-level, not in a pipe) updates the tracked cwd for later
    // steps so `cd src && sed -n 1,9p a.cpp` reads src/a.cpp.
    std::string cwd;
    bool cwd_known = true;
    const auto tops = s.top_level();
    for (std::size_t i = 0; i < tops.size();) {
        const Command& head = *tops[i];
        std::size_t j = i + 1;
        while (j < tops.size() && tops[j]->pipeline == head.pipeline && tops[j]->stage > 0) ++j;
        std::vector<const Command*> stages(tops.begin() + static_cast<std::ptrdiff_t>(i),
                                           tops.begin() + static_cast<std::ptrdiff_t>(j));
        i = j;

        if (head.program() == "cd" && stages.size() == 1) {
            const std::string* d = head.argv.size() == 2 ? lit(head.argv[1]) : nullptr;
            if (d && !d->empty() && *d != "-") cwd = join_path(cwd, *d);
            else cwd_known = false;
            continue;
        }
        if (stages.size() == 1 && is_noop(head)) continue;

        Step st;
        st.begin = head.begin;
        st.end = stages.back()->end;
        auto parsed = translate(head);
        if (parsed) {
            // resolve paths against the tracked cwd
            std::visit([&](auto& a) {
                using A = std::decay_t<decltype(a)>;
                if constexpr (std::is_same_v<A, ReadAction> || std::is_same_v<A, ListAction>)
                    a.path = join_path(cwd, a.path);
                else if constexpr (std::is_same_v<A, SearchAction>)
                    for (auto& p : a.paths) p = join_path(cwd, p);
            }, parsed->action);
            st.action = std::move(parsed->action);
        } else {
            st.action = OtherAction{std::string{head.program()}};
        }
        bool exact = parsed && parsed->exact && cwd_known;
        for (std::size_t k = 1; k < stages.size(); ++k) {
            Shape sh = shape_of(*stages[k]);
            if (sh.kind == Shape::Kind::Opaque) exact = false;
            st.shapes.push_back(std::move(sh));
        }
        for (const Command* c : stages) {
            // Any dynamic word (glob, $VAR) means the argv the program sees
            // isn't known to us: fine for a typed reading, never exact.
            if (!c->fully_literal() || !c->env.empty()) exact = false;
            for (const auto& r : c->redirects) {
                // `2>/dev/null` is harmless; anything else changes the output
                // or writes a file.
                const std::string* t = lit(r.target);
                if (!(r.kind == Redirect::Kind::Out && r.fd == 2 && t && *t == "/dev/null"))
                    exact = false;
                if (r.writes_file()) {
                    // A step that writes a file is not inspection at all.
                    st.action = OtherAction{std::string{c->program()}};
                    exact = false;
                }
            }
            if (c->join == Join::PipeErr) exact = false;
        }
        st.exact = exact;
        out.steps.push_back(std::move(st));
    }
    return out;
}

bool Plan::pure_inspection() const noexcept {
    if (steps.empty()) return false;
    return std::ranges::all_of(steps, [](const Step& s) {
        return !std::holds_alternative<OtherAction>(s.action);
    });
}

bool Plan::all_exact() const noexcept {
    return !steps.empty() && std::ranges::all_of(steps, [](const Step& s) { return s.exact; });
}

std::string describe(const Step& st) {
    // Card label: must stay short. A 120-char regex or a 5-path search list
    // would push the raw command (shown after the label) off the row.
    auto clip = [](std::string s, std::size_t n) {
        if (s.size() <= n) return s;
        std::size_t cut = n;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 boundary
        return s.substr(0, cut) + "\xe2\x80\xa6";
    };
    return std::visit([&](const auto& a) -> std::string {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, ReadAction>) {
            std::string r = "Read " + clip(a.path, 60);
            if (a.range.from_end && a.range.first) r += " (last " + std::to_string(*a.range.first) + " lines)";
            else if (a.range.first && a.range.last) r += ":" + std::to_string(*a.range.first) + "-" + std::to_string(*a.range.last);
            else if (a.range.first && *a.range.first > 1) r += ":" + std::to_string(*a.range.first) + "-";
            return r;
        } else if constexpr (std::is_same_v<A, SearchAction>) {
            std::string r = "Search `" + clip(a.pattern, 32) + "`";
            if (!a.paths.empty()) {
                r += " in " + clip(a.paths[0], 40);
                if (a.paths.size() > 1) r += " +" + std::to_string(a.paths.size() - 1);
            }
            if (!a.include_globs.empty()) r += " (" + clip(a.include_globs.front(), 16) + ")";
            return r;
        } else if constexpr (std::is_same_v<A, ListAction>) {
            std::string r = a.recursive ? "Find " : "List ";
            r += clip(a.path, 50);
            if (a.name_glob) r += " " + clip(*a.name_glob, 20);
            return r;
        } else if constexpr (std::is_same_v<A, GitReadAction>) {
            std::string r = "git " + a.sub;
            for (const auto& x : a.args) { if (r.size() > 40) { r += " \xe2\x80\xa6"; break; } r += " " + x; }
            return r;
        } else {
            return "Run " + a.program;
        }
    }, st.action);
}

std::string describe(const Plan& p) {
    if (p.steps.empty()) return {};
    if (p.steps.size() == 1) return describe(p.steps[0]);
    // Several steps: one line can't hold them all. Count by kind:
    // "3 searches, 1 read" — the raw command after the label says which.
    int reads = 0, searches = 0, lists = 0, gits = 0, other = 0;
    for (const auto& s : p.steps)
        std::visit([&](const auto& a) {
            using A = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<A, ReadAction>) ++reads;
            else if constexpr (std::is_same_v<A, SearchAction>) ++searches;
            else if constexpr (std::is_same_v<A, ListAction>) ++lists;
            else if constexpr (std::is_same_v<A, GitReadAction>) ++gits;
            else ++other;
        }, s.action);
    std::string r;
    auto add = [&](int n, const char* one, const char* many) {
        if (!n) return;
        if (!r.empty()) r += ", ";
        r += std::to_string(n) + " " + (n == 1 ? one : many);
    };
    add(searches, "search", "searches");
    add(reads, "read", "reads");
    add(lists, "listing", "listings");
    add(gits, "git query", "git queries");
    add(other, "command", "commands");
    return r;
}

std::string native_call(const Step& st) {
    // Shapes that fold into the call: a leading Head/Tail bound. Anything
    // after that (a filter, a count) the native tool doesn't take — still
    // suggest the call, the bound just stays on the model's side.
    std::optional<std::int64_t> head, tail;
    if (!st.shapes.empty()) {
        if (st.shapes.front().kind == Shape::Kind::Head) head = st.shapes.front().n;
        else if (st.shapes.front().kind == Shape::Kind::Tail) tail = st.shapes.front().n;
    }
    auto q = [](const std::string& s) {
        // Quote only when needed, the way a model would type it.
        if (!s.empty() && s.find_first_of(" \t\"'`$\\") == std::string::npos) return s;
        std::string o = "\"";
        for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
        return o + "\"";
    };
    return std::visit([&](const auto& a) -> std::string {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, ReadAction>) {
            std::string c = "read path=" + q(a.path);
            const auto& r = a.range;
            if (r.from_end && r.first) c += " offset=-" + std::to_string(*r.first);
            else if (tail) c += " offset=-" + std::to_string(*tail);
            else {
                if (r.first && *r.first > 1) c += " start_line=" + std::to_string(*r.first);
                if (r.last) c += " end_line=" + std::to_string(*r.last);
                else if (head) c += " limit=" + std::to_string(*head);
            }
            return c;
        } else if constexpr (std::is_same_v<A, SearchAction>) {
            std::string c = "grep pattern=" + q(a.pattern);
            if (a.paths.size() == 1) c += " path=" + q(a.paths[0]);
            if (!a.include_globs.empty()) c += " glob=" + q(a.include_globs.front());
            if (!a.ignore_case) c += " case_sensitive=true";
            if (a.word) c += " word=true";
            if (a.files_only) c += " output=files";
            else if (a.count) c += " output=count";
            if (a.context_before || a.context_after)
                c += " context=" + std::to_string(std::max(a.context_before, a.context_after));
            // grep pages its own output (20 matches + offset=) — no limit arg.
            return c;
        } else if constexpr (std::is_same_v<A, ListAction>) {
            if (a.recursive && a.name_glob)
                return "glob pattern=" + q(a.name_glob->find('/') == std::string::npos && a.path != "."
                                               ? a.path + "/**/" + *a.name_glob : *a.name_glob);
            if (a.path.find(' ') != std::string::npos) return {};   // several paths
            return "list_dir path=" + q(a.path) + (a.recursive ? " recursive=true" : "");
        } else {
            return {};
        }
    }, st.action);
}

std::string_view category(const Plan& p) noexcept {
    if (p.steps.empty()) return "none";
    std::string_view cat;
    for (const auto& s : p.steps) {
        const std::string_view c = std::visit([](const auto& a) -> std::string_view {
            using A = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<A, ReadAction>) return "read";
            else if constexpr (std::is_same_v<A, SearchAction>) return "search";
            else if constexpr (std::is_same_v<A, ListAction>) return "list";
            else if constexpr (std::is_same_v<A, GitReadAction>) return "git";
            else return "other";
        }, s.action);
        if (cat.empty()) cat = c;
        else if (cat != c) return "mixed";
    }
    return cat;
}

// ═════════════════════════════════════════════════════════════════════════
//  7. Native execution
// ═════════════════════════════════════════════════════════════════════════
//
// Semantics are pinned to GNU coreutils/sed by a differential test that runs
// both paths over real files (no-trailing-newline, empty, CRLF, blank lines,
// ranges past EOF, inverted sed ranges). The rules the code below encodes:
//   * a "line" is a run of bytes ended by \n; a final unterminated run is a
//     line too, and it is emitted WITHOUT a newline (cat/head/tail/sed all
//     preserve the file's missing final newline);
//   * sed -n 'A,Bp' with B < A prints line A only; ranges past EOF print
//     what exists; `A,+N` is A..A+N;
//   * wc -l counts \n bytes, prints "N\n".

namespace {

// A byte string cut into lines, each keeping its own terminator (or none).
struct Lines {
    std::vector<std::string_view> v;
    static Lines of(std::string_view s) {
        Lines l;
        std::size_t i = 0;
        while (i < s.size()) {
            const auto nl = s.find('\n', i);
            const std::size_t e = nl == std::string_view::npos ? s.size() : nl + 1;
            l.v.push_back(s.substr(i, e - i));
            i = e;
        }
        return l;
    }
    [[nodiscard]] std::string join(std::size_t b, std::size_t e) const {
        std::string o;
        for (std::size_t i = b; i < e && i < v.size(); ++i) o.append(v[i]);
        return o;
    }
};

// The read a ReadAction denotes, applied to a file's bytes.
std::string apply_read(std::string_view bytes, const LineRange& r) {
    const Lines L = Lines::of(bytes);
    const std::size_t n = L.v.size();
    if (r.from_end) {                                       // tail -N
        const std::size_t k = static_cast<std::size_t>(std::max<std::int64_t>(0, r.first.value_or(10)));
        return L.join(n > k ? n - k : 0, n);
    }
    const std::int64_t a = r.first.value_or(1);
    if (a < 1) return {};
    std::int64_t b = r.last ? *r.last : static_cast<std::int64_t>(n);
    if (r.clamp_inverted && r.last && b < a) b = a;         // sed: B<A → line A only
    const std::size_t lo = static_cast<std::size_t>(a - 1);
    const std::size_t hi = static_cast<std::size_t>(std::max<std::int64_t>(b, 0));
    if (lo >= n || hi <= lo) return {};                     // incl. head -0
    return L.join(lo, std::min(hi, n));
}

std::optional<std::string> apply_shape(const std::string& in, const Shape& sh) {
    const Lines L = Lines::of(in);
    const std::size_t n = L.v.size();
    switch (sh.kind) {
        case Shape::Kind::Head: {
            const auto k = static_cast<std::size_t>(std::max<std::int64_t>(0, sh.n));
            return L.join(0, std::min(k, n));
        }
        case Shape::Kind::Tail: {
            const auto k = static_cast<std::size_t>(std::max<std::int64_t>(0, sh.n));
            return L.join(n > k ? n - k : 0, n);
        }
        case Shape::Kind::CountLines:
            return std::to_string(std::count(in.begin(), in.end(), '\n')) + "\n";
        default:
            return std::nullopt;       // sort/uniq/filter: locale-dependent; decline
    }
}

// Read a whole regular file. Declines (nullopt) on anything where the shell's
// output involves an error message or a special file we'd have to imitate.
std::optional<std::string> slurp_plain(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto st = fs::status(path, ec);
    if (ec || !fs::is_regular_file(st)) return std::nullopt;   // missing/dir/fifo/dev
    const auto sz = fs::file_size(path, ec);
    if (ec || sz > (64u << 20)) return std::nullopt;           // huge: let the shell stream it
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;                                // EACCES etc.
    std::string s(static_cast<std::size_t>(sz), '\0');
    f.read(s.data(), static_cast<std::streamsize>(sz));
    if (static_cast<std::uintmax_t>(f.gcount()) != sz) return std::nullopt;   // raced
    return s;
}

// ── native grep ──────────────────────────────────────────────────────────────────────
//
// One file, no recursion, and a pattern that is a set of LITERALS: either a
// plain string, or (BRE) `a\|b\|c` / (-E) `a|b|c` alternatives of plain
// strings. That is 55% of real single-file greps and needs no regex engine,
// so there is nothing to disagree with GNU about. Supported flags: -n -c -i
// (ASCII-only pattern AND file — GNU folds Unicode case, we don't) -w -F.
// Everything else (-A/-B/-C context, -o, -v, -x, -l, anchors, classes,
// backrefs, `.`, `*`) declines to the real grep.

struct Alternatives { std::vector<std::string> lits; };

// Split a pattern into literal alternatives, or nullopt if it uses any regex
// feature beyond `|` alternation. `ere` = -E syntax, `fixed` = -F (no
// metachars at all; a newline would split, but argv can't hold one usefully).
std::optional<Alternatives> literal_alternatives(std::string_view p, bool ere, bool fixed) {
    Alternatives out;
    if (fixed) {
        if (p.find('\n') != std::string_view::npos) return std::nullopt;
        out.lits.emplace_back(p);
        return out;
    }
    std::string cur;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\\') {
            if (i + 1 >= p.size()) return std::nullopt;
            const char n = p[i + 1];
            if (!ere && n == '|') { out.lits.push_back(std::move(cur)); cur.clear(); ++i; continue; }
            // `\.` `\/` `\-` etc.: an escaped punctuation char is itself.
            // Escaped letters/digits are classes, anchors or backrefs.
            if (std::isalnum(static_cast<unsigned char>(n)) || n == '<' || n == '>' || n == '{' || n == '}'
                || (!ere && (n == '(' || n == ')' || n == '+' || n == '?')))
                return std::nullopt;
            cur += n; ++i;
            continue;
        }
        if (ere && c == '|') { out.lits.push_back(std::move(cur)); cur.clear(); continue; }
        // Metacharacters of BRE; ERE adds + ? ( ) { }.
        if (c == '.' || c == '[' || c == ']' || c == '*' || c == '^' || c == '$') return std::nullopt;
        if (ere && (c == '+' || c == '?' || c == '(' || c == ')' || c == '{' || c == '}')) return std::nullopt;
        cur += c;
    }
    out.lits.push_back(std::move(cur));
    // An empty alternative matches every line — fine, but rare and easy to
    // get subtly wrong with -w; decline.
    for (const auto& l : out.lits) if (l.empty()) return std::nullopt;
    return out;
}

bool is_ascii(std::string_view s) noexcept {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

char lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// GNU -w: a match counts only if the chars around it are not word chars
// (alnum or _). GNU retries later/shorter matches in the same line; with
// literals, "try every occurrence of every alternative" is exactly that.
bool line_matches(std::string_view line, const Alternatives& alt, bool icase, bool word) {
    auto is_w = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (const auto& lit : alt.lits) {
        if (lit.size() > line.size()) continue;
        for (std::size_t i = 0; i + lit.size() <= line.size(); ++i) {
            bool eq = true;
            for (std::size_t k = 0; k < lit.size(); ++k) {
                char a = line[i + k], b = lit[k];
                if (icase) { a = lower_ascii(a); b = lower_ascii(b); }
                if (a != b) { eq = false; break; }
            }
            if (!eq) continue;
            if (!word) return true;
            const bool left_ok = i == 0 || !is_w(line[i - 1]);
            const bool right_ok = i + lit.size() == line.size() || !is_w(line[i + lit.size()]);
            if (left_ok && right_ok) return true;
        }
    }
    return false;
}

struct GrepOut { std::string text; int status; };

// Run the search over one file's bytes. status follows grep: 0 = matched,
// 1 = no match.
std::optional<GrepOut> native_grep(const SearchAction& a, bool ere, std::string_view bytes) {
    if (a.recursive || a.paths.size() != 1 || a.files_only || a.context_before || a.context_after)
        return std::nullopt;
    auto alt = literal_alternatives(a.pattern, ere, a.fixed);
    if (!alt) return std::nullopt;
    if (a.ignore_case && (!is_ascii(a.pattern) || !is_ascii(bytes))) return std::nullopt;
    const Lines L = Lines::of(bytes);
    GrepOut o{{}, 1};
    std::size_t count = 0;
    for (std::size_t i = 0; i < L.v.size(); ++i) {
        std::string_view line = L.v[i];
        const bool terminated = !line.empty() && line.back() == '\n';
        if (terminated) line.remove_suffix(1);
        if (!line_matches(line, *alt, a.ignore_case, a.word)) continue;
        ++count;
        if (a.count) continue;
        if (a.line_numbers) { o.text += std::to_string(i + 1); o.text += ':'; }
        o.text.append(line);
        o.text += '\n';          // grep always terminates an output line
    }
    if (a.count) o.text = std::to_string(count) + "\n";
    o.status = count ? 0 : 1;
    return o;
}

} // namespace

std::optional<NativeResult> native_run(std::string_view command, std::string_view cwd) {
    const Script s = analyze(command);
    if (!s.clean || s.truncated) return std::nullopt;
    // Anything nested, conditional, backgrounded or substituted: decline.
    for (const auto& c : s.commands)
        if ((static_cast<std::uint16_t>(c.ctx) & ~static_cast<std::uint16_t>(Ctx::Piped)) != 0)
            return std::nullopt;
    const Plan p = plan(s);
    if (p.steps.empty() || !p.all_exact()) return std::nullopt;
    // plan() drops no-op commands (echo, pwd, date, cd, printf …) because
    // they don't change what a call is FOR. But they PRINT. If any top-level
    // command didn't become a step, its output would silently vanish from a
    // native answer: decline. (Every step is one pipeline; count heads.)
    std::size_t heads = 0;
    for (const auto& c : s.commands) if (c.stage == 0) ++heads;
    if (heads != p.steps.size()) return std::nullopt;
    // Steps must run unconditionally in order: only `;`-style sequencing
    // between them. `a && b` / `a || b` depend on exit status we'd have to
    // model for every branch — fine when every step succeeds, which we
    // guarantee below (any would-be failure declines), so && is OK; || is
    // not (the second step would NOT run).
    for (const auto& c : s.commands) if (c.join == Join::Or) return std::nullopt;

    NativeResult out;
    for (std::size_t k = 0; k < p.steps.size(); ++k) {
        const auto& st = p.steps[k];
        std::string cur;
        int status = 0;
        if (const auto* sr = std::get_if<SearchAction>(&st.action)) {
            if (sr->paths.size() != 1) return std::nullopt;
            std::string path = sr->paths[0];
            if (!path.empty() && path.front() != '/' && !cwd.empty())
                path = std::string{cwd} + (cwd.back() == '/' ? "" : "/") + path;
            auto bytes = slurp_plain(path);
            if (!bytes || bytes->find('\0') != std::string::npos) return std::nullopt;
            auto g = native_grep(*sr, sr->extended, *bytes);
            if (!g) return std::nullopt;
            cur = std::move(g->text);
            status = g->status;
        } else if (const auto* rd = std::get_if<ReadAction>(&st.action)) {
            std::string path = rd->path;
            if (!path.empty() && path.front() != '/' && !cwd.empty())
                path = std::string{cwd} + (cwd.back() == '/' ? "" : "/") + path;
            auto bytes = slurp_plain(path);
            if (!bytes) return std::nullopt;
            // Binary content: cat would print it raw, and the capture path
            // then sanitises it — identical only if we reproduce that. Decline.
            if (bytes->find('\0') != std::string::npos) return std::nullopt;
            cur = apply_read(*bytes, rd->range);
        } else {
            return std::nullopt;                      // exact List/Git: not native yet
        }
        // A pipeline's status is its LAST stage's. Every modelled shape
        // (head/tail/wc -l) exits 0, so shapes reset it.
        for (const auto& sh : st.shapes) {
            auto next = apply_shape(cur, sh);
            if (!next) return std::nullopt;
            cur = std::move(*next);
            status = 0;
        }
        out.output += cur;
        out.exit_code = status;
        // `a && b` with a failing a does NOT run b. We could model that, but
        // a non-zero status in the MIDDLE of a chain is rare and subtle
        // (`set -e`-like expectations); decline instead of guessing.
        if (status != 0 && k + 1 < p.steps.size()) return std::nullopt;
    }
    return out;
}

} // namespace mcp::tools::util::shellx
