// SPDX-License-Identifier: Apache-2.0
//
// structural.cpp — search_structural and rewrite_structural: walk the tree,
// prefilter each file on the pattern's literals, run the shape matcher
// (shape.hpp), and render hits like grep (file:line, context, enclosing
// symbol).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/glob.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/utf8.hpp>
#include <mcp/tools/host.hpp>

#include "tool_shell.hpp"
#include "call_state.hpp"
#include "tool_body.hpp"
#include "shape.hpp"

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ExecResult;
using util::ToolError;
using util::ToolOutput;
using mcp::tools::FileChange;
using namespace shape;   // the matcher's types and entry points

namespace {

// ── Tunables ────────────────────────────────────────────────────────────
constexpr std::size_t kMaxFileBytes = 2u * 1024 * 1024;   // skip huge files
constexpr std::size_t kMaxScanned   = 20000;              // file-count ceiling
constexpr std::size_t kMaxMatches    = 200;               // result cap
constexpr std::size_t kMaxOutBytes   = 24u * 1024;        // output budget
constexpr int         kContext       = 1;                 // ± lines of context

// Shares to split a scan into; the host's splitter owns the threads.
std::size_t worker_count(const Call& call) {
    return std::min<std::size_t>(call.width(), 16);
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
                    if (s.size() > 90) s.resize(util::safe_utf8_cut(s, 90));
                    return s;
                }
            }
            // Keyword-less scope opener (C: `int main(…) {`).
            if (!s.empty() && (s.back() == '{' || s.back() == '(')
                && fallback.empty()) {
                fallback = s;
                if (fallback.size() > 90) fallback.resize(util::safe_utf8_cut(fallback, 90));
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
    std::string root = ".";
    std::string glob;    // optional extension/name filter
    bool        expand = false;   // return the whole enclosing block per match
    std::string display_description;
};

const util::ArgSpec<StructArgs> kStructArgs{
    util::arg(&StructArgs::pattern, "pattern",
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
              "recurses into groups).").required(),
    util::arg(&StructArgs::root, "path", "Directory to search (default: workspace root)."),
    util::arg(&StructArgs::glob, "glob", "Optional filename glob filter, e.g. '*.ts'."),
    util::arg(&StructArgs::expand, "expand",
              "When true, return the WHOLE enclosing function/block "
              "around each match instead of ±1 context line — so the "
              "result is self-sufficient and you rarely need a "
              "follow-up `read`. Default false."),
    util::arg(&StructArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
};

[[nodiscard]] bool is_blank(std::string_view s) noexcept {
    return s.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::expected<StructArgs, ToolError> check_struct(StructArgs a, const util::Bounds&) {
    if (is_blank(a.pattern))
        return std::unexpected(ToolError::invalid_args("pattern must not be blank"));
    return a;
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
int verify_lead(const FileSystem& files, const fs::path& ws_root, DocPassage& p) {
    if (p.path.empty()) return 0;
    fs::path f = fs::path{p.path}.is_absolute() ? fs::path{p.path}
                                                : ws_root / p.path;
    if (!files.stat(f).exists) return 0;
    std::string bytes = util::read_file(files, f);
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
std::string semantic_leads(const FileSystem& files, DocRetriever* sem, const StructArgs& a,
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
        if (!verify_lead(files, ws_root, p)) continue;   // stale → silently dropped
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
            if (ln.size() > 88) ln.resize(util::safe_utf8_cut(ln, 88));
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

ExecResult run_structural(const Call& call, const StructArgs& a, DocRetriever* sem) {
    auto wp = util::make_workspace_path_checked(a.root, "search_structural", bounds(call));
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

    // ── Collect candidate files ──
    std::vector<fs::path> files;
    files.reserve(4096);
    {
        call.fs().walk(wp->path(), [&](const DirEntry& e) {
            if (e.symlink) return e.directory ? WalkStep::skip_dir : WalkStep::next;
            if (e.directory)
                return util::should_skip_dir(e.path.filename().string()) ? WalkStep::skip_dir
                                                                          : WalkStep::next;
            const fs::path& p = e.path;
            if (!is_source_ext(p)) return WalkStep::next;
            if (!a.glob.empty() && !util::glob_match(a.glob, p.filename().string()))
                return WalkStep::next;
            files.push_back(p);
            return files.size() >= kMaxScanned ? WalkStep::stop : WalkStep::next;
        });
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
    // Share k scans files k, k+n, … and counts its own hits against its slice
    // of the cap, so the shares share nothing while they run.
    std::vector<FileMatch> results(files.size());
    const std::size_t nshares = std::max<std::size_t>(1, std::min(worker_count(call), files.size()));
    const std::size_t share_cap = std::max<std::size_t>(1, kMaxMatches / nshares);
    std::vector<std::size_t> counted(nshares, 0);

    auto worker = [&](std::size_t k) {
        std::size_t& total_hits = counted[k];
        for (std::size_t idx = k; idx < files.size() && total_hits < share_cap; idx += nshares) {
            const fs::path& p = files[idx];

            // Exception wall: an escape from a share is std::terminate.
            // A file that trips anything (I/O race, allocation, matcher
            // corner) is skipped; the scan completes.
            try {
                std::error_code ec;
                auto sz = call.fs().stat(p).size;
                if (ec || sz == 0 || sz > kMaxFileBytes) continue;
                if (util::is_binary_file(call.fs(), p)) continue;

                std::string bytes = util::read_file(call.fs(), p);
                if (bytes.empty()) continue;

                Lang lang = lang_of(p);
                int key = static_cast<int>(lang);
                const auto& gates = gates_by_lang[key];
                if (!passes_prefilter(bytes, gates)) continue;

                auto toks = tokenize(bytes, lang);
                auto tree = build_tree(toks);
                auto hits = match_file(pat_by_lang[key], tree);
                if (hits.empty()) continue;

                auto rel = p.lexically_relative(wp->path());   // walked from root
                results[idx] = FileMatch{
                    rel.empty() ? p.string() : rel.generic_string(),
                    std::move(hits),
                };
                total_hits += results[idx].hits.size();
            } catch (...) { /* skip this file */ }
        }
    };

    if (!files.empty()) call.split(nshares, worker);
    bool capped = false;
    for (auto c : counted) capped = capped || c >= share_cap;

    // ── Render ───────────────────────────────────────────────────────────
    std::size_t match_count = 0, file_count = 0;
    for (auto& fm : results) if (!fm.rel.empty()) { ++file_count; match_count += fm.hits.size(); }

    if (match_count == 0) {
        std::string msg =
            "no structural matches. The pattern must match the code SHAPE "
            "token-for-token (metavariables $X / $$$X match one / many). If you "
            "wanted a text match, use `grep` instead.";
        // RAG proposes: append verified semantic neighbours as leads.
        msg += semantic_leads(call.fs(), sem, a, probe_pat, wp->path());
        return ToolOutput{std::move(msg), std::nullopt};
    }

    // More matches than the output budget renders → semantic proximity picks
    // WHICH files render first. The hit set itself is untouched (soundness);
    // only presentation order changes, and only when truncation is possible.
    if (match_count > 20 || capped)
        semantic_order(sem, a, probe_pat, results);

    std::ostringstream out;
    out << "Found " << match_count << " structural match(es) in "
        << file_count << " file(s):\n";

    std::size_t emitted = 0;
    bool truncated = false;
    for (auto& fm : results) {
        if (fm.rel.empty()) continue;
        // Re-read for context rendering (cheap; only matched files).
        std::string bytes = util::read_file(call.fs(), fs::path{a.root} / fm.rel);
        if (bytes.empty()) bytes = util::read_file(call.fs(), fs::path{fm.rel});
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
                        << (src.size() > 200 ? src.substr(0, util::safe_utf8_cut(src, 200)) + "…" : src) << "\n";
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
    std::string root = ".";
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

const util::ArgSpec<RewriteArgs> kRewriteArgs{
    util::arg(&RewriteArgs::pattern, "pattern",
              "The code SHAPE to find — same language as search_structural "
              "($NAME binds one node, $$$NAME binds a run, back-refs must "
              "match). Every match's exact byte span is replaced.").required(),
    util::arg(&RewriteArgs::rewrite, "rewrite",
              "Replacement template. $NAME / $$$NAME splice the VERBATIM "
              "source bytes that metavariable captured (original spacing "
              "kept). Example: pattern 'assertEquals($A, $B)' rewrite "
              "'assertEqual($B, $A)' swaps the arguments at every call "
              "site. Every $NAME in the template must be bound by the "
              "pattern.").required(),
    util::arg(&RewriteArgs::root, "path", "Directory to rewrite under (default: workspace root)."),
    util::arg(&RewriteArgs::glob, "glob", "Optional filename glob filter, e.g. '*.py'."),
    util::arg(&RewriteArgs::apply, "apply",
              "false (default) = DRY RUN: show every before/after "
              "pair without touching disk. true = write the files "
              "(atomic per file). ALWAYS dry-run first and read the "
              "preview before applying."),
    util::arg(&RewriteArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
};

std::expected<RewriteArgs, ToolError> check_rewrite(RewriteArgs a, const util::Bounds&) {
    if (is_blank(a.pattern))
        return std::unexpected(ToolError::invalid_args("pattern must not be blank"));
    return a;
}

ExecResult run_rewrite(const Call& call, const RewriteArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "rewrite_structural", bounds(call));
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
        call.fs().walk(wp->path(), [&](const DirEntry& e) {
            if (e.symlink) return e.directory ? WalkStep::skip_dir : WalkStep::next;
            if (e.directory)
                return util::should_skip_dir(e.path.filename().string()) ? WalkStep::skip_dir
                                                                          : WalkStep::next;
            const fs::path& p = e.path;
            if (!is_source_ext(p)) return WalkStep::next;
            if (!a.glob.empty() && !util::glob_match(a.glob, p.filename().string()))
                return WalkStep::next;
            files.push_back(p);
            return files.size() >= kMaxScanned ? WalkStep::stop : WalkStep::next;
        });
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
    std::size_t total_repl = 0, files_changed = 0;
    bool truncated = false;
    std::optional<FileChange> change;   // carried for single-file applies

    for (const auto& p : files) {
        if (total_repl >= kMaxMatches) { truncated = true; break; }
        std::error_code ec;
        auto sz = call.fs().stat(p).size;
        if (ec || sz == 0 || sz > kMaxFileBytes) continue;
        if (util::is_binary_file(call.fs(), p)) continue;
        std::string bytes = util::read_file(call.fs(), p);
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
        auto rel = p.lexically_relative(wp->path());   // walked from root
        std::string relname = rel.empty() ? p.string() : rel.generic_string();

        for (const Hit* h : kept) {
            auto rendered = render_template(tpl, h->binds, bytes);
            if (!rendered) continue;
            if (total_repl >= kMaxMatches) { truncated = true; break; }
            if (applied_here == 0) out << "\n" << relname << ":\n";
            // Preview: the before/after of the spliced region, single-line
            // clipped — enough to eyeball correctness without a full diff.
            auto clip = [](std::string s) {
                for (auto& c : s) if (c == '\n') c = ' ';
                if (s.size() > 120) { s.resize(util::safe_utf8_cut(s, 119)); s += "\xe2\x80\xa6"; }
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
            if (auto err = util::write_file(call.fs(), p, rebuilt); !err.empty())
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
           kStructArgs.schema(),
           EffectSet{Effect::ReadFs},
           body<StructArgs>([sem](const Call& c, const StructArgs& a) {
               return run_structural(c, a, sem.get());
           }, kStructArgs, check_struct),
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
           kRewriteArgs.schema(),
           EffectSet{Effect::ReadFs, Effect::WriteFs},
           body<RewriteArgs>(run_rewrite, kRewriteArgs, check_rewrite),
           /*token_budget=*/30'000);
}

} // namespace mcp::tools::detail
