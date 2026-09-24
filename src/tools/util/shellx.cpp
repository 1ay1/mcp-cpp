// SPDX-License-Identifier: Apache-2.0
//
// shellx — parse (tree-sitter-bash) → lower to a flat typed Script → guard.
// See the header for the model. Organisation:
//   1. parser + node helpers
//   2. words: tree-sitter word nodes → Lit | Dyn
//   3. lowering: statements → Commands with context, joins, pipelines
//   4. unwrapping: sudo/env/timeout/…, bash -c '…', xargs, find -exec
//   5. guard
#include <mcp/tools/util/shellx.hpp>

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
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
// still guard the prefix (and the raw text) but give no advice.
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

} // namespace mcp::tools::util::shellx
