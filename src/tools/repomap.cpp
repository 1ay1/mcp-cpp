// SPDX-License-Identifier: Apache-2.0
//
// repomap.cpp — register_repo_map_tool: a token-budgeted, ranked skeleton of
// the codebase (aider's "repository map", the single most cost-effective
// technique for working in HUGE repos: 15B tokens/week validated upstream).
//
// WHY THIS TOOL EXISTS
// ════════════════════
// On a large unfamiliar repo, an agent's exploration (read/grep fan-out) is
// 45-60% of total token spend (FastContext, SWE-Pruner measurements). A repo
// map answers "what's IN here and what matters?" in ONE call at a fixed
// budget: every source file becomes a node, references to symbols defined in
// other files become edges, PageRank ranks the graph, and the output is the
// top files with their DEFINITION SIGNATURES (one line per symbol) packed
// greedily until the byte budget is exhausted.
//
// The optional `focus` argument personalizes the ranking (aider's chat-file
// personalization): files whose path or symbols match the focus terms get
// boosted restart probability, so the map orients around the task at hand
// rather than the global god-nodes.
//
// DEP-FREE BY DESIGN: no tree-sitter. Definitions come from the same
// language-family regex set find_definition uses; references are identifier
// occurrences of known defined names. That's coarser than an AST but captures
// the shape PageRank needs, costs nothing to build, and works on ANY text —
// including in-house DSLs the regexes have never seen (their files still
// contribute path/identifier edges).
//
// CACHED per workspace signature (Σ size+mtime over scanned files): the walk
// re-stats every call (cheap), the parse+graph only rebuilds on change.

#include "tool_shell.hpp"

#include <mcp/tools/util/fs_helpers.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcp::tools::detail {

namespace {

namespace fs = std::filesystem;

// ── Source-file filter (mirrors find_definition's language set, plus a few
//    config/DSL-ish extensions that carry identifiers worth graphing). ──
bool is_map_source(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    static const char* kExts[] = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx",
        ".py", ".js", ".ts", ".jsx", ".tsx", ".mjs",
        ".go", ".rs", ".java", ".kt", ".rb", ".swift", ".zig", ".lua",
        ".cs", ".scala", ".ex", ".exs", ".ml", ".hs", ".proto",
    };
    for (auto* e : kExts) if (ext == e) return true;
    return false;
}

// One regex per language family; group 1 = the defined symbol name.
const std::vector<std::regex>& def_patterns() {
    static const std::vector<std::regex> kPatterns = [] {
        std::vector<std::regex> v;
        v.emplace_back(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|enum(?:\s+class)?|union|namespace|interface|trait|impl|module)\s+([A-Za-z_]\w*))",
                       std::regex::optimize);
        v.emplace_back(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:def|fn|func|function)\s+([A-Za-z_]\w*))",
                       std::regex::optimize);
        v.emplace_back(R"(^\s*(?:export\s+)?(?:async\s+)?(?:const|let|var|type)\s+([A-Za-z_]\w*)\s*[=:<])",
                       std::regex::optimize);
        v.emplace_back(R"(^[A-Za-z_][\w:<>,\s\*&]*?\s+([A-Za-z_]\w*)\s*\([^;]*(?:\)\s*(?:const\s*)?(?:noexcept\s*)?\{|\)\s*$))",
                       std::regex::optimize);
        v.emplace_back(R"(^\s*#define\s+([A-Za-z_]\w*))", std::regex::optimize);
        return v;
    }();
    return kPatterns;
}

struct Def {
    std::string name;
    int         line = 0;
    std::string signature;   // the trimmed definition line
    // Inbound references from OTHER files (filled in the linking pass) — how
    // many distinct files mention this symbol. Drives both edge weight and
    // the symbol-level rank used to pack the budget aider-style: a widely-
    // referenced def in a mid-ranked file beats 20 trivial defs in a
    // god-file.
    int         inbound = 0;
    double      rank = 0.0;  // this symbol's share of its file's PageRank
};

struct FileNode {
    std::string      rel;      // workspace-relative path
    std::vector<Def> defs;
    // Names referenced in this file that are DEFINED in another file — the
    // outbound edges, weighted by sqrt(#refs)/#definers. Filled in the
    // linking pass.
    std::unordered_map<std::uint32_t, double> out_edges;   // file idx → weight
    double rank = 0.0;
};

struct RepoGraph {
    std::vector<FileNode> files;
    std::uint64_t         signature = 0;   // Σ(size+mtime) stat fingerprint
};

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r')) --e;
    return std::string{s.substr(b, e - b)};
}

// Identifier tokenizer for the reference pass: [A-Za-z_]\w+, ≥3 chars.
void identifiers(std::string_view text, std::unordered_set<std::string>& out) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        char c = text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            std::size_t b = i;
            while (i < n && ((text[i] >= 'a' && text[i] <= 'z')
                          || (text[i] >= 'A' && text[i] <= 'Z')
                          || (text[i] >= '0' && text[i] <= '9')
                          || text[i] == '_')) ++i;
            if (i - b >= 3) out.emplace(text.substr(b, i - b));
        } else {
            ++i;
        }
    }
}

// Stop-list: identifiers so common they'd wire every file to every file,
// PLUS control-flow / operator keywords that the greedy C-family function
// regex would otherwise mis-capture as a "function name" (`if (x)`,
// `while (y)`, `switch (z)`, `return foo()`, `sizeof(T)`, `catch (e)`). A
// def whose captured name is one of these is a false positive — dropping it
// keeps the graph honest and stops phantom `L42: if (...)` signature lines.
bool is_stopword(const std::string& s) {
    static const std::unordered_set<std::string> kStop = {
        "int", "char", "bool", "void", "auto", "const", "static", "return",
        "true", "false", "null", "nullptr", "None", "self", "this", "std",
        "string", "size_t", "vector", "for", "while", "else", "break",
        "continue", "public", "private", "protected", "class", "struct",
        "def", "function", "import", "from", "include", "namespace", "using",
        "new", "delete", "sizeof", "typedef", "template", "typename",
        // Control-flow / operator keywords: never a real definition name, but
        // the `<type> <name>(...)` function regex captures them from lines
        // like `if (cond) {` or `switch (x) {`.
        "if", "switch", "catch", "case", "do", "goto", "throw", "try",
        "where", "async", "await", "yield", "defer", "select", "match",
        "and", "not", "with", "assert", "print", "println", "printf",
        "sizeof", "alignof", "decltype", "static_cast", "reinterpret_cast",
        "dynamic_cast", "const_cast", "co_await", "co_return", "co_yield",
    };
    return kStop.contains(s);
}

// Build (or reuse) the def/ref graph for `root`. Guarded by the caller's
// mutex; the cache holds one graph per process (workspace tools are
// single-rooted in practice).
const RepoGraph& build_graph(const fs::path& root) {
    static RepoGraph g;
    static std::string g_root;

    // Walk pass 1: enumerate files + stat signature.
    struct Cand { fs::path abs; std::string rel; std::uint64_t sig; };
    std::vector<Cand> cands;
    std::uint64_t sig = 0;
    std::error_code ec;
    constexpr std::size_t kMaxFiles = 5000;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && cands.size() < kMaxFiles;
         it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        std::error_code e2;
        if (entry.is_directory(e2)) {
            auto name = entry.path().filename().string();
            if (util::should_skip_dir(name)
                || (it.depth() > 0 && name.starts_with(".")))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(e2)) continue;
        if (!is_map_source(entry.path())) continue;
        auto sz = entry.file_size(e2);
        if (e2 || sz > 512 * 1024) continue;   // skip minified/generated
        auto mt = fs::last_write_time(entry.path(), e2);
        std::uint64_t fsig = static_cast<std::uint64_t>(sz)
            ^ static_cast<std::uint64_t>(mt.time_since_epoch().count());
        sig = sig * 1099511628211ull + fsig;
        auto rel = fs::relative(entry.path(), root, e2).string();
        if (rel.empty()) rel = entry.path().filename().string();
        cands.push_back({entry.path(), std::move(rel), fsig});
    }

    if (g_root == root.string() && g.signature == sig && !g.files.empty())
        return g;   // unchanged — reuse the parsed graph

    // Parse pass: definitions + raw text (kept transiently for linking).
    g = RepoGraph{};
    g.signature = sig;
    g_root = root.string();
    std::vector<std::string> bodies;
    bodies.reserve(cands.size());
    const auto& pats = def_patterns();
    for (auto& c : cands) {
        std::ifstream in(c.abs, std::ios::binary);
        if (!in) continue;
        std::string body((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        FileNode node;
        node.rel = std::move(c.rel);
        // Line scan for definitions.
        std::size_t pos = 0;
        int lineno = 0;
        constexpr std::size_t kMaxDefsPerFile = 64;
        while (pos <= body.size() && node.defs.size() < kMaxDefsPerFile) {
            auto eol = body.find('\n', pos);
            std::string_view line{body.data() + pos,
                (eol == std::string::npos ? body.size() : eol) - pos};
            ++lineno;
            if (!line.empty() && line.size() < 500) {
                std::string ls{line};
                std::smatch m;
                for (const auto& re : pats) {
                    if (std::regex_search(ls, m, re) && m.size() >= 2) {
                        std::string name = m[1].str();
                        if (!is_stopword(name) && name.size() >= 3) {
                            std::string sigline = trim(line);
                            if (sigline.size() > 120) sigline.resize(120);
                            node.defs.push_back(
                                {std::move(name), lineno, std::move(sigline)});
                        }
                        break;
                    }
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        g.files.push_back(std::move(node));
        bodies.push_back(std::move(body));
    }

    // Linking pass: symbol name → defining file(s), then per-file identifier
    // scan produces the reference edges. We also count, per referenced symbol,
    // how many DISTINCT files mention it — that count both (a) weights the
    // edge (aider: an edge carries sqrt(#refs), so a symbol used ten times is
    // a stronger vote than one used once, but sub-linearly so a single hub
    // can't dominate) and (b) accumulates onto the DEFINITION's `inbound`
    // tally, which later drives symbol-level budget packing.
    std::unordered_map<std::string, std::vector<std::uint32_t>> def_sites;
    for (std::uint32_t i = 0; i < g.files.size(); ++i)
        for (const auto& d : g.files[i].defs)
            def_sites[d.name].push_back(i);

    // First tally raw reference counts per (referencing-file, symbol) so we
    // can sqrt-scale the edge weight instead of adding one unit per identifier
    // occurrence. `idents` is a SET, so each symbol counts once per file —
    // presence, not frequency; that already matches aider's per-file model and
    // avoids a giant file inflating an edge purely by repetition.
    std::unordered_set<std::string> idents;
    std::unordered_map<std::string, int> global_refs;  // symbol → #referring files
    std::vector<std::unordered_set<std::string>> file_refs(g.files.size());
    for (std::uint32_t i = 0; i < g.files.size(); ++i) {
        idents.clear();
        identifiers(bodies[i], idents);
        for (const auto& id : idents) {
            if (is_stopword(id)) continue;
            if (!def_sites.count(id)) continue;
            file_refs[i].insert(id);
            ++global_refs[id];
        }
    }

    for (std::uint32_t i = 0; i < g.files.size(); ++i) {
        for (const auto& id : file_refs[i]) {
            auto it = def_sites.find(id);
            if (it == def_sites.end()) continue;
            // Symbols defined in many files are generic (a `run`/`build`/
            // `Config` everyone has). Rather than a hard cutoff that erases
            // them from the graph, down-weight: an ident defined in D files
            // contributes 1/D of an edge, so a symbol defined once is a
            // full-strength vote and a ubiquitous one barely moves the rank.
            const std::size_t defcount = it->second.size();
            if (defcount > 16) continue;   // pathological — still drop the worst
            // Edge weight: sqrt of how many files reference this symbol
            // (sub-linear), scaled down by how many files define it.
            const int refs = global_refs[id];
            double w = std::sqrt(static_cast<double>(std::max(refs, 1)))
                     / static_cast<double>(defcount);
            for (auto j : it->second) {
                if (j == i) continue;
                g.files[i].out_edges[j] += w;
                // Credit the specific definition in the defining file with an
                // inbound reference (used for symbol-level ranking below).
                for (auto& d : g.files[j].defs)
                    if (d.name == id) { ++d.inbound; break; }
            }
        }
    }
    return g;
}

// PageRank with optional personalization. Damping 0.85, 24 iterations —
// converged well past display precision at these graph sizes.
void pagerank(RepoGraph& g, const std::vector<double>& personalize) {
    const std::size_t n = g.files.size();
    if (n == 0) return;
    constexpr double kD = 0.85;
    std::vector<double> rank(n, 1.0 / static_cast<double>(n));
    std::vector<double> next(n, 0.0);
    // Restart distribution: uniform, or the caller's personalization.
    std::vector<double> restart(n, 1.0 / static_cast<double>(n));
    double psum = 0.0;
    for (double p : personalize) psum += p;
    if (psum > 0.0)
        for (std::size_t i = 0; i < n; ++i) restart[i] = personalize[i] / psum;

    for (int iter = 0; iter < 24; ++iter) {
        std::fill(next.begin(), next.end(), 0.0);
        double dangling = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& edges = g.files[i].out_edges;
            if (edges.empty()) { dangling += rank[i]; continue; }
            double total_w = 0.0;
            for (const auto& [j, w] : edges) total_w += w;
            for (const auto& [j, w] : edges)
                next[j] += kD * rank[i] * (static_cast<double>(w) / total_w);
        }
        for (std::size_t i = 0; i < n; ++i)
            next[i] += (1.0 - kD) * restart[i] + kD * dangling * restart[i];
        rank.swap(next);
    }
    for (std::size_t i = 0; i < n; ++i) g.files[i].rank = rank[i];

    // Symbol-level rank (aider's key move): a file's PageRank is a property of
    // the FILE, but the budget is packed by SYMBOL, so distribute each file's
    // rank across its definitions in proportion to how referenced each is.
    // A def with many inbound refs claims most of its file's rank; unreferenced
    // defs share a small floor so a brand-new symbol nobody calls yet still
    // ranks above nothing. This is what lets a single widely-used function in
    // a mid-ranked file out-pack twenty trivial helpers in a god-file.
    for (std::size_t i = 0; i < n; ++i) {
        double total_in = 0.0;
        for (const auto& d : g.files[i].defs)
            total_in += static_cast<double>(d.inbound) + 0.25;  // 0.25 floor
        if (total_in <= 0.0) continue;
        for (auto& d : g.files[i].defs)
            d.rank = g.files[i].rank
                   * ((static_cast<double>(d.inbound) + 0.25) / total_in);
    }
}

struct RepoMapArgs {
    std::string focus;
    std::string root;
    int         budget = 8000;   // bytes (~2000 tokens)
};

mcp::cap::Result run_repo_map(const Json& args) {
    RepoMapArgs a;
    if (args.is_object()) {
        a.focus  = args.value("focus", std::string{});
        a.root   = args.value("path", std::string{"."});
        a.budget = args.value("budget", 8000);
    }
    if (a.budget < 1000)  a.budget = 1000;
    if (a.budget > 60000) a.budget = 60000;

    auto wp = util::make_workspace_path_checked(a.root, "repo_map");
    if (!wp) return mcp::cap::Result::error(wp.error().detail);

    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);

    // The cached graph is mutated by pagerank (rank field) — take a working
    // copy of the const cache so personalization doesn't leak across calls.
    RepoGraph g = build_graph(wp->path());
    if (g.files.empty())
        return mcp::cap::Result::error(
            "repo_map: no source files found under " + wp->path().string());

    // Personalization: lowercase focus terms matched against path + symbol
    // names. A hit boosts the file's restart probability 20x.
    std::vector<double> personalize;
    if (!a.focus.empty()) {
        std::string lf;
        for (char c : a.focus)
            lf.push_back(static_cast<char>(std::tolower((unsigned char)c)));
        std::vector<std::string> terms;
        std::istringstream iss(lf);
        for (std::string t; iss >> t;) if (t.size() >= 3) terms.push_back(t);
        if (!terms.empty()) {
            personalize.assign(g.files.size(), 1.0);
            for (std::size_t i = 0; i < g.files.size(); ++i) {
                std::string hay = g.files[i].rel;
                for (const auto& d : g.files[i].defs) {
                    hay += ' ';
                    hay += d.name;
                }
                for (auto& c : hay)
                    c = static_cast<char>(std::tolower((unsigned char)c));
                for (const auto& t : terms)
                    if (hay.find(t) != std::string::npos) {
                        personalize[i] = 20.0;
                        break;
                    }
            }
        }
    }
    pagerank(g, personalize);

    std::vector<std::uint32_t> order(g.files.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) {
        if (g.files[x].rank != g.files[y].rank)
            return g.files[x].rank > g.files[y].rank;
        return g.files[x].rel < g.files[y].rel;
    });

    // Render ONE file's block: its path, then its definitions ordered by
    // SYMBOL rank (most-referenced first) so that when a file has more defs
    // than the per-file cap, the ones shown are the architecturally important
    // ones — not just whatever appeared first in the file. Line order is
    // preserved as the tie-break so the block still reads top-to-bottom for
    // equally-ranked symbols. Files with no extracted defs still get a bare
    // path line (they exist and rank — e.g. DSL files the def regexes don't
    // parse).
    auto render_block = [](const FileNode& f) {
        std::vector<const Def*> ds;
        ds.reserve(f.defs.size());
        for (const auto& d : f.defs) ds.push_back(&d);
        std::sort(ds.begin(), ds.end(), [](const Def* x, const Def* y) {
            if (x->rank != y->rank) return x->rank > y->rank;
            return x->line < y->line;
        });
        std::ostringstream block;
        block << f.rel << ":\n";
        std::size_t shown = 0;
        // For the shown subset, restore LINE order so the signatures read in
        // source order (rank chose WHICH to show; line chooses the order).
        constexpr std::size_t kCap = 24;
        std::vector<const Def*> pick(ds.begin(),
            ds.begin() + std::min<std::size_t>(kCap, ds.size()));
        std::sort(pick.begin(), pick.end(),
                  [](const Def* x, const Def* y) { return x->line < y->line; });
        for (const auto* d : pick) {
            block << "  L" << d->line << ": " << d->signature << "\n";
            ++shown;
        }
        if (f.defs.size() > shown)
            block << "  \xe2\x80\xa6 +" << (f.defs.size() - shown)
                  << " more defs\n";
        return block.str();
    };

    // Header (fixed) + binary-search the file COUNT that best fills the
    // budget. Greedy packing overshoots on the last block or stops early;
    // aider binary-searches to land within ~15% of budget. We do the same:
    // find the largest prefix of `order` whose rendered size fits, then emit
    // it. Blocks are memoized so we render each file at most twice.
    std::ostringstream header;
    header << "Repository map (" << g.files.size() << " files ranked";
    if (!a.focus.empty()) header << ", focused on '" << a.focus << "'";
    header << "; PageRank over the def/ref graph):\n\n";
    const std::string header_str = header.str();

    std::vector<std::string> blocks(order.size());
    auto block_at = [&](std::size_t k) -> const std::string& {
        if (blocks[k].empty()) blocks[k] = render_block(g.files[order[k]]);
        return blocks[k];
    };
    // Footer is ~90 bytes; reserve room so the closing line never pushes past
    // budget. cumulative size of the first `count` blocks + header + footer.
    constexpr int kFooterReserve = 110;
    auto fits = [&](std::size_t count) {
        long long total = static_cast<long long>(header_str.size()) + kFooterReserve;
        for (std::size_t k = 0; k < count; ++k)
            total += static_cast<long long>(block_at(k).size());
        return total <= a.budget;
    };

    // Binary search for the largest prefix that fits. Always emit ≥1 file.
    std::size_t lo = 1, hi = order.size(), best = 1;
    while (lo <= hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (fits(mid)) { best = mid; lo = mid + 1; }
        else { if (mid == 0) break; hi = mid - 1; }
    }
    if (order.empty()) best = 0;

    std::ostringstream out;
    out << header_str;
    std::size_t emitted_files = 0;
    for (std::size_t k = 0; k < best; ++k) {
        out << block_at(k);
        ++emitted_files;
    }
    out << "\n(" << emitted_files << " of " << g.files.size()
        << " files shown, budget " << a.budget << " bytes. Re-run with "
        << "`focus` to re-center the map, `budget` to widen it.)";
    return mcp::cap::Result::ok(out.str());
}

} // namespace

void register_repo_map_tool(Shells& sh) {
    sh.add("repo_map",
        "Token-budgeted ranked map of the codebase: every source file scored "
        "by PageRank over the definition/reference graph, top files rendered "
        "with their definition signatures (name + line). THE tool to call "
        "FIRST in a large or unfamiliar repository — one call replaces a "
        "dozen exploratory read/grep rounds. Pass `focus` (task keywords or "
        "symbol names) to re-center the ranking on your task.",
        Json{
            {"type", "object"},
            {"properties", {
                {"focus",  {{"type","string"},  {"description","Keywords / symbol names to personalize the ranking around (like aider's chat-file boost). Optional."}}},
                {"path",   {{"type","string"},  {"description","Root directory (default: workspace root)"}}},
                {"budget", {{"type","integer"}, {"description","Output byte budget, 1000-60000 (default 8000 ≈ 2k tokens)"}}},
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
            }},
        },
        EffectSet{Effect::ReadFs},
        run_repo_map, 60'000);
}

} // namespace mcp::tools::detail
