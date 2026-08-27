// SPDX-License-Identifier: Apache-2.0
//
// search.cpp — register_search_tools: grep / glob / find_definition.
// Faithful port of agentty's src/tool/tools/{grep,glob,find_definition}.cpp.
// Refined domain types (NonBlank/NonNegative) are replaced with plain
// string/int; the parsers enforce the same invariants up front.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/glob.hpp>
#include <mcp/tools/util/subprocess.hpp>
#include <mcp/tools/util/utf8.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/regex_guard.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

// Forward decl — defined below near is_literal_pattern; used by run_glob.
[[nodiscard]] bool glob_hit(std::string_view pattern,
                            const fs::path& file, const fs::path& root);

// ═══════════════════════════════════════════════════════════════════════
//  glob
// ═══════════════════════════════════════════════════════════════════════

struct GlobArgs {
    std::string pattern;   // non-blank by construction (parser enforces)
    std::string root;
    std::string display_description;
};

std::expected<GlobArgs, ToolError> parse_glob_args(const json& j) {
    util::ArgReader ar(j);
    auto pat_opt = ar.require_str("pattern");
    if (!pat_opt)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    std::string pat = *std::move(pat_opt);
    if (pat.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::unexpected(ToolError::invalid_args(
            "pattern must not be blank (received only whitespace)"));
    return GlobArgs{
        std::move(pat),
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_glob(const GlobArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "glob");
    if (!wp) return std::unexpected(std::move(wp.error()));

    const auto& pat = a.pattern;
    bool has_glob = pat.find_first_of("*?[") != std::string::npos;

    struct Entry {
        std::string path;
        bool is_dir;
        bool is_link;
        uintmax_t size;
        std::int64_t mtime = 0;   // ns since epoch — recency ranking
    };
    std::vector<Entry> entries;
    entries.reserve(512);

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(wp->path(),
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        auto fn = it->path().filename().string();
        bool is_dir_entry = it->is_directory(ec);
        // Symlink-loop guard: don't recurse THROUGH a symlinked directory
        // (a cyclic link makes recursive_directory_iterator spin forever).
        // Symlinked entries are still reported below via the is_symlink flag
        // when they match, but we never descend into them.
        {
            std::error_code lec;
            if (it->is_symlink(lec) && is_dir_entry) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (is_dir_entry) {
            if (util::should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
        }
        bool hit = has_glob ? util::glob_match(pat, fn)
                            : fn.find(pat) != std::string::npos;
        // Slash-bearing patterns ('src/*.cpp', '**/util/*.hpp') are PATH
        // patterns — match them against the workspace-relative path, not
        // just the basename, so directory-scoped globs work as users expect.
        if (!hit && has_glob && pat.find('/') != std::string::npos)
            hit = glob_hit(pat, it->path(), wp->path());
        if (hit) {
            bool is_link = it->is_symlink(ec);
            uintmax_t sz = 0;
            std::int64_t mt = 0;
            if (!is_dir_entry && !is_link) {
                std::error_code sec;
                sz = it->file_size(sec);
                if (auto t = it->last_write_time(sec); !sec)
                    mt = t.time_since_epoch().count();
            }
            // Workspace-relative path: denser output, and the model feeds
            // these straight back into read/edit which resolve relative
            // paths against the project root anyway.
            std::error_code rec2;
            auto rel = fs::relative(it->path(), wp->path(), rec2);
            entries.push_back({
                (rec2 || rel.empty()) ? it->path().string()
                                      : rel.generic_string(),
                is_dir_entry, is_link, sz, mt});
            if (entries.size() > 500) break;
        }
    }

    if (entries.empty())
        return ToolOutput{"no matches. Try a different pattern, or `list_dir` "
                          "on parent directories to see what exists.",
                          std::nullopt};

    // Recency-first (Claude Code's Glob contract: sorted by modification
    // time) — when a pattern hits 40 files, the recently-touched one is
    // almost always the one the task is about. Directories keep a stable
    // alphabetical block at the end (they carry no useful mtime signal).
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
        if (x.is_dir != y.is_dir) return x.is_dir < y.is_dir;   // files first
        if (x.is_dir) return x.path < y.path;                    // dirs: alpha
        if (x.mtime != y.mtime) return x.mtime > y.mtime;        // newest first
        return x.path < y.path;
    });

    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[16];
        const double b = static_cast<double>(bytes);
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", b/1024.0); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fM", b/(1024.0*1024.0)); return buf;
    };

    std::ostringstream out;
    for (const auto& e : entries) {
        out << e.path;
        if (e.is_dir) out << "/";
        else if (e.is_link) out << "@";
        else if (e.size > 0) out << "  " << format_size(e.size);
        out << "\n";
    }

    std::string body = "Found " + std::to_string(entries.size()) + " file(s):\n" + out.str();
    if (entries.size() > 500) body += "[>500, truncated]\n";
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ═══════════════════════════════════════════════════════════════════════
//  find_definition
// ═══════════════════════════════════════════════════════════════════════

struct FindDefinitionArgs {
    std::string symbol;
    std::string root;
    std::string display_description;
};

std::expected<FindDefinitionArgs, ToolError> parse_find_definition_args(const json& j) {
    util::ArgReader ar(j);
    auto sym_opt = ar.require_str("symbol");
    if (!sym_opt)
        return std::unexpected(ToolError::invalid_args("symbol required"));
    return FindDefinitionArgs{
        *std::move(sym_opt),
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_find_definition(const FindDefinitionArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "find_definition");
    if (!wp) return std::unexpected(std::move(wp.error()));

    std::string esc;
    esc.reserve(a.symbol.size() * 2);
    for (char c : a.symbol) {
        switch (c) {
            case '.': case '*': case '+': case '?': case '(': case ')':
            case '[': case ']': case '{': case '}': case '|': case '^':
            case '$': case '\\':
                esc.push_back('\\'); [[fallthrough]];
            default:
                esc.push_back(c);
        }
    }

    std::string rg_pattern =
        "\\b(class|struct|enum|union|namespace|typedef|using|def|function|"
        "const|let|var|type|interface|export|func|fn|trait|mod|static)\\s+"
        + esc + "\\b|#define\\s+" + esc + "\\b|\\b\\w[\\w:*&<> ]*\\s+" + esc + "\\s*\\(";

    static int rg_available = -1;
    if (rg_available < 0) {
        auto probe = util::Subprocess::run(util::SubprocessOptions{
            .argv      = std::vector<std::string>{"rg", "--version"},
            .timeout   = std::chrono::seconds(2),
            .max_bytes = 1024,
        });
        rg_available = (probe.started && probe.exit_code == 0) ? 1 : 0;
    }

    if (rg_available == 1) {
        // Pass every argument via argv — the pattern contains regex meta
        // (|, (), \b, <, >, *) that a shell string would mangle or, worse,
        // interpret. argv form reaches rg byte-for-byte.
        std::vector<std::string> argv = {
            "rg", "-n", "-H", "--no-heading", "--no-config", "-M", "500", "-m", "50",
            "--type-add",
            "code:*.{cpp,hpp,c,h,cc,hh,cxx,hxx,py,js,ts,jsx,tsx,go,rs,java,kt,rb,swift,zig,lua,cs,scala,dart,ex,exs,ml,hs,php,pl,pm,sh,bash}",
            "-t", "code", "-e", rg_pattern,
            wp->path().string(),
        };
        // Prune build / vendor / _deps so rg doesn't crawl generated trees.
        for (const auto& g : util::skip_dir_rg_globs()) argv.push_back(g);
        auto r = util::Subprocess::run(util::SubprocessOptions{
            .argv      = std::move(argv),
            .timeout   = std::chrono::seconds(30),
            .max_bytes = 100000,
        });
        if (r.started && (r.exit_code == 0 || r.exit_code == 1)) {
            if (r.output.empty() || r.exit_code == 1) {
                return ToolOutput{"no definitions found for '" + a.symbol + "'", std::nullopt};
            }
            std::string body = r.output;
            while (!body.empty() && body.back() == '\n') body.pop_back();
            if (!a.display_description.empty())
                body = a.display_description + "\n" + body;
            return ToolOutput{std::move(body), std::nullopt};
        }
    }

    std::vector<std::regex> patterns;
    try {
        patterns.emplace_back("\\b(class|struct|enum|union|namespace|typedef|using)\\s+" + esc + "\\b");
        patterns.emplace_back("\\b\\w[\\w:*&<> ]*\\s+" + esc + "\\s*\\(");
        patterns.emplace_back("#define\\s+" + esc + "\\b");
        patterns.emplace_back("\\b(def|class)\\s+" + esc + "\\s*[\\(:]");
        patterns.emplace_back("\\b(function|const|let|var|type|interface|export)\\s+" + esc + "\\b");
        patterns.emplace_back("\\b(func|type)\\s+" + esc + "\\b");
        patterns.emplace_back("\\b(fn|struct|enum|trait|type|mod|const|static)\\s+" + esc + "\\b");
    } catch (...) {
        return std::unexpected(ToolError::invalid_regex("invalid symbol name for regex"));
    }

    std::ostringstream out;
    int matches = 0;
    std::error_code ec;
    constexpr uintmax_t kMaxFileBytes = 512u * 1024u;
    for (auto it = fs::recursive_directory_iterator(wp->path(),
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        const bool is_dir = entry.is_directory(ec);

        // Symlink-loop guard: never descend into a symlinked directory.
        {
            std::error_code lec;
            if (is_dir && entry.is_symlink(lec)) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (is_dir && util::should_skip_dir(fn)) {
            it.disable_recursion_pending();
            continue;
        }
        if (fn.starts_with(".")) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }
        if (is_dir) continue;
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        static const std::vector<std::string> code_exts = {
            ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx",
            ".py", ".js", ".ts", ".jsx", ".tsx", ".go", ".rs",
            ".java", ".kt", ".rb", ".swift", ".zig", ".lua",
            ".cs", ".scala", ".dart", ".ex", ".exs", ".ml", ".hs",
            ".php", ".pl", ".pm", ".sh", ".bash",
        };
        bool is_code = false;
        for (const auto& e : code_exts) { if (ext == e) { is_code = true; break; } }
        if (!is_code) continue;
        std::error_code sec;
        auto sz = entry.file_size(sec);
        if (!sec && (sz == 0 || sz > kMaxFileBytes)) continue;

        std::ifstream ifs(entry.path());
        if (!ifs) continue;
        std::string line;
        int n = 1;
        while (std::getline(ifs, line)) {
            for (const auto& re : patterns) {
                if (std::regex_search(line, re)) {
                    out << entry.path().string() << ":" << n << ": " << line << "\n";
                    if (++matches > 50) goto done;
                    break;
                }
            }
            n++;
        }
    }
    done:
    if (matches == 0) return ToolOutput{"no definitions found for '" + a.symbol + "'", std::nullopt};
    if (matches > 50) out << "[>50 definitions, truncated]\n";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ═══════════════════════════════════════════════════════════════════════
//  grep
// ═══════════════════════════════════════════════════════════════════════

constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr int         kPerPage      = 20;
constexpr int         kContext      = 2;
constexpr int         kMaxScanned   = 500;
constexpr std::size_t kMaxOutputBytes = 20'000;
// File-parallel scan scales with cores; cap high enough to saturate a big
// CI box but bounded so we never spawn hundreds of threads on a huge tree.
constexpr unsigned    kMaxWorkers   = 32;

struct GrepArgs {
    std::string pattern;   // non-blank by construction
    std::string root;
    std::string file_glob;
    bool        case_sensitive;
    bool        word;      // whole-word match (absorbs find_references)
    bool        block;     // return the whole enclosing function/block per hit
    int         context_lines = kContext;  // ±N lines (context:"N")
    int         offset;    // ≥ 0
    // Output mode: Content (default) renders matching lines w/ context;
    // FilesOnly renders one path + match-count per line (rg -l — the survey
    // shape: "which files touch X" without the line noise); Count renders
    // ONLY the total + per-file counts (rg -c — the cheapest possible probe
    // for "how widespread is X", perfect before a rewrite).
    enum class Mode { Content, FilesOnly, Count };
    Mode        mode = Mode::Content;
    std::string display_description;
};

std::expected<GrepArgs, ToolError> parse_grep_args(const json& j) {
    util::ArgReader ar(j);
    auto pat_opt = ar.require_str("pattern");
    if (!pat_opt)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    std::string pat = *std::move(pat_opt);
    if (pat.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::unexpected(ToolError::invalid_args(
            "pattern must not be blank (received only whitespace)"));
    int offset = ar.integer("offset", 0);
    if (offset < 0) offset = 0;
    // `context`: "block" returns the enclosing scope; a NUMBER ("0".."10",
    // or a bare integer) sets the ±N window; anything else is the default ±2.
    bool block = false;
    int  ctx_lines = kContext;
    {
        std::string ctx = ar.str("context", "");
        if (ctx.empty() && ar.has("context")) {
            // Model sent a bare integer (context: 5) — ArgReader.str returns
            // "" for non-strings; read it as an integer instead.
            ctx_lines = std::clamp(ar.integer("context", kContext), 0, 10);
        } else if (ctx == "block") {
            block = true;
        } else if (!ctx.empty()
                   && ctx.find_first_not_of("0123456789") == std::string::npos) {
            ctx_lines = std::clamp(std::atoi(ctx.c_str()), 0, 10);
        }
    }
    GrepArgs::Mode mode = GrepArgs::Mode::Content;
    {
        std::string o = ar.str("output", "");
        if (o == "files") mode = GrepArgs::Mode::FilesOnly;
        else if (o == "count") mode = GrepArgs::Mode::Count;
    }
    GrepArgs g{
        std::move(pat),
        ar.str("path", "."),
        ar.str("glob", ""),
        ar.boolean("case_sensitive", false),
        ar.boolean("word", false),
        block,
        ctx_lines,
        offset,
        mode,
        ar.str("display_description", ""),
    };
    return g;
}

[[nodiscard]] bool is_literal_pattern(std::string_view p) noexcept {
    constexpr std::string_view meta{".^$*+?()[]{}|\\"};
    return p.find_first_of(meta) == std::string_view::npos;
}

// Escape ECMAScript regex metacharacters so a literal string can be embedded
// in a regex (used by grep's word-boundary path). One backslash per meta char.
[[nodiscard]] std::string regex_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (std::string_view{".^$|()[]{}*+?\\"}.find(c) != std::string_view::npos)
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Match a glob against a file, choosing filename-vs-path semantics from the
// pattern shape. A pattern with no '/' (e.g. "*.cpp", "test_*") matches the
// BASENAME anywhere in the tree — the historical, least-surprising default.
// A pattern that contains '/' (e.g. "src/*.cpp", "**/util/*.hpp") is a PATH
// pattern: it's matched against the workspace-relative path with normalised
// forward slashes, and a leading "**/" is allowed to match at any depth.
// This is the behaviour users coming from ripgrep/git already expect, and
// it was previously silently broken (slash patterns matched nothing because
// only filename() was ever tested).
[[nodiscard]] bool glob_hit(std::string_view pattern,
                            const fs::path& file,
                            const fs::path& root) {
    if (pattern.find('/') == std::string_view::npos)
        return util::glob_match(pattern, file.filename().string());

    std::error_code rec;
    fs::path rel = fs::relative(file, root, rec);
    std::string rp = (rec || rel.empty() ? file : rel).generic_string();
    while (rp.starts_with("./")) rp.erase(0, 2);

    if (util::glob_match(pattern, rp)) return true;
    // "**/" prefix (or a bare "**") should also match paths at the root with
    // no leading directory — strip it and retry so "**/foo.c" hits "foo.c".
    if (pattern.starts_with("**/"))
        return util::glob_match(pattern.substr(3), rp);
    return false;
}

[[nodiscard]] bool likely_binary_ext(const fs::path& p) {
    static const std::unordered_set<std::string> bins = {
        ".exe", ".dll", ".lib", ".a", ".o", ".obj", ".pdb", ".so", ".dylib",
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".tiff",
        ".pdf", ".zip", ".tar", ".gz", ".bz2", ".xz", ".7z", ".rar",
        ".mp3", ".mp4", ".wav", ".avi", ".mov", ".webm", ".flac", ".ogg",
        ".ttf", ".otf", ".woff", ".woff2", ".eot",
        ".class", ".jar", ".pyc", ".pyo", ".wasm",
        ".bin", ".iso", ".dat", ".db", ".sqlite", ".sqlite3",
        ".dmg", ".deb", ".rpm", ".msi",
        ".lock",
    };
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return bins.contains(e);
}

void scan_literal(std::string_view content, std::string_view needle,
                  bool case_insensitive, std::vector<std::size_t>& out,
                  std::atomic<int>& total) {
    if (needle.empty()) return;
    auto record = [&](std::size_t pos) -> bool {
        out.push_back(pos);
        return total.fetch_add(1, std::memory_order_relaxed) + 1 < kMaxScanned;
    };
    if (!case_insensitive) {
        std::size_t pos = 0;
        while ((pos = content.find(needle, pos)) != std::string_view::npos) {
            if (!record(pos)) return;
            pos += needle.size();
        }
        return;
    }
    // Case-insensitive: search in place instead of lowercasing the whole
    // file into two heap strings (the old path allocated 2x content.size()
    // per file, dominating scan time on large trees). Fold only the first
    // needle byte to find candidate starts, then compare the remainder
    // byte-for-byte with ASCII case folding. ASCII-only fold matches the
    // previous std::tolower behaviour on the default C locale.
    auto fold = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    const unsigned char n0 = fold(static_cast<unsigned char>(needle[0]));
    const std::size_t nsz = needle.size();
    if (content.size() < nsz) return;
    const std::size_t limit = content.size() - nsz + 1;
    const char* base = content.data();

    // Fast path: when the needle's first byte is NOT a letter it has a single
    // case-fold form, so a SIMD-vectorised memchr can skip non-matching runs
    // a cache line at a time. (For an alpha first byte the two possible source
    // bytes would force two memchr passes per step, which is pathologically
    // slow when that letter is common — so that case keeps the scalar loop.)
    // The two branches are separate loops so neither carries the other's test.
    if (!(n0 >= 'a' && n0 <= 'z')) {
        std::size_t i = 0;
        while (i < limit) {
            const char* p = static_cast<const char*>(
                std::memchr(base + i, n0, limit - i));
            if (!p) return;
            i = static_cast<std::size_t>(p - base);
            std::size_t k = 1;
            for (; k < nsz; ++k) {
                if (fold(static_cast<unsigned char>(content[i + k]))
                    != fold(static_cast<unsigned char>(needle[k]))) break;
            }
            if (k == nsz) {
                if (!record(i)) return;
                i += nsz;   // non-overlapping
            } else {
                ++i;
            }
        }
        return;
    }

    // Alpha first byte: tight scalar fold+compare (memchr can't help without a
    // second pass per candidate).
    for (std::size_t i = 0; i < limit; ++i) {
        if (fold(static_cast<unsigned char>(content[i])) != n0) continue;
        std::size_t k = 1;
        for (; k < nsz; ++k) {
            if (fold(static_cast<unsigned char>(content[i + k]))
                != fold(static_cast<unsigned char>(needle[k]))) break;
        }
        if (k == nsz) {
            if (!record(i)) return;
            i += nsz - 1;   // non-overlapping, matches the exact-path stride
        }
    }
}

void scan_regex(std::string_view content, const std::regex& re,
                std::vector<std::size_t>& out, std::atomic<int>& total) {
    auto begin = std::cregex_iterator(content.data(),
                                       content.data() + content.size(), re);
    auto end = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
        out.push_back(static_cast<std::size_t>(it->position(0)));
        if (total.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxScanned)
            return;
    }
}

struct LineInfo {
    int          line_no;
    std::size_t  line_start;
    std::size_t  line_end;
};

[[nodiscard]] std::vector<LineInfo>
offsets_to_lines(std::string_view content,
                 const std::vector<std::size_t>& offsets) {
    std::vector<LineInfo> out;
    out.reserve(offsets.size());
    int line_no = 1;
    std::size_t line_start = 0;
    std::size_t cursor = 0;
    for (std::size_t off : offsets) {
        while (cursor < off) {
            if (content[cursor] == '\n') {
                ++line_no;
                line_start = cursor + 1;
            }
            ++cursor;
        }
        auto nl = content.find('\n', off);
        std::size_t line_end = (nl == std::string_view::npos)
                             ? content.size() : nl;
        out.push_back({line_no, line_start, line_end});
    }
    return out;
}

struct FileHit {
    fs::path                  path;
    std::string               content;
    std::vector<std::size_t>  match_offsets;
};

enum class Backend { Ripgrep, BuiltIn };

[[nodiscard]] Backend detect_backend() {
    static const Backend cached = []{
        auto r = util::Subprocess::run(util::SubprocessOptions{
            .argv     = std::vector<std::string>{"rg", "--version"},
            .timeout  = std::chrono::seconds(3),
            .max_bytes = 1024,
        });
        return (r.started && r.exit_code == 0)
                ? Backend::Ripgrep : Backend::BuiltIn;
    }();
    return cached;
}

[[nodiscard]] std::string enclosing_symbol(std::string_view content,
                                           int match_line_1based) {
    if (match_line_1based < 2) return {};
    constexpr int kMaxLookback = 400;
    std::vector<std::pair<std::size_t,std::size_t>> lines;
    lines.reserve(static_cast<std::size_t>(match_line_1based) + 1);
    std::size_t ls = 0;
    int ln = 0;
    for (std::size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n') {
            ++ln;
            std::size_t le = i;
            if (le > ls && content[le - 1] == '\r') --le;
            lines.emplace_back(ls, le);
            if (ln >= match_line_1based) break;
            ls = i + 1;
        }
    }
    if (static_cast<int>(lines.size()) < match_line_1based) return {};
    auto indent_of = [&](std::pair<std::size_t,std::size_t> r) -> int {
        int w = 0;
        for (std::size_t i = r.first; i < r.second; ++i) {
            char c = content[i];
            if (c == ' ') ++w;
            else if (c == '\t') w += 4;
            else break;
        }
        return w;
    };
    auto is_blank = [&](std::pair<std::size_t,std::size_t> r) {
        for (std::size_t i = r.first; i < r.second; ++i)
            if (content[i] != ' ' && content[i] != '\t') return false;
        return true;
    };
    auto classify = [&](std::string_view s) -> int {
        static constexpr std::string_view kw[] = {
            "fn ", "def ", "class ", "struct ", "enum ", "impl ", "trait ",
            "interface ", "namespace ", "function", "func ", "public ",
            "private ", "protected ", "static ", "void ", "template",
            "module ", "export ", "type ",
        };
        static constexpr std::string_view ctrl[] = {
            "for ", "for(", "while ", "while(", "if ", "if(", "else",
            "switch ", "switch(", "do ", "do{", "try", "catch", "} else",
            "} catch", "loop ", "loop{", "match ", "match(",
        };
        for (auto k : kw)
            if (s.find(k) != std::string_view::npos) return 2;
        for (auto c : ctrl)
            if (s.starts_with(c)) return 0;
        if (!s.empty() && (s.back() == '{' || s.back() == '(')) return 1;
        return 0;
    };
    const int mi = match_line_1based - 1;
    const int match_indent = indent_of(lines[static_cast<std::size_t>(mi)]);
    int lo = std::max(0, mi - kMaxLookback);
    int best_indent = match_indent;
    std::string fallback;
    for (int i = mi - 1; i >= lo; --i) {
        auto r = lines[static_cast<std::size_t>(i)];
        if (is_blank(r)) continue;
        int ind = indent_of(r);
        if (ind >= best_indent) continue;
        std::string_view s{content.data() + r.first, r.second - r.first};
        std::size_t l = 0;
        while (l < s.size() && (s[l] == ' ' || s[l] == '\t')) ++l;
        s.remove_prefix(l);
        int kind = classify(s);
        if (kind == 2) {
            std::string out{s};
            if (out.size() > 100) { out.resize(util::safe_utf8_cut(out, 99)); out += "\xe2\x80\xa6"; }
            return out;
        }
        if (kind == 1) {
            if (fallback.empty()) {
                fallback.assign(s);
                if (fallback.size() > 100) { fallback.resize(util::safe_utf8_cut(fallback, 99)); fallback += "\xe2\x80\xa6"; }
            }
            best_indent = ind;
            continue;
        }
        best_indent = ind;
    }
    return fallback;
}

// Split content into lines (LF, tolerant of CRLF) — used by block-context mode.
[[nodiscard]] std::vector<std::string> split_content_lines(std::string_view s) {
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

// Find the enclosing brace scope [lo,hi] (1-based, inclusive) of `line` for
// grep's context:"block" mode — so a hit returns the whole function/block it
// lives in and the model needs no follow-up `read`. Lightweight per-line brace
// counting that skips // and # line comments and quoted strings. Clamped to
// `max_span` so a giant function can't blow the budget.
[[nodiscard]] std::pair<int,int> brace_block_range(
    const std::vector<std::string>& lines, int line, int max_span = 120) {
    auto delta = [](const std::string& s) {
        int d = 0; bool in_str = false; char q = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (in_str) { if (c == '\\') { ++i; continue; } if (c == q) in_str = false; continue; }
            // C++ raw string R"delim( ... )delim" (incl. u8R/uR/UR/LR): body is
            // literal, so skip it whole — embedded quotes/braces are just bytes.
            if (c == 'R' || c == 'u' || c == 'U' || c == 'L') {
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
                        i = (close == std::string::npos) ? s.size() - 1
                                                         : close + term.size() - 1;
                        continue;
                    }
                }
            }
            // Apostrophe: only a quote if it CLOSES on this line (char
            // literal or short string). An unpaired ' is a Rust lifetime
            // ('a) — treating it as an open would hide every brace after it.
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
    const int n = static_cast<int>(lines.size());
    if (line < 1 || line > n) return {line, line};
    int lo = line, depth = 0;
    for (int ln = line; ln >= 1 && line - ln < max_span; --ln) {
        depth += delta(lines[ln - 1]); lo = ln;
        if (depth > 0) break;                 // opened our enclosing block
    }
    int hi = line, fd = 0; bool started = false;
    for (int ln = lo; ln <= n && ln - lo < max_span; ++ln) {
        fd += delta(lines[ln - 1]); if (fd > 0) started = true; hi = ln;
        if (started && fd <= 0) break;         // matching close brace
    }
    if (hi < line) hi = line;
    return {lo, hi};
}

// context:"block" for INDENT-scoped languages (Python/Ruby), where braces
// don't delimit scope. Walk backward from the hit to the nearest header at
// strictly smaller indent (Python: line ends with ':'; Ruby: def/class/
// module), then forward while lines are blank or indented deeper than the
// header. Mirrors structural.cpp's indent_scope. 1-based inclusive.
[[nodiscard]] std::pair<int,int> indent_block_range(
    const std::vector<std::string>& lines, int line, bool ruby,
    int max_span = 120) {
    auto indent_of = [](const std::string& s) -> int {
        int w = 0;
        for (char c : s) {
            if (c == ' ') ++w;
            else if (c == '\t') w += 8;
            else return w;
        }
        return -1;                                 // blank line
    };
    auto is_header = [&](const std::string& s) {
        if (ruby) {
            std::size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return false;
            std::string_view v{s.data() + b, s.size() - b};
            return v.starts_with("def ") || v.starts_with("class ")
                || v.starts_with("module ");
        }
        std::size_t e = s.find_last_not_of(" \t\r");
        return e != std::string::npos && s[e] == ':';
    };
    const int n = static_cast<int>(lines.size());
    if (line < 1 || line > n) return {line, line};

    int hit_ind = indent_of(lines[line - 1]);
    if (hit_ind < 0) hit_ind = 0;

    int lo = line, hdr_ind = -1;
    for (int ln = line; ln >= 1 && line - ln <= max_span; --ln) {
        int ind = indent_of(lines[ln - 1]);
        if (ind < 0) continue;
        if (ind < hit_ind && is_header(lines[ln - 1])) { lo = ln; hdr_ind = ind; break; }
        if (ind == 0 && ln < line) break;          // top-level non-header: stop
    }
    if (hdr_ind < 0) return {line, line};          // module-level hit: bare

    int hi = line;
    for (int ln = line; ln <= n && ln - lo <= max_span; ++ln) {
        int ind = indent_of(lines[ln - 1]);
        if (ind < 0) continue;                     // blank: tentative
        if (ind <= hdr_ind && ln > lo) break;
        hi = ln;
    }
    return {lo, hi};
}

// A dead-end "No matches found." costs the model a turn guessing why. Build a
// short, PRIORITISED hint from the actual args: the causes that most often
// turn a real hit into zero, in the order worth trying.
std::string grep_no_match_hint(const GrepArgs& a) {
    std::string h = "No matches for '" + a.pattern + "'";
    if (!a.file_glob.empty()) h += " in files matching '" + a.file_glob + "'";
    h += ".";
    std::vector<std::string> tips;
    if (a.case_sensitive)
        tips.emplace_back("drop case_sensitive (search is case-insensitive by "
                          "default) if the casing might differ");
    if (!a.file_glob.empty())
        tips.emplace_back("widen or remove `glob` — it may be filtering out the "
                          "files that contain it");
    if (a.word)
        tips.emplace_back("drop `word` if the term appears as part of a larger "
                          "identifier");
    // Always-useful escalation paths.
    tips.emplace_back("loosen the regex (fewer anchors / a substring)");
    tips.emplace_back("use `search_code` for a meaning-based search when you "
                      "don't know the exact spelling");
    h += " Try: ";
    for (std::size_t i = 0; i < tips.size(); ++i)
        h += (i ? "; " : "") + tips[i];
    h += ".";
    return h;
}

ExecResult run_ripgrep(const GrepArgs& a) {
    std::vector<std::string> argv = {"rg", "--json", "--no-config"};
    if (!a.case_sensitive) argv.push_back("-i");
    if (a.word) argv.push_back("-w");           // whole-word match
    if (is_literal_pattern(a.pattern)) argv.push_back("-F");
    argv.push_back("-C");
    // Summary modes never render lines — context bytes are pure waste.
    argv.push_back(std::to_string(
        a.mode == GrepArgs::Mode::Content ? a.context_lines : 0));
    // Prune the build / vendor / _deps trees the built-in walker also skips.
    // Without these, ripgrep still stat + gitignore-checks every generated
    // file (tens of thousands in a repo with out-of-source build dirs) on a
    // cold cache, and scans them outright when there's no .gitignore. Match
    // should_skip_dir() exactly so grep behaves identically on both backends.
    for (const auto& g : util::skip_dir_rg_globs()) argv.push_back(g);
    if (!a.file_glob.empty()) {
        argv.push_back("-g");
        argv.push_back(a.file_glob);
    }
    argv.push_back("--");
    argv.push_back(a.pattern);
    argv.push_back(a.root.empty() ? std::string{"."} : a.root);

    auto r = util::Subprocess::run(util::SubprocessOptions{
        .argv      = std::move(argv),
        .timeout   = std::chrono::seconds(60),
        .max_bytes = 8 * 1024 * 1024,
    });
    if (!r.started)
        return std::unexpected(ToolError::spawn(
            "rg failed to start: " + r.start_error));
    if (r.exit_code == 1)
        return ToolOutput{grep_no_match_hint(a), std::nullopt};
    if (r.exit_code != 0)
        return std::unexpected(ToolError::subprocess(
            "rg exited " + std::to_string(r.exit_code) + ":\n"
            + r.output.substr(0, 1024)));

    struct LineRow { int line_no; std::string text; bool is_match; };
    struct FileRows { std::string path; std::vector<LineRow> rows; int matches = 0; };
    std::vector<FileRows> files;
    int total_matches = 0;
    {
        std::size_t pos = 0;
        while (pos < r.output.size() && total_matches < kMaxScanned) {
            auto nl = r.output.find('\n', pos);
            if (nl == std::string::npos) nl = r.output.size();
            std::string_view line{r.output.data() + pos, nl - pos};
            pos = nl + 1;
            if (line.empty()) continue;
            json j = json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object()) continue;
            auto type = j.value("type", "");
            if (type == "begin") {
                auto path = j["data"]["path"].value("text", "");
                files.push_back({std::move(path), {}, 0});
            } else if ((type == "context" || type == "match") && !files.empty()) {
                auto& d = j["data"];
                int ln = d.value("line_number", 0);
                std::string text = d.contains("lines")
                    ? d["lines"].value("text", std::string{}) : std::string{};
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                bool is_match = (type == "match");
                files.back().rows.push_back({ln, std::move(text), is_match});
                if (is_match) {
                    ++files.back().matches;
                    ++total_matches;
                }
            }
        }
    }

    std::erase_if(files, [](const FileRows& f){ return f.matches == 0; });

    if (total_matches == 0)
        return ToolOutput{grep_no_match_hint(a), std::nullopt};

    // Summary modes: no line bodies at all — the cheapest useful answer.
    if (a.mode != GrepArgs::Mode::Content) {
        std::ostringstream out;
        out << "Found " << total_matches << " match"
            << (total_matches == 1 ? "" : "es")
            << (total_matches >= kMaxScanned ? "+" : "")
            << " across " << files.size()
            << " file" << (files.size() == 1 ? "" : "s") << ".\n\n";
        if (a.mode == GrepArgs::Mode::FilesOnly) {
            std::size_t listed = 0;
            for (const auto& f : files) {
                out << f.path << "  (" << f.matches
                    << (f.matches == 1 ? " match)" : " matches)") << "\n";
                if (++listed >= 200) {
                    out << "[" << (files.size() - listed)
                        << " more files — narrow with `glob` or `path`]\n";
                    break;
                }
            }
        } else {   // Count — per-file counts, densest form
            for (const auto& f : files)
                out << f.matches << "\t" << f.path << "\n";
        }
        return ToolOutput{out.str(), std::nullopt};
    }

    std::ostringstream out;
    out << "Found " << total_matches << " match"
        << (total_matches == 1 ? "" : "es")
        << (total_matches >= kMaxScanned ? "+" : "")
        << " across " << files.size()
        << " file" << (files.size() == 1 ? "" : "s") << ".\n\n";

    int shown = 0, skipped = 0;
    bool size_capped = false;
    for (auto& f : files) {
        if (shown >= kPerPage || size_capped) break;
        struct Block { int s, e; std::vector<const LineRow*> rows; int matches; };
        std::vector<Block> blocks;
        for (const auto& row : f.rows) {
            if (blocks.empty() || row.line_no > blocks.back().e + 1) {
                blocks.push_back({row.line_no, row.line_no, {&row}, row.is_match ? 1 : 0});
            } else {
                blocks.back().e = row.line_no;
                blocks.back().rows.push_back(&row);
                if (row.is_match) ++blocks.back().matches;
            }
        }

        std::string file_body;
        bool body_loaded = false;
        auto breadcrumb_for = [&](int match_line) -> std::string {
            if (!body_loaded) {
                file_body = util::read_file(fs::path{f.path});
                body_loaded = true;
            }
            if (file_body.empty()) return {};
            return enclosing_symbol(file_body, match_line);
        };

        bool emitted_header = false;
        for (auto& b : blocks) {
            if (b.matches == 0) continue;
            if (skipped + b.matches <= a.offset) {
                skipped += b.matches;
                continue;
            }
            if (shown >= kPerPage) break;
            if (static_cast<std::size_t>(out.tellp()) >= kMaxOutputBytes) {
                size_capped = true;
                break;
            }
            if (!emitted_header) {
                out << "## Matches in " << f.path << "\n\n";
                emitted_header = true;
            }
            int first_match_line = b.s;
            for (const auto* row : b.rows)
                if (row->is_match) { first_match_line = row->line_no; break; }
            std::string sym = breadcrumb_for(first_match_line);
            out << "### ";
            if (!sym.empty()) out << sym << " \xe2\x80\xba ";
            out << "L" << b.s << "-" << b.e << "\n```\n";
            for (const auto* row : b.rows) out << row->text << "\n";
            out << "```\n\n";
            shown += b.matches;
        }
    }
    if (size_capped) {
        out << "[output capped at " << kMaxOutputBytes
            << " bytes — narrow the pattern or use offset to page]\n\n";
    }

    int remaining = total_matches - (a.offset + shown);
    if (remaining > 0) {
        out << "Showing matches " << (a.offset + 1) << "-"
            << (a.offset + shown) << " of " << total_matches
            << (total_matches >= kMaxScanned ? "+ (scan limit reached)" : "")
            << ". Use offset: " << (a.offset + kPerPage)
            << " to see the next page.";
    } else if (shown == 0) {
        return ToolOutput{
            "No matches on this page. Total matches: "
            + std::to_string(total_matches) + ". Try a smaller offset.",
            std::nullopt};
    } else {
        out << "Showing all " << total_matches << " matches.";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ExecResult run_builtin(const GrepArgs& a) {
    // word=true forces whole-word matching. We compile a regex with \b anchors
    // around the (escaped-if-literal) pattern, so the fast literal path is
    // bypassed — correctness (no `foo` inside `foobar`) beats the micro-opt.
    const bool literal = !a.word && is_literal_pattern(a.pattern);
    std::regex re;
    if (!literal) {
        // std::regex is an uninterruptible backtracker; a nested unbounded
        // quantifier (e.g. `(a+)+`) against a long non-matching line hangs for
        // seconds–minutes, and the per-file jthread exception wall below does
        // NOT catch it (catastrophic backtracking hangs, it doesn't throw).
        // Refuse the structural cause up front — same guard as textproc /
        // extract — so this backend (used when ripgrep is absent) can't be
        // wedged by one model-supplied pattern.
        if (util::has_nested_quantifier(a.pattern))
            return std::unexpected(ToolError::invalid_regex(
                "regex '" + a.pattern + "' has a nested unbounded quantifier "
                "(e.g. (a+)+) that can cause catastrophic backtracking — "
                "rewrite it without the nested +/*, or search a plain "
                "substring."));
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!a.case_sensitive) flags = flags | std::regex::icase;
        std::string src = a.pattern;
        if (a.word) {
            // Escape when the pattern is a plain literal so a word search for
            // e.g. "a.b" is treated literally; otherwise honour the user regex.
            if (is_literal_pattern(a.pattern)) src = regex_escape(a.pattern);
            src = "\\b(?:" + src + ")\\b";
        }
        try { re = std::regex(src, flags); }
        catch (const std::regex_error& e) {
            return std::unexpected(ToolError::invalid_regex(
                std::string{"invalid regex '"} + a.pattern + "': " + e.what()));
        }
    }

    std::vector<fs::path> candidates;
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(a.root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            auto fn = entry.path().filename().string();
            if (entry.is_directory(ec)) {
                std::error_code lec;
                if (entry.is_symlink(lec)) { it.disable_recursion_pending(); continue; }
                if (util::should_skip_dir(fn)) it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(ec)) continue;
            if (fn.starts_with(".")) continue;
            if (!a.file_glob.empty()
                && !glob_hit(a.file_glob, entry.path(), fs::path{a.root})) continue;
            if (likely_binary_ext(entry.path())) continue;
            std::error_code sec;
            auto sz = entry.file_size(sec);
            if (sec || sz == 0 || sz > kMaxFileBytes) { sec.clear(); continue; }
            candidates.push_back(entry.path());
        }
    }

    if (candidates.empty()) {
        return ToolOutput{
            "No matches found. The directory may be empty or every file was "
            "filtered (binary extension, size cap, or hidden).", std::nullopt};
    }

    std::vector<FileHit>  hits(candidates.size());
    std::atomic<std::size_t> next{0};
    std::atomic<int>      total_matches{0};

    auto worker = [&] {
        while (true) {
            if (total_matches.load(std::memory_order_relaxed) >= kMaxScanned)
                return;
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= candidates.size()) return;
            const auto& path = candidates[i];

            // EXCEPTION WALL. std::regex throws error_complexity/error_stack
            // AT MATCH TIME on catastrophic backtracking (e.g. `(a+)+b`
            // against a large file) — and an exception escaping a jthread is
            // std::terminate: one bad model-supplied pattern would kill the
            // whole process. A file that blows the matcher is skipped, the
            // rest of the scan completes.
            try {
                std::string content = util::read_file(path);
                if (content.empty()) continue;
                auto head = std::min<std::size_t>(content.size(), 4096);
                if (std::memchr(content.data(), '\0', head)) continue;

                std::vector<std::size_t> offsets;
                if (literal) {
                    scan_literal(content, a.pattern, !a.case_sensitive,
                                 offsets, total_matches);
                } else {
                    scan_regex(content, re, offsets, total_matches);
                }
                if (offsets.empty()) continue;
                hits[i].path = path;
                hits[i].content = std::move(content);
                hits[i].match_offsets = std::move(offsets);
            } catch (...) {
                // regex blow-up or I/O race — skip this file, keep scanning.
            }
        }
    };

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 2;
    nthreads = std::min(nthreads, kMaxWorkers);
    nthreads = std::min(nthreads, static_cast<unsigned>(candidates.size()));
    {
        std::vector<std::jthread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    }

    int total = total_matches.load();
    if (total == 0)
        // Same actionable hint the ripgrep backend emits (names the pattern +
        // prioritised next steps incl. `search_code`) so grep's zero-match
        // output is identical whichever backend served it. Append the builtin-
        // specific ECMAScript-vs-PCRE note, which only applies to this path.
        return ToolOutput{
            grep_no_match_hint(a)
            + " (Builtin backend: pattern is ECMAScript regex, not PCRE — no "
              "look-behind or named groups.)",
            std::nullopt};

    std::size_t files_with_hits = 0;
    for (const auto& h : hits) if (!h.path.empty()) ++files_with_hits;

    // Summary modes: path + count only, no line bodies.
    if (a.mode != GrepArgs::Mode::Content) {
        std::ostringstream out;
        out << "Found " << total << " match" << (total == 1 ? "" : "es")
            << (total >= kMaxScanned ? "+" : "")
            << " across " << files_with_hits
            << " file" << (files_with_hits == 1 ? "" : "s") << ".\n\n";
        std::size_t listed = 0;
        for (const auto& h : hits) {
            if (h.path.empty()) continue;
            std::error_code rec;
            auto rel = fs::relative(h.path, a.root, rec);
            std::string rp = (rec || rel.empty() ? h.path : rel).generic_string();
            auto n = h.match_offsets.size();
            if (a.mode == GrepArgs::Mode::FilesOnly) {
                out << rp << "  (" << n << (n == 1 ? " match)" : " matches)") << "\n";
                if (++listed >= 200) {
                    out << "[" << (files_with_hits - listed)
                        << " more files — narrow with `glob` or `path`]\n";
                    break;
                }
            } else {
                out << n << "\t" << rp << "\n";
            }
        }
        return ToolOutput{out.str(), std::nullopt};
    }

    std::ostringstream out;
    out << "Found " << total << " match" << (total == 1 ? "" : "es")
        << (total >= kMaxScanned ? "+" : "")
        << " across " << files_with_hits
        << " file" << (files_with_hits == 1 ? "" : "s") << ".\n\n";

    int shown = 0, skipped = 0;
    bool size_capped = false;
    for (const auto& h : hits) {
        if (h.path.empty()) continue;
        if (shown >= kPerPage || size_capped) break;
        if (static_cast<std::size_t>(out.tellp()) >= kMaxOutputBytes) {
            size_capped = true;
            break;
        }

        auto lines = offsets_to_lines(h.content, h.match_offsets);

        // In block mode, precompute the file's line vector once so each hit's
        // range can be expanded to its enclosing brace scope. Indent-scoped
        // languages (Python/Ruby) have no brace scope — walk indentation.
        std::vector<std::string> file_lines;
        bool indent_scoped = false;
        bool ruby = false;
        if (a.block) {
            file_lines = split_content_lines(h.content);
            auto ext = h.path.extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower((unsigned char)ch));
            indent_scoped = (ext == ".py" || ext == ".pyi" || ext == ".rb");
            ruby = (ext == ".rb");
        }

        std::vector<std::pair<int,int>> page_ranges;
        for (const auto& li : lines) {
            if (skipped < a.offset) { ++skipped; continue; }
            if (shown >= kPerPage) break;
            int row = li.line_no - 1;
            int start, end;
            if (a.block) {
                auto [lo, hi] = indent_scoped
                    ? indent_block_range(file_lines, li.line_no, ruby)
                    : brace_block_range(file_lines, li.line_no);
                start = lo - 1; end = hi - 1;
            } else {
                start = std::max(0, row - a.context_lines);
                end   = row + a.context_lines;
            }
            if (!page_ranges.empty()
                && start <= page_ranges.back().second + 1) {
                page_ranges.back().second =
                    std::max(page_ranges.back().second, end);
            } else {
                page_ranges.emplace_back(start, end);
            }
            ++shown;
        }
        if (page_ranges.empty()) continue;

        int max_row = 0;
        for (auto [s, e] : page_ranges) max_row = std::max(max_row, e);
        std::vector<std::size_t> line_starts;
        line_starts.reserve(static_cast<std::size_t>(max_row + 2));
        line_starts.push_back(0);
        for (std::size_t i = 0; i < h.content.size()
                              && static_cast<int>(line_starts.size()) <= max_row + 1; ++i) {
            if (h.content[i] == '\n') line_starts.push_back(i + 1);
        }
        line_starts.push_back(h.content.size() + 1);
        const int last_line = static_cast<int>(line_starts.size()) - 2;

        out << "## Matches in " << h.path.string() << "\n\n";
        for (auto [s, e] : page_ranges) {
            int es = std::min(e, last_line);
            std::string sym = enclosing_symbol(h.content, s + 1);
            out << "### ";
            if (!sym.empty()) out << sym << " \xe2\x80\xba ";
            out << "L" << (s + 1) << "-" << (es + 1) << "\n```\n";
            for (int i = s; i <= es; ++i) {
                std::size_t ls = line_starts[static_cast<std::size_t>(i)];
                std::size_t le = line_starts[static_cast<std::size_t>(i) + 1] - 1;
                if (le > h.content.size()) le = h.content.size();
                if (ls <= le) {
                    out.write(h.content.data() + ls,
                              static_cast<std::streamsize>(le - ls));
                }
                out << "\n";
            }
            out << "```\n\n";
        }
    }

    int remaining = total - (a.offset + shown);
    if (size_capped) {
        out << "[output capped at " << kMaxOutputBytes
            << " bytes — narrow the pattern or use offset to page]\n\n";
    }
    if (remaining > 0) {
        out << "Showing matches " << (a.offset + 1) << "-"
            << (a.offset + shown) << " of " << total
            << (total >= kMaxScanned ? "+ (scan limit reached)" : "")
            << ". Use offset: " << (a.offset + kPerPage)
            << " to see the next page.";
    } else if (shown == 0) {
        return ToolOutput{
            "No matches on this page. Total matches: "
            + std::to_string(total) + ". Try a smaller offset.",
            std::nullopt};
    } else {
        out << "Showing all " << total << " matches.";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ExecResult run_grep(const GrepArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "grep");
    if (!wp) return std::unexpected(std::move(wp.error()));
    GrepArgs gated = a;
    gated.root = wp->string();

    // Block mode (context:"block") needs the file content in hand to expand a
    // hit to its enclosing brace scope — the builtin scanner always has it, so
    // force that path (ripgrep's --json gives only ±C fixed context).
    const bool use_builtin = a.block || detect_backend() != Backend::Ripgrep;
    auto r = use_builtin ? run_builtin(gated) : run_ripgrep(gated);
    if (r.has_value()) r->text = util::to_valid_utf8(std::move(r->text));
    return r;
}

// ── Schemas ──────────────────────────────────────────────────────────────

json glob_schema() {
    return json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"pattern", {{"type","string"}, {"description","Glob pattern, e.g. *.cpp"}}},
            {"path",    {{"type","string"}, {"description","Root directory (default: cwd)"}}},
        }},
    };
}

json find_definition_schema() {
    return json{
        {"type","object"},
        {"required", {"symbol"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"symbol", {{"type","string"}, {"description","The symbol name to find"}}},
            {"path",   {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
        }},
    };
}

json grep_schema() {
    return json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"pattern",        {{"type","string"}, {"description","Regex pattern to search for"}}},
            {"path",           {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
            {"glob",           {{"type","string"}, {"description","Filter files by glob. A bare pattern like `*.cpp` matches the filename anywhere; a slash pattern like `src/*.ts` or `**/test/*.py` matches the workspace-relative path."}}},
            {"case_sensitive", {{"type","boolean"}, {"description","Case-sensitive match (default: false)"}}},
            {"word",           {{"type","boolean"}, {"description","Whole-word match: `foo` won't match `foobar` or `do_foo`. Use for finding all USES of an identifier."}}},
            {"context",        {{"type","string"}, {"description","`line` (default) = ±2 lines around each hit. `block` = the WHOLE enclosing function/block, so you rarely need a follow-up `read`. A number `0`-`10` = ±N lines (use `0` for match lines only — densest content view)."}}},
            {"output",         {{"type","string"}, {"enum", {"content","files","count"}}, {"description","`content` (default) = matching lines with context. `files` = one path + match-count per line, no line bodies (the survey view: WHICH files touch X). `count` = per-file match counts only (the cheapest probe: HOW WIDESPREAD is X — run this before a bulk rewrite)."}}},
            {"offset",         {{"type","integer"}, {"description","Skip this many matches (for pagination)"}}},
        }},
    };
}

} // namespace

void register_search_tools(Shells& sh) {
    sh.add("grep",
        "Search for a regex pattern across files. Returns matches grouped by "
        "file with 2 lines of context, each block headed by the enclosing "
        "function/class when detectable (e.g. `### fn foo \xe2\x80\xba L12-14`). "
        "Paginated 20 results per page. Case-insensitive by default; pass "
        "case_sensitive=true for exact case. Use offset for subsequent pages.",
        grep_schema(), EffectSet{Effect::ReadFs},
        body<GrepArgs>(run_grep, parse_grep_args), 30'000);

    sh.add("glob",
        "Find files by glob pattern. Supports `*` (any run), `?` (one char), "
        "`[abc]` classes, and bare substrings. A pattern with no slash matches "
        "the FILENAME anywhere in the tree (`*.cpp`); a pattern containing a "
        "slash matches the workspace-relative PATH (`src/*.ts`, `**/util/*.hpp`). "
        "Case-insensitive on Windows.",
        glob_schema(), EffectSet{Effect::ReadFs},
        body<GlobArgs>(run_glob, parse_glob_args), 25'000);

    sh.add("find_definition",
        "Jump to where a symbol is DEFINED (function, class, struct, enum, "
        "type) across the codebase, using curated per-language definition "
        "patterns for C/C++, Python, JavaScript/TypeScript, Go, and Rust — so "
        "it returns the declaration, not a ranked overview (that's `repo_map`). "
        "To find USES of a symbol, use `grep` with word=true; for calls with a "
        "specific shape use `search_structural`.",
        find_definition_schema(), EffectSet{Effect::ReadFs},
        body<FindDefinitionArgs>(run_find_definition, parse_find_definition_args), 25'000);
}

} // namespace mcp::tools::detail
