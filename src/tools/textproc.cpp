// SPDX-License-Identifier: Apache-2.0
//
// textproc.cpp — register_textproc_tools: the "transform & aggregate" layer.
//
// grep/search_structural FIND text; edit/rewrite_structural CHANGE it. This
// module fills the gap between: turning a match set into a *reshaped, reduced*
// result in one call, the way sed/awk/`rg -o`/`sort | uniq -c` do — without
// the fetch→filter→refetch round-trips that burn the model's context.
//
//   • extract    — project each regex match to a capture group OR a delimited
//                  field (awk `$N`), across files. unique / count / sort.
//   • aggregate  — group a pattern's matches (by file | match text | capture)
//                  and reduce (count | list | sum). `sort | uniq -c` for code.
//   • replace    — literal/regex find-replace across a glob, DRY-RUN first.
//                  The plain-text sibling of rewrite_structural.
//   • read_filter— a condensed read: only lines matching a pattern (+context),
//                  the rest collapsed to `⋯ N lines ⋯`. Read a 3k-line file at
//                  a fraction of the context cost when you care about one thing.
//
// Every filesystem-touching tool routes paths through the workspace boundary
// and walks the same skip-list as grep, so behaviour is consistent.

#include "tool_body.hpp"
#include "tool_shell.hpp"
#include "call_state.hpp"

#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/regex_guard.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/utf8.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
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
using util::ToolError;
using util::ExecResult;
using util::ToolOutput;
using json = nlohmann::json;

// ── Shared limits (mirror grep's) ────────────────────────────────────────
constexpr std::size_t kMaxFileBytes   = 8 * 1024 * 1024;
constexpr int         kMaxScanned     = 5000;    // total matches across scan
constexpr std::size_t kMaxOutputBytes = 20'000;
constexpr unsigned    kMaxWorkers     = 32;
constexpr int         kMaxReplaceHits = 10'000;

// ── Small helpers ────────────────────────────────────────────────────────

// A pattern is a plain literal if it contains no regex metacharacters — then
// we can skip std::regex construction for the common substring case.
[[nodiscard]] bool is_literal_pattern(std::string_view p) noexcept {
    return p.find_first_of("\\^$.|?*+()[]{}") == std::string_view::npos;
}

[[nodiscard]] std::string regex_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (std::strchr("\\^$.|?*+()[]{}", c)) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Extensions we never treat as text (binary short-circuit).
[[nodiscard]] bool likely_binary_ext(const fs::path& p) {
    static const std::unordered_set<std::string> kBin = {
        ".png",".jpg",".jpeg",".gif",".bmp",".ico",".webp",".pdf",".zip",
        ".gz",".xz",".zst",".bz2",".7z",".tar",".jar",".class",".o",".a",
        ".so",".dylib",".dll",".exe",".bin",".wasm",".woff",".woff2",".ttf",
        ".otf",".mp3",".mp4",".mov",".avi",".mkv",".wav",".flac",".ogg",
        ".sqlite",".db",".lock",".pyc",".pyo",".node",".ipynb"};
    auto ext = p.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower((unsigned char)ch));
    return kBin.count(ext) > 0;
}

// Minimal glob: `*` any-run, `?` one char, `[abc]` class. A slash in the
// pattern matches against the workspace-relative path; otherwise the
// filename. Anchored full-match.
[[nodiscard]] bool glob_one(std::string_view pat, std::string_view s) {
    std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) { ++pi; ++si; }
        else if (pi < pat.size() && pat[pi] == '[') {
            std::size_t j = pi + 1; bool neg = false, ok = false;
            if (j < pat.size() && (pat[j] == '!' || pat[j] == '^')) { neg = true; ++j; }
            while (j < pat.size() && pat[j] != ']') {
                if (j + 2 < pat.size() && pat[j + 1] == '-' && pat[j + 2] != ']') {
                    if (s[si] >= pat[j] && s[si] <= pat[j + 2]) ok = true;
                    j += 3;
                } else { if (s[si] == pat[j]) ok = true; ++j; }
            }
            if (ok != neg) { pi = j + 1; ++si; } else if (star != std::string_view::npos) {
                pi = star + 1; si = ++mark;
            } else return false;
        }
        else if (pi < pat.size() && pat[pi] == '*') { star = pi++; mark = si; }
        else if (star != std::string_view::npos) { pi = star + 1; si = ++mark; }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

[[nodiscard]] bool glob_hit(std::string_view pattern, const fs::path& file,
                            const fs::path& root) {
    if (pattern.empty()) return true;
    if (pattern.find('/') != std::string_view::npos) {
        auto rel = file.lexically_relative(root);   // walked from root: lexical is exact
        std::string rp = (rel.empty() ? file : rel).generic_string();
        return glob_one(pattern, rp);
    }
    return glob_one(pattern, file.filename().string());
}

// Collect candidate text files under `root`, honouring the same skip rules as
// grep: prune skip-dirs + symlinked dirs, drop dotfiles, binary exts, empty /
// oversized files, and apply an optional file glob.
[[nodiscard]] std::vector<fs::path>
collect_files(const FileSystem& files, const fs::path& root, std::string_view file_glob) {
    std::vector<fs::path> out;
    files.walk(root, [&](const DirEntry& entry) {
        auto fn = entry.path.filename().string();
        if (entry.directory) {
            if (entry.symlink || util::should_skip_dir(fn)) return WalkStep::skip_dir;
            return WalkStep::next;
        }
        if (!entry.regular) return WalkStep::next;
        if (fn.starts_with(".")) return WalkStep::next;
        if (!file_glob.empty() && !glob_hit(file_glob, entry.path, root)) return WalkStep::next;
        if (likely_binary_ext(entry.path)) return WalkStep::next;
        if (entry.size == 0 || entry.size > kMaxFileBytes) return WalkStep::next;
        out.push_back(entry.path);
        return WalkStep::next;
    });
    return out;
}

// Conservative ReDoS guard: reject nested unbounded quantifiers up front.
// The detector lives in a shared header so grep's builtin backend uses the
// same one — see util/regex_guard.hpp for the full rationale.
using util::has_nested_quantifier;

// Compile a search regex from a pattern; honours literal/word/case. On a
// literal pattern with word=false, returns nullopt so callers can use the
// faster substring path.
[[nodiscard]] std::expected<std::optional<std::regex>, ToolError>
compile_pattern(const std::string& pattern, bool case_sensitive, bool word) {
    const bool literal = !word && is_literal_pattern(pattern);
    if (literal) return std::optional<std::regex>{std::nullopt};
    if (has_nested_quantifier(pattern))
        return std::unexpected(ToolError::invalid_regex(
            "regex '" + pattern + "' has a nested unbounded quantifier (e.g. "
            "(a+)+) that can cause catastrophic backtracking — rewrite it "
            "without the nested +/* (e.g. use a single quantifier or a "
            "possessive/atomic form), or search for a plain substring."));
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (!case_sensitive) flags |= std::regex::icase;
    std::string src = word
        ? "\\b(?:" + (is_literal_pattern(pattern) ? regex_escape(pattern) : pattern) + ")\\b"
        : pattern;
    try { return std::optional<std::regex>{std::regex(src, flags)}; }
    catch (const std::regex_error& e) {
        return std::unexpected(ToolError::invalid_regex(
            "invalid regex '" + pattern + "': " + e.what()));
    }
}

// 1-based line number of byte offset `off` in `content` (linear; callers batch
// offsets so this stays cheap enough).
[[nodiscard]] int line_of(const std::string& content, std::size_t off) {
    int line = 1;
    for (std::size_t i = 0; i < off && i < content.size(); ++i)
        if (content[i] == '\n') ++line;
    return line;
}

[[nodiscard]] std::string rel_path(const fs::path& p, const fs::path& root) {
    auto rel = p.lexically_relative(root);   // walked from root: lexical is exact
    return (rel.empty() ? p : rel).generic_string();
}

// A single projected value plus provenance.
struct Projection {
    std::string value;     // the extracted string
    std::string file;      // relative path
    int         line = 0;  // 1-based
};

// ═══════════════════════════════════════════════════════════════════════════
//  extract — project each match to a capture group or a delimited field.
// ═══════════════════════════════════════════════════════════════════════════

struct ExtractArgs {
    std::string root = ".";
    std::string pattern;         // regex (or literal) selecting matches
    std::string file_glob;
    bool        case_sensitive = false;
    int         group = 0;       // capture group to emit (0 = whole match)
    // Column/field mode (awk): split each MATCHING line by `delimiter` and
    // emit field `column` (1-based). When set, takes precedence over `group`.
    std::string delimiter;       // e.g. "," ":" "\t" or "" for whitespace runs
    int         column = 0;      // 1-based; 0 = disabled
    bool        unique = false;  // dedup values (stable first-seen order)
    bool        count  = false;  // value → occurrence count, desc
    bool        sort   = false;  // sort values lexically (with unique/plain)
    bool        with_location = false; // annotate each value with file:line
    int         limit = 500;     // cap emitted rows
    std::string display_description;
    std::string response_format;  // "detailed" turns on with_location
};

const util::ArgSpec<ExtractArgs> kExtractArgs{
    util::arg(&ExtractArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
    util::arg(&ExtractArgs::pattern, "pattern",
              "Regex (ECMAScript) or literal selecting each match. With a regex, `group` picks which capture to emit.").required(),
    util::arg(&ExtractArgs::root, "path",
              "Directory to scan (default: cwd)."),
    util::arg(&ExtractArgs::file_glob, "glob",
              "Filter files, e.g. *.ts or src/**/*.go."),
    util::arg(&ExtractArgs::case_sensitive, "case_sensitive",
              "Case-sensitive match (default: false)."),
    util::arg(&ExtractArgs::group, "group",
              "Capture group to emit per match (0 = whole match, 1 = first group). Mutually exclusive with `column`."),
    util::arg(&ExtractArgs::delimiter, "delimiter",
              "Field mode (awk): split each matching line by this delimiter (\"\" = whitespace runs) and emit field `column`."),
    util::arg(&ExtractArgs::column, "column",
              "1-based field to emit in delimiter/awk mode. Overrides `group`."),
    util::arg(&ExtractArgs::unique, "unique",
              "Dedup values (first-seen order)."),
    util::arg(&ExtractArgs::count, "count",
              "Emit `value → occurrences` sorted desc (like sort|uniq -c)."),
    util::arg(&ExtractArgs::sort, "sort",
              "Sort values lexically."),
    util::arg(&ExtractArgs::with_location, "with_location",
              "Prefix each value with file:line (same as response_format=detailed)."),
    util::arg(&ExtractArgs::response_format, "response_format",
              "concise (default) = values only; detailed = each value tagged with file:line. Ask for concise unless you need provenance — it's cheaper.")
        .suggest({"concise", "detailed"}),
    util::arg(&ExtractArgs::limit, "limit",
              "Max rows emitted (default 500).").range(1, 5000),
};

// Anthropic's response_format token-economy pattern: `concise` (default)
// emits just the values; `detailed` adds file:line provenance.
std::expected<ExtractArgs, ToolError> check_extract(ExtractArgs a, const util::Bounds&) {
    if (a.response_format == "detailed") a.with_location = true;
    if (a.group < 0)
        return std::unexpected(ToolError::invalid_args("`group` must be >= 0"));
    if (a.column < 0)
        return std::unexpected(ToolError::invalid_args("`column` must be >= 0"));
    return a;
}

// Split a line into fields. Empty delimiter = split on whitespace runs
// (awk default); otherwise split on the literal delimiter string.
[[nodiscard]] std::vector<std::string>
split_fields(std::string_view line, std::string_view delim) {
    std::vector<std::string> out;
    if (delim.empty()) {
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
            std::size_t s = i;
            while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
            if (i > s) out.emplace_back(line.substr(s, i - s));
        }
    } else {
        std::size_t s = 0, p;
        while ((p = line.find(delim, s)) != std::string_view::npos) {
            out.emplace_back(line.substr(s, p - s));
            s = p + delim.size();
        }
        out.emplace_back(line.substr(s));
    }
    return out;
}

[[nodiscard]] std::string_view line_at(const std::string& content, std::size_t off) {
    std::size_t s = content.rfind('\n', off == 0 ? 0 : off - 1);
    s = (s == std::string::npos) ? 0 : s + 1;
    std::size_t e = content.find('\n', off);
    if (e == std::string::npos) e = content.size();
    return std::string_view{content}.substr(s, e - s);
}

ExecResult run_extract(const Call& call, const ExtractArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "extract", bounds(call));
    if (!wp) return std::unexpected(std::move(wp.error()));
    const fs::path root = wp->path();

    auto compiled = compile_pattern(a.pattern, a.case_sensitive, /*word=*/false);
    if (!compiled) return std::unexpected(std::move(compiled.error()));
    const bool literal = !compiled->has_value();
    const bool field_mode = a.column > 0;

    if (field_mode && a.group > 0)
        return std::unexpected(ToolError::invalid_args(
            "use `group` OR `column`, not both"));
    if (!literal && a.group > 0) {
        // group validity is checked per-match below (regex smatch size)
    }

    auto files = collect_files(call.fs(), root, a.file_glob);
    if (files.empty())
        return ToolOutput{"No files to scan (empty dir or all filtered).", std::nullopt};

    // PERF, measured — this scan is the slowest tool in the set.
    //
    //   extract '#include <([^>]+)>' over this tree (2,547 files, 30 MiB):
    //     this loop, 12 threads ............ 36.6 ms   (~0.8 GB/s aggregate)
    //     ripgrep -o -r '$1', 1 process ..... 8.4 ms   (~3.6 GB/s)
    //
    // ~90% of our time is inside std::regex's backtracking _M_dfs (perf),
    // and rg's DFA beats N threads of it with one process. `grep`/
    // `find_definition` in search.cpp already shell out to rg for exactly
    // this reason (see rg_available there); the textproc tools never got
    // the same treatment. `-o` with `-r $N` is literally this tool's
    // projection semantics, so the mapping is direct.
    //
    // A differential over 12 patterns (plain, capture-group, alternation,
    // POSIX class, anchors, word boundaries) says the ENGINES agree: with
    // the corpora aligned, every value either side produced matched, and
    // the only survivors were
    //   * rg finding MORE, because this scan stops at kMaxScanned (5000)
    //     and rg does not, and
    //   * file-set skew from the harness's own rg globs.
    // Neither is a semantic difference. Notably `(?i)error` is the reverse
    // of what I assumed: std::regex REJECTS inline flags ("Invalid '(?...)'
    // zero-width assertion") and rg accepts them, so rg is a superset
    // there.
    //
    // So the fast path is viable, but it is NOT a swap, and the reasons are
    // the fiddly ones a rewrite gets wrong:
    //   * rg is Rust regex: it REJECTS lookbehind/lookahead/backreferences
    //     that std::regex accepts (`(?<=foo)bar`, `(?=foo)\w+`,
    //     `\b(\w+)::\1\b` are all parse errors, exit 2). rg must be a PATH
    //     keyed on that exit, with this loop as the fallback.
    //   * the corpora must be made identical or results move for reasons no
    //     user can see: collect_files() walks dotfile DIRECTORIES (.github
    //     is in scope) and ignores .gitignore, so rg needs --hidden
    //     --no-ignore plus the same skip-dir globs and the same size/binary
    //     filters.
    //   * `unique` first-seen ORDER, per-file caps and kMaxScanned are all
    //     observable, and rg streams in a different order than this
    //     file-parallel loop.
    // Worth doing — 4x on the headline case — but it needs that alignment
    // written and tested, not a quick edit.

    std::vector<std::vector<Projection>> per_file(files.size());

    // Shares, not threads: the host's splitter runs them. Share k scans files
    // k, k+n, … and counts into its own slot, so nothing is shared while they
    // run. Each share stops at its slice of the match cap.
    const std::size_t nshares = std::max<std::size_t>(1, std::min<std::size_t>(
        call.width(), std::min<std::size_t>(kMaxWorkers, files.size())));
    const int share_cap = std::max<int>(1, static_cast<int>(kMaxScanned / static_cast<int>(nshares)));
    std::vector<int> counted(nshares, 0);

    auto worker = [&](std::size_t k) {
        int& total = counted[k];
        for (std::size_t i = k; i < files.size() && total < share_cap; i += nshares) {
            std::string content;
            try { content = util::read_file(call.fs(), files[i]); } catch (...) { continue; }
            if (content.empty()) continue;
            auto head = std::min<std::size_t>(content.size(), 4096);
            if (std::memchr(content.data(), '\0', head)) continue;

            auto& out = per_file[i];
            try {
                if (literal) {
                    // Substring scan; whole match == the pattern text.
                    std::size_t pos = 0;
                    std::string hay = content, needle = a.pattern;
                    if (!a.case_sensitive) {
                        for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
                        for (auto& c : needle) c = (char)std::tolower((unsigned char)c);
                    }
                    while ((pos = hay.find(needle, pos)) != std::string::npos) {
                        if (total++ >= share_cap) break;
                        Projection pr;
                        pr.line = line_of(content, pos);
                        if (field_mode) {
                            auto fields = split_fields(line_at(content, pos), a.delimiter);
                            if (a.column <= (int)fields.size())
                                pr.value = fields[a.column - 1];
                        } else {
                            pr.value = a.pattern;   // group 0 of a literal
                        }
                        if (a.with_location) pr.file = rel_path(files[i], root);
                        out.push_back(std::move(pr));
                        pos += needle.empty() ? 1 : needle.size();
                    }
                } else {
                    const std::regex& re = **compiled;
                    auto begin = std::sregex_iterator(content.begin(), content.end(), re);
                    auto end   = std::sregex_iterator();
                    for (auto it = begin; it != end; ++it) {
                        if (total++ >= share_cap) break;
                        const std::smatch& m = *it;
                        Projection pr;
                        auto off = static_cast<std::size_t>(m.position(0));
                        pr.line = line_of(content, off);
                        if (field_mode) {
                            auto fields = split_fields(line_at(content, off), a.delimiter);
                            if (a.column <= (int)fields.size())
                                pr.value = fields[a.column - 1];
                        } else if (a.group < (int)m.size()) {
                            pr.value = m[a.group].str();
                        }
                        if (a.with_location) pr.file = rel_path(files[i], root);
                        out.push_back(std::move(pr));
                    }
                }
            } catch (...) { /* regex blow-up on this file — skip */ }
        }
    };
    call.split(nshares, worker);

    // Flatten in file order (deterministic).
    std::vector<Projection> all;
    for (auto& v : per_file)
        for (auto& p : v) all.push_back(std::move(p));

    if (all.empty())
        return ToolOutput{"No matches. Check the pattern (ECMAScript regex) "
                          "or widen `glob`.", std::nullopt};

    std::ostringstream out;
    int scanned = 0;
    bool capped = false;
    for (int c : counted) { scanned += c; capped = capped || c >= share_cap; }

    // ── count mode: value → occurrences, desc ─────────────────────────────
    if (a.count) {
        std::unordered_map<std::string, int> tally;
        for (auto& p : all) ++tally[p.value];
        std::vector<std::pair<std::string,int>> rows(tally.begin(), tally.end());
        std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) {
            return x.second != y.second ? x.second > y.second : x.first < y.first;
        });
        out << "Extracted " << all.size() << " value" << (all.size()==1?"":"s")
            << (capped?"+":"") << " \xe2\x86\x92 " << rows.size()
            << " distinct.\n\n";
        int shown = 0;
        for (auto& [val, n] : rows) {
            if (shown++ >= a.limit) { out << "\xe2\x80\xa6 " << (rows.size()-a.limit)
                                          << " more.\n"; break; }
            out << n << "\t" << val << "\n";
            if ((std::size_t)out.tellp() >= kMaxOutputBytes) { out << "[capped]\n"; break; }
        }
    }
    // ── plain / unique list ───────────────────────────────────────────────
    else {
        std::vector<Projection> rows;
        if (a.unique) {
            std::unordered_set<std::string> seen;
            for (auto& p : all) if (seen.insert(p.value).second) rows.push_back(p);
        } else rows = std::move(all);
        if (a.sort)
            std::sort(rows.begin(), rows.end(),
                      [](auto& x, auto& y){ return x.value < y.value; });
        out << "Extracted " << rows.size() << " value"
            << (rows.size()==1?"":"s") << (capped?" (scan capped)":"") << ".\n\n";
        int shown = 0;
        for (auto& p : rows) {
            if (shown++ >= a.limit) { out << "\xe2\x80\xa6 " << (rows.size()-a.limit)
                                          << " more (raise `limit`).\n"; break; }
            if (a.with_location) out << p.file << ":" << p.line << "\t";
            out << p.value << "\n";
            if ((std::size_t)out.tellp() >= kMaxOutputBytes) { out << "[capped]\n"; break; }
        }
    }
    std::string body = out.str();
    if (!a.display_description.empty()) body = a.display_description + "\n" + body;
    return ToolOutput{util::to_valid_utf8(std::move(body)), std::nullopt};
}

// ═══════════════════════════════════════════════════════════════════════════
//  aggregate — group a pattern's matches and reduce.
// ═══════════════════════════════════════════════════════════════════════════

struct AggregateArgs {
    std::string root = ".";
    std::string pattern;
    std::string file_glob;
    bool        case_sensitive = false;
    bool        word = false;
    std::string by = "file";   // file | match | capture
    int         group = 1;     // capture group for by=capture
    std::string op = "count";  // count | list | sum
    int         limit = 100;
    std::string display_description;
};

const util::ArgSpec<AggregateArgs> kAggregateArgs{
    util::arg(&AggregateArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
    util::arg(&AggregateArgs::pattern, "pattern",
              "Regex or literal whose matches are grouped and reduced.").required(),
    util::arg(&AggregateArgs::root, "path",
              "Directory to scan (default: cwd)."),
    util::arg(&AggregateArgs::file_glob, "glob",
              "Filter files, e.g. *.py."),
    util::arg(&AggregateArgs::case_sensitive, "case_sensitive"),
    util::arg(&AggregateArgs::word, "word",
              "Whole-word match."),
    util::arg(&AggregateArgs::by, "by",
              "Group key: `file` (which files touch X, and how often), `match` (the whole matched text), or `capture` (a regex group — set `group`).")
        .one_of({"file", "match", "capture"}),
    util::arg(&AggregateArgs::group, "group",
              "Capture group for by=capture (default 1)."),
    util::arg(&AggregateArgs::op, "op",
              "Reduce: `count` per group, `list` (count + up to 5 sample lines), or `sum` (parse each matched text as a number and total it).").one_of({"count", "list", "sum"}),
    util::arg(&AggregateArgs::limit, "limit",
              "Max groups shown (default 100).").range(1, 2000),
};

std::expected<AggregateArgs, ToolError> check_aggregate(AggregateArgs a, const util::Bounds&) {
    if (a.root.empty()) a.root = ".";
    return a;
}

ExecResult run_aggregate(const Call& call, const AggregateArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "aggregate", bounds(call));
    if (!wp) return std::unexpected(std::move(wp.error()));
    const fs::path root = wp->path();

    auto compiled = compile_pattern(a.pattern, a.case_sensitive, a.word);
    if (!compiled) return std::unexpected(std::move(compiled.error()));
    const bool literal = !compiled->has_value();
    if (literal && (a.by == "capture"))
        return std::unexpected(ToolError::invalid_args(
            "by=capture needs a regex with a group; the pattern is a literal"));

    auto files = collect_files(call.fs(), root, a.file_glob);
    if (files.empty())
        return ToolOutput{"No files to scan.", std::nullopt};

    // key → (count, sum, sample lines)
    struct Bucket { long long count = 0; double sum = 0; std::vector<std::string> samples; };
    std::map<std::string, Bucket> buckets;   // ordered for stable output

    // Shares, not threads: the host's splitter runs them. Share k scans files
    // k, k+n, k+2n, … into its own slot, and the slots are merged after, so
    // nothing is shared while they run. Each share stops at its slice of the
    // match cap.
    const std::size_t nshares = std::max<std::size_t>(1, std::min<std::size_t>(
        call.width(), std::min<std::size_t>(kMaxWorkers, files.size())));
    const int share_cap = std::max<int>(1, static_cast<int>(kMaxScanned / static_cast<int>(nshares)));
    std::vector<std::map<std::string, Bucket>> slots(nshares);
    std::vector<int> counted(nshares, 0);

    auto worker = [&](std::size_t k) {
        std::map<std::string, Bucket>& local = slots[k];
        int& total = counted[k];
        for (std::size_t i = k; i < files.size() && total < share_cap; i += nshares) {
            std::string content;
            try { content = util::read_file(call.fs(), files[i]); } catch (...) { continue; }
            if (content.empty()) continue;
            auto head = std::min<std::size_t>(content.size(), 4096);
            if (std::memchr(content.data(), '\0', head)) continue;
            std::string rp = rel_path(files[i], root);

            auto emit = [&](const std::string& key, std::string_view line) {
                Bucket& b = local[key];
                ++b.count;
                if (a.op == "sum") { try { b.sum += std::stod(std::string{line}); } catch (...) {} }
                if (a.op == "list" && b.samples.size() < 5)
                    b.samples.emplace_back(line);
            };

            try {
                if (literal) {
                    std::string hay = content, needle = a.pattern;
                    if (!a.case_sensitive) {
                        for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
                        for (auto& c : needle) c = (char)std::tolower((unsigned char)c);
                    }
                    std::size_t pos = 0;
                    while ((pos = hay.find(needle, pos)) != std::string::npos) {
                        if (total++ >= share_cap) break;
                        std::string_view ln = line_at(content, pos);
                        std::string key = (a.by == "file") ? rp : std::string{ln};
                        emit(key, ln);
                        pos += needle.empty() ? 1 : needle.size();
                    }
                } else {
                    const std::regex& re = **compiled;
                    auto b = std::sregex_iterator(content.begin(), content.end(), re);
                    for (auto it = b; it != std::sregex_iterator(); ++it) {
                        if (total++ >= share_cap) break;
                        const std::smatch& m = *it;
                        auto off = static_cast<std::size_t>(m.position(0));
                        std::string_view ln = line_at(content, off);
                        std::string key;
                        if (a.by == "file") key = rp;
                        else if (a.by == "match") key = m[0].str();
                        else key = (a.group < (int)m.size()) ? m[a.group].str() : std::string{};
                        emit(key, ln);
                    }
                }
            } catch (...) { /* skip file */ }
        }
    };
    call.split(nshares, worker);
    for (auto& local : slots)
        for (auto& [key, v] : local) {
            Bucket& g = buckets[key];
            g.count += v.count; g.sum += v.sum;
            for (auto& smp : v.samples) if (g.samples.size() < 5) g.samples.push_back(std::move(smp));
        }

    if (buckets.empty())
        return ToolOutput{"No matches to aggregate.", std::nullopt};

    // Rank buckets by count (desc) then key.
    std::vector<std::pair<std::string, Bucket>> rows(
        std::make_move_iterator(buckets.begin()),
        std::make_move_iterator(buckets.end()));
    std::sort(rows.begin(), rows.end(), [&](auto& x, auto& y) {
        if (a.op == "sum" && x.second.sum != y.second.sum) return x.second.sum > y.second.sum;
        return x.second.count != y.second.count ? x.second.count > y.second.count
                                                : x.first < y.first;
    });

    std::ostringstream out;
    int scanned = 0;
    bool capped = false;
    for (int c : counted) { scanned += c; capped = capped || c >= share_cap; }
    out << "Aggregated " << scanned << (capped?"+":"") << " match"
        << (scanned==1?"":"es") << " by " << a.by
        << " \xe2\x86\x92 " << rows.size() << " group"
        << (rows.size()==1?"":"s") << " (op=" << a.op << ").\n\n";

    int shown = 0;
    for (auto& [key, b] : rows) {
        if (shown++ >= a.limit) { out << "\xe2\x80\xa6 " << (rows.size()-a.limit)
                                      << " more groups.\n"; break; }
        if (a.op == "sum")      out << b.sum   << "\t" << key << "\n";
        else                    out << b.count << "\t" << key << "\n";
        if (a.op == "list")
            for (auto& s : b.samples) {
                std::string t = s; if (t.size() > 120) t = t.substr(0,117) + "...";
                out << "        " << t << "\n";
            }
        if ((std::size_t)out.tellp() >= kMaxOutputBytes) { out << "[capped]\n"; break; }
    }
    std::string body = out.str();
    if (!a.display_description.empty()) body = a.display_description + "\n" + body;
    return ToolOutput{util::to_valid_utf8(std::move(body)), std::nullopt};
}

// ═══════════════════════════════════════════════════════════════════════════
//  replace — literal/regex find-replace across a glob, DRY-RUN by default.
// ═══════════════════════════════════════════════════════════════════════════

struct ReplaceArgs {
    std::string root = ".";
    std::string find;
    std::string replacement;   // required, may be "" (delete matches)
    std::string file_glob;
    bool        regex = false;
    bool        case_sensitive = true;   // literal replace defaults to exact
    bool        apply = false;           // false = dry run
    int         max_preview = 40;
    std::string display_description;
};

const util::ArgSpec<ReplaceArgs> kReplaceArgs{
    util::arg(&ReplaceArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
    util::arg(&ReplaceArgs::find, "find",
              "Literal text (default) or regex (regex:true) to find in every file.").required(),
    util::arg(&ReplaceArgs::replacement, "replacement",
              "Replacement. In regex mode, $1-$9/$& insert capture groups.")
        .required_present("`replacement` is required"),
    util::arg(&ReplaceArgs::root, "path",
              "Directory to scan (default: cwd)."),
    util::arg(&ReplaceArgs::file_glob, "glob",
              "Filter files — STRONGLY recommended to bound the blast radius, e.g. src/**/*.ts."),
    util::arg(&ReplaceArgs::regex, "regex",
              "Treat `find` as an ECMAScript regex (default: literal)."),
    util::arg(&ReplaceArgs::case_sensitive, "case_sensitive",
              "Default true. In regex mode false adds the icase flag."),
    util::arg(&ReplaceArgs::apply, "apply",
              "false (default) = DRY RUN preview; true = write the files."),
    util::arg(&ReplaceArgs::max_preview, "max_preview",
              "Max per-file diff lines shown in the preview (default 40).").range(1, 200),
};

std::expected<ReplaceArgs, ToolError> check_replace(ReplaceArgs a, const util::Bounds&) {
    if (a.root.empty()) a.root = ".";
    return a;
}

// Count/replace literal occurrences of `needle` in `s` → returns new string +
// hit count. Case-sensitive only (literal path defaults to exact).
[[nodiscard]] std::pair<std::string,int>
literal_replace(const std::string& s, const std::string& needle,
                const std::string& repl) {
    std::string out; out.reserve(s.size());
    int n = 0; std::size_t pos = 0, prev = 0;
    while ((pos = s.find(needle, prev)) != std::string::npos) {
        out.append(s, prev, pos - prev);
        out.append(repl);
        prev = pos + needle.size();
        ++n;
    }
    out.append(s, prev, std::string::npos);
    return {std::move(out), n};
}

ExecResult run_replace(const Call& call, const ReplaceArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "replace", bounds(call));
    if (!wp) return std::unexpected(std::move(wp.error()));
    const fs::path root = wp->path();

    std::optional<std::regex> re;
    if (a.regex) {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!a.case_sensitive) flags |= std::regex::icase;
        try { re.emplace(a.find, flags); }
        catch (const std::regex_error& e) {
            return std::unexpected(ToolError::invalid_regex(
                "invalid regex '" + a.find + "': " + e.what()));
        }
    }

    auto files = collect_files(call.fs(), root, a.file_glob);
    if (files.empty())
        return ToolOutput{"No files to scan.", std::nullopt};

    struct Change { std::string path; int hits; std::string before_line, after_line; int line; };
    std::vector<Change> changes;
    std::vector<mcp::tools::FileChange> file_changes;  // for diff-review (apply only)
    long long total_hits = 0;
    int files_changed = 0;
    std::vector<std::pair<fs::path,std::string>> writes;  // path → new content
    std::vector<std::string> originals;                   // parallel to writes

    for (const auto& f : files) {
        std::string content;
        try { content = util::read_file(call.fs(), f); } catch (...) { continue; }
        if (content.empty()) continue;
        auto head = std::min<std::size_t>(content.size(), 4096);
        if (std::memchr(content.data(), '\0', head)) continue;

        std::string updated;
        int hits = 0;
        if (a.regex) {
            // Count first (iterator), then format-replace.
            auto b = std::sregex_iterator(content.begin(), content.end(), *re);
            for (auto it = b; it != std::sregex_iterator(); ++it) ++hits;
            if (hits == 0) continue;
            try { updated = std::regex_replace(content, *re, a.replacement); }
            catch (...) { continue; }
        } else {
            auto [u, n] = literal_replace(content, a.find, a.replacement);
            if (n == 0) continue;
            hits = n; updated = std::move(u);
        }
        if (total_hits + hits > kMaxReplaceHits)
            return std::unexpected(ToolError::invalid_args(
                "too many matches (>" + std::to_string(kMaxReplaceHits) +
                ") — narrow with `glob` or a more specific `find`"));

        total_hits += hits; ++files_changed;

        // Grab the first changed line for the preview.
        std::size_t off = a.regex
            ? (std::size_t)std::sregex_iterator(content.begin(), content.end(), *re)->position(0)
            : content.find(a.find);
        int ln = line_of(content, off);
        std::string before{line_at(content, off)};
        // Compute the corresponding after-line by replacing within that line.
        std::string after = a.regex
            ? std::regex_replace(before, *re, a.replacement)
            : literal_replace(before, a.find, a.replacement).first;
        changes.push_back({rel_path(f, root), hits, before, after, ln});
        originals.push_back(content);
        writes.emplace_back(f, std::move(updated));
    }

    if (files_changed == 0)
        return ToolOutput{"No matches for `" + a.find + "`.", std::nullopt};

    std::ostringstream out;
    out << (a.apply ? "Applied " : "DRY RUN \xe2\x80\x94 would replace ")
        << total_hits << " occurrence" << (total_hits==1?"":"s")
        << " of `" << a.find << "` \xe2\x86\x92 `" << a.replacement << "` across "
        << files_changed << " file" << (files_changed==1?"":"s") << ".\n\n";

    int shown = 0;
    for (auto& c : changes) {
        if (shown++ >= a.max_preview) { out << "\xe2\x80\xa6 " << (changes.size()-a.max_preview)
                                            << " more files.\n"; break; }
        out << c.path << ":" << c.line << "  (" << c.hits
            << (c.hits==1?" hit)":" hits)") << "\n";
        std::string b = c.before_line, af = c.after_line;
        if (b.size()  > 160) b  = b.substr(0,157) + "...";
        if (af.size() > 160) af = af.substr(0,157) + "...";
        out << "  - " << b  << "\n  + " << af << "\n";
        if ((std::size_t)out.tellp() >= kMaxOutputBytes) { out << "[preview capped]\n"; break; }
    }

    if (a.apply) {
        int written = 0;
        for (std::size_t i = 0; i < writes.size(); ++i) {
            auto& [p, content] = writes[i];
            auto err = util::write_file(call.fs(), p, content);
            if (!err.empty()) continue;
            ++written;
            // Queue this file for diff-review: original → written content.
            file_changes.push_back(
                make_change(rel_path(p, root), originals[i], content));
        }
        out << "\nWrote " << written << " file" << (written==1?"":"s") << ".";
    } else {
        out << "\nRe-run with apply:true to write these changes.";
    }
    std::string body = out.str();
    if (!a.display_description.empty()) body = a.display_description + "\n" + body;
    ToolOutput tout{util::to_valid_utf8(std::move(body)), std::nullopt};
    tout.changes = std::move(file_changes);
    return tout;
}

// ═══════════════════════════════════════════════════════════════════════════
//  read_filter — condensed read: matching lines + context, rest collapsed.
// ═══════════════════════════════════════════════════════════════════════════

struct ReadFilterArgs {
    std::string path;
    std::string pattern;
    bool        case_sensitive = false;
    bool        word = false;
    bool        invert = false;   // keep NON-matching lines instead
    int         context = 2;      // lines of context around each kept line
    int         max_lines = 800;  // cap on emitted (non-collapsed) lines
    std::string display_description;
};

const util::ArgSpec<ReadFilterArgs> kReadFilterArgs{
    util::arg(&ReadFilterArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
    util::arg(&ReadFilterArgs::path, "path",
              "File to read.").required(),
    util::arg(&ReadFilterArgs::pattern, "pattern",
              "Regex or literal — only lines matching this (plus context) are emitted; the rest collapse to `⋯ N lines ⋯`.").required(),
    util::arg(&ReadFilterArgs::case_sensitive, "case_sensitive"),
    util::arg(&ReadFilterArgs::word, "word",
              "Whole-word match."),
    util::arg(&ReadFilterArgs::invert, "invert",
              "Keep NON-matching lines instead (grep -v)."),
    util::arg(&ReadFilterArgs::context, "context",
              "Lines of context around each kept line (default 2, max 20).").range(0, 20),
    util::arg(&ReadFilterArgs::max_lines, "max_lines",
              "Cap on emitted lines (default 800).").range(1, 5000),
};

ExecResult run_read_filter(const Call& call, const ReadFilterArgs& a) {
    const auto b = bounds(call);
    auto wp = util::make_readable_path_checked(a.path, "read_filter", b);
    if (!wp) return std::unexpected(std::move(wp.error()));
    const fs::path p = wp->path();

    std::error_code ec;
    if (!call.fs().stat(p).exists)
        return std::unexpected(ToolError::not_found(p.string()));
    if (!call.fs().stat(p).regular)
        return std::unexpected(ToolError::not_a_file(p.string()));

    std::string content;
    try { content = util::read_file(call.fs(), p); } catch (...) {
        return std::unexpected(ToolError::io("could not read " + p.string()));
    }
    auto head = std::min<std::size_t>(content.size(), 4096);
    if (std::memchr(content.data(), '\0', head))
        return std::unexpected(ToolError::binary(p.string()));

    // Split into lines.
    std::vector<std::string> lines;
    { std::size_t s = 0, n;
      while ((n = content.find('\n', s)) != std::string::npos) {
          lines.emplace_back(content.substr(s, n - s)); s = n + 1; }
      if (s < content.size()) lines.emplace_back(content.substr(s)); }

    auto compiled = compile_pattern(a.pattern, a.case_sensitive, a.word);
    if (!compiled) return std::unexpected(std::move(compiled.error()));
    const bool literal = !compiled->has_value();

    std::string needle = a.pattern;
    if (literal && !a.case_sensitive)
        for (auto& c : needle) c = (char)std::tolower((unsigned char)c);

    auto matches = [&](const std::string& line) -> bool {
        bool hit;
        if (literal) {
            if (a.case_sensitive) hit = line.find(needle) != std::string::npos;
            else { std::string lc = line;
                   for (auto& c : lc) c = (char)std::tolower((unsigned char)c);
                   hit = lc.find(needle) != std::string::npos; }
        } else {
            try { hit = std::regex_search(line, **compiled); } catch (...) { hit = false; }
        }
        return a.invert ? !hit : hit;
    };

    const int N = (int)lines.size();
    std::vector<char> keep(N, 0);
    int match_count = 0;
    for (int i = 0; i < N; ++i) {
        if (matches(lines[i])) {
            ++match_count;
            for (int j = std::max(0, i - a.context);
                 j <= std::min(N - 1, i + a.context); ++j) keep[j] = 1;
        }
    }

    if (match_count == 0)
        return ToolOutput{"No lines match `" + a.pattern + "` in " +
                          rel_path(p, util::project_root(b)) + " (" +
                          std::to_string(N) + " lines scanned).", std::nullopt};

    std::ostringstream out;
    out << rel_path(p, util::project_root(b)) << " \xe2\x80\x94 " << match_count
        << " matching line" << (match_count==1?"":"s") << " of " << N
        << " (\xc2\xb1" << a.context << " context; gaps collapsed):\n\n";

    int emitted = 0, i = 0;
    bool truncated = false;
    while (i < N) {
        if (keep[i]) {
            if (emitted >= a.max_lines) { truncated = true; break; }
            // width-aligned line number; a huge line is clipped around its match
            std::size_t focus = 0;
            if (lines[i].size() > util::kMaxShownLineBytes && !a.invert) {
                if (literal) {
                    std::string lc = lines[i];
                    if (!a.case_sensitive)
                        for (auto& c : lc) c = (char)std::tolower((unsigned char)c);
                    if (auto at = lc.find(needle); at != std::string::npos) focus = at;
                } else {
                    std::smatch sm;
                    try {
                        if (std::regex_search(lines[i], sm, **compiled))
                            focus = static_cast<std::size_t>(sm.position(0));
                    } catch (...) {}
                }
            }
            out << (i + 1) << "\t" << util::clip_line(lines[i], focus) << "\n";
            ++emitted; ++i;
        } else {
            int gap_start = i;
            while (i < N && !keep[i]) ++i;
            int gap = i - gap_start;
            out << "\xe2\x8b\xaf " << gap << " line" << (gap==1?"":"s") << " \xe2\x8b\xaf\n";
        }
        if ((std::size_t)out.tellp() >= kMaxOutputBytes) { truncated = true; break; }
    }
    if (truncated)
        out << "\n[output capped — raise `max_lines`, tighten `pattern`, or "
               "drop `context`]";

    std::string body = out.str();
    if (!a.display_description.empty()) body = a.display_description + "\n" + body;
    return ToolOutput{util::to_valid_utf8(std::move(body)), std::nullopt};
}

} // namespace

void register_textproc_tools(Shells& sh) {
    sh.add("extract",
        "RULES: `pattern` is an ECMAScript regex — escape regex metachars in a "
        "literal; put the part you want to pull in a group and pass its number "
        "as `group` (1 = first group), OR use `delimiter`+`column` for awk-style "
        "field slicing (not both). — Project every match of a pattern to a "
        "VALUE and return the set (the `rg -o -r` / awk `$N` niche): every "
        "import target, route path, TODO owner. `unique` dedups, `count` gives "
        "value\xe2\x86\x92" "frequency (sort|uniq -c), `sort` orders them, "
        "response_format=detailed tags each with file:line. One call replaces "
        "grep\xe2\x86\x92read\xe2\x86\x92hand-aggregate.",
        kExtractArgs.schema(), EffectSet{Effect::ReadFs},
        body<ExtractArgs>(run_extract, kExtractArgs, check_extract), 25'000);

    sh.add("aggregate",
        "USE WHEN the question is \"how many X per Y\" or \"which Y has the most "
        "X\" — count/group/sum a pattern's matches in ONE pass instead of grep "
        "then tallying by hand. The `sort | uniq -c` / GROUP BY for a codebase. "
        "`by:capture` groups on a regex group (e.g. TODO owner, count per "
        "author); `by:file` answers which files touch X and how often (heat map "
        "a refactor); `by:match` tallies the distinct matched strings. `op` is "
        "count (default), list (count + sample lines), or sum (total the matched "
        "numbers). Sorted by magnitude — dominant group first. Reach for this "
        "over grep whenever you'd otherwise count matches yourself.",
        kAggregateArgs.schema(), EffectSet{Effect::ReadFs},
        body<AggregateArgs>(run_aggregate, kAggregateArgs, check_aggregate), 25'000);

    sh.add("replace",
        "RULES: DRY-RUN by default — returns a per-file before/after preview + "
        "total hit count WITHOUT touching disk; re-run with apply:true to write. "
        "ALWAYS pass a `glob` to bound the blast radius. `find` is literal unless "
        "regex:true (then $1-$9/$& in `replacement` insert capture groups). — "
        "Literal or regex find-and-replace across every file under the glob: the "
        "plain-text sibling of rewrite_structural, for renames / string swaps "
        "that aren't an AST shape. For code-shape changes (call sites, control "
        "flow) prefer rewrite_structural; for a single file prefer edit.",
        kReplaceArgs.schema(), EffectSet{Effect::ReadFs, Effect::WriteFs},
        body<ReplaceArgs>(run_replace, kReplaceArgs, check_replace), 25'000);

    sh.add("read_filter",
        "Read a file but keep ONLY the lines matching a pattern (plus context), "
        "collapsing every gap to `\xe2\x8b\xaf N lines \xe2\x8b\xaf`. A condensed "
        "read for a big file when you care about one concern: pull every error "
        "path, every `TODO`, every route registration out of a 3k-line file at a "
        "fraction of the context cost of reading it whole. `invert:true` keeps "
        "non-matching lines (grep -v); `context` sets the \xc2\xb1N window.",
        kReadFilterArgs.schema(), EffectSet{Effect::ReadFs},
        body<ReadFilterArgs>(run_read_filter, kReadFilterArgs), 25'000);
}

} // namespace mcp::tools::detail
