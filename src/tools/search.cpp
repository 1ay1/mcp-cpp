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
        if (is_dir_entry) {
            if (util::should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
        }
        bool hit = has_glob ? util::glob_match(pat, fn)
                            : fn.find(pat) != std::string::npos;
        if (hit) {
            bool is_link = it->is_symlink(ec);
            uintmax_t sz = 0;
            if (!is_dir_entry && !is_link) {
                std::error_code sec;
                sz = it->file_size(sec);
            }
            entries.push_back({it->path().string(), is_dir_entry, is_link, sz});
            if (entries.size() > 500) break;
        }
    }

    if (entries.empty())
        return ToolOutput{"no matches. Try a different pattern, or `list_dir` "
                          "on parent directories to see what exists.",
                          std::nullopt};

    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
        if (x.is_dir != y.is_dir) return x.is_dir > y.is_dir;
        return x.path < y.path;
    });

    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[16];
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024.0)); return buf;
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
        auto r = util::run_command_s("rg --version", 5000, std::chrono::seconds{2});
        rg_available = (r.started && r.exit_code == 0) ? 1 : 0;
    }

    if (rg_available == 1) {
        std::string cmd = "rg -n -H --no-heading -M 500 -m 50 "
            "--type-add 'code:*.{cpp,hpp,c,h,cc,hh,cxx,hxx,py,js,ts,jsx,tsx,go,rs,java,kt,rb,swift,zig,lua}' "
            "-t code -e '" + rg_pattern + "' '" + wp->path().string() + "'";
        auto r = util::run_command_s(cmd, 100000, std::chrono::seconds{30});
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
constexpr unsigned    kMaxWorkers   = 8;

struct GrepArgs {
    std::string pattern;   // non-blank by construction
    std::string root;
    std::string file_glob;
    bool        case_sensitive;
    int         offset;    // ≥ 0
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
    return GrepArgs{
        std::move(pat),
        ar.str("path", "."),
        ar.str("glob", ""),
        ar.boolean("case_sensitive", false),
        offset,
        ar.str("display_description", ""),
    };
}

[[nodiscard]] bool is_literal_pattern(std::string_view p) noexcept {
    constexpr std::string_view meta{".^$*+?()[]{}|\\"};
    return p.find_first_of(meta) == std::string_view::npos;
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
    std::string lower(content.size(), '\0');
    std::ranges::transform(content, lower.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::string nl(needle.size(), '\0');
    std::ranges::transform(needle, nl.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = lower.find(nl, pos)) != std::string::npos) {
        if (!record(pos)) return;
        pos += nl.size();
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
            if (out.size() > 100) { out.resize(99); out += "\xe2\x80\xa6"; }
            return out;
        }
        if (kind == 1) {
            if (fallback.empty()) {
                fallback.assign(s);
                if (fallback.size() > 100) { fallback.resize(99); fallback += "\xe2\x80\xa6"; }
            }
            best_indent = ind;
            continue;
        }
        best_indent = ind;
    }
    return fallback;
}

ExecResult run_ripgrep(const GrepArgs& a) {
    std::vector<std::string> argv = {"rg", "--json", "--no-config"};
    if (!a.case_sensitive) argv.push_back("-i");
    if (is_literal_pattern(a.pattern)) argv.push_back("-F");
    argv.push_back("-C");
    argv.push_back(std::to_string(kContext));
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
        return ToolOutput{"No matches found.", std::nullopt};
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
        return ToolOutput{"No matches found.", std::nullopt};

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
    const bool literal = is_literal_pattern(a.pattern);
    std::regex re;
    if (!literal) {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!a.case_sensitive) flags = flags | std::regex::icase;
        try { re = std::regex(a.pattern, flags); }
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
                if (util::should_skip_dir(fn)) it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(ec)) continue;
            if (fn.starts_with(".")) continue;
            if (!a.file_glob.empty()
                && !util::glob_match(a.file_glob, fn)) continue;
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
        return ToolOutput{
            "No matches found. Check the pattern syntax (this is ECMAScript "
            "regex, not PCRE — no look-behind, no named groups), try a "
            "broader pattern, or use `glob` first to narrow the file set.",
            std::nullopt};

    std::size_t files_with_hits = 0;
    for (const auto& h : hits) if (!h.path.empty()) ++files_with_hits;

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

        std::vector<std::pair<int,int>> page_ranges;
        for (const auto& li : lines) {
            if (skipped < a.offset) { ++skipped; continue; }
            if (shown >= kPerPage) break;
            int row = li.line_no - 1;
            int start = std::max(0, row - kContext);
            int end   = row + kContext;
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

    auto r = (detect_backend() == Backend::Ripgrep) ? run_ripgrep(gated)
                                                    : run_builtin(gated);
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
            {"glob",           {{"type","string"}, {"description","File extension filter (e.g. *.cpp)"}}},
            {"case_sensitive", {{"type","boolean"}, {"description","Case-sensitive match (default: false)"}}},
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
        "`[abc]` classes, and bare substrings. Matches against filename "
        "(not full path). Case-insensitive on Windows.",
        glob_schema(), EffectSet{Effect::ReadFs},
        body<GlobArgs>(run_glob, parse_glob_args), 25'000);

    sh.add("find_definition",
        "Find the definition of a symbol (function, class, struct, enum, type) "
        "across the codebase. Searches for common definition patterns in C/C++, "
        "Python, JavaScript/TypeScript, Go, and Rust.",
        find_definition_schema(), EffectSet{Effect::ReadFs},
        body<FindDefinitionArgs>(run_find_definition, parse_find_definition_args), 25'000);
}

} // namespace mcp::tools::detail
