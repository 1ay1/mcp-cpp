// SPDX-License-Identifier: Apache-2.0
//
// fs.cpp — register_fs_tools: read / write / list_dir (edit lives in
// fs_edit.cpp and is registered through register_edit_tool, called here).
// Faithful port of agentty's src/tool/tools/{read,write,list_dir}.cpp;
// FileChange (write) is carried via the detail::lower() meta bridge.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

// Forward — edit tool registers through its own TU.
void register_edit_tool(Shells& sh);

namespace {

// ─────────────────────────────────────────────────────────────────────────
//  read
// ─────────────────────────────────────────────────────────────────────────

struct ReadCacheKey {
    std::string canonical_path;
    int offset = 1;
    int limit  = 2000;
    bool operator==(const ReadCacheKey&) const noexcept = default;
};
struct ReadCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const ReadCacheKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.canonical_path);
        h = h * 31u + static_cast<std::size_t>(k.offset);
        h = h * 31u + static_cast<std::size_t>(k.limit);
        return h;
    }
};
struct ReadCache {
    std::mutex mu;
    std::unordered_map<ReadCacheKey, fs::file_time_type, ReadCacheKeyHash> seen;
};
[[nodiscard]] ReadCache& read_cache() {
    static ReadCache c;
    return c;
}

constexpr std::size_t kAutoOutlineSize = 32 * 1024;

[[nodiscard]] inline const std::regex& outline_pattern() {
    static const std::regex re(
        R"(^(\s*)((?:#{1,6}\s+\S.*$)|)"
        R"((?:(?:pub\s+|public\s+|private\s+|protected\s+|static\s+|)"
        R"(inline\s+|virtual\s+|async\s+|export\s+|export\s+default\s+|)"
        R"(extern(?:\s+"[^"]*")?\s+|template\s*<[^>]*>\s*)*)"
        R"((?:fn|def|class|struct|enum|impl|trait|interface|namespace|)"
        R"(function|module|component|service|directive)\b[^=]*)|)"
        R"((?:const|let|var)\s+\w+\s*(?:=|:)|)"
        R"(\w+\s*=\s*(?:async\s+)?(?:function|\([^)]*\)\s*=>)|)"
        R"((?:[\w:~<>\[\]&*\s,]+\s+)?\w+\s*\([^)]*\)\s*(?:const\s*)?\{?\s*$))",
        std::regex::ECMAScript | std::regex::optimize);
    return re;
}

[[nodiscard]] std::string render_outline(std::string_view content) {
    constexpr std::size_t kMaxEntries = 250;
    std::string out;
    out.reserve(content.size() / 16);
    int line_no = 0;
    std::size_t line_start = 0;
    std::size_t emitted = 0;
    auto emit_line = [&](std::string_view line) {
        if (emitted >= kMaxEntries) return;
        std::size_t l = 0;
        while (l < line.size() && (line[l] == ' ' || line[l] == '\t')) ++l;
        auto trimmed = line.substr(l);
        while (!trimmed.empty() && (trimmed.back() == ' '
                                    || trimmed.back() == '\t'
                                    || trimmed.back() == '\r')) {
            trimmed.remove_suffix(1);
        }
        if (trimmed.empty()) return;
        std::format_to(std::back_inserter(out), "[L{}] {}\n", line_no, trimmed);
        ++emitted;
    };
    const auto& re = outline_pattern();
    for (std::size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n') {
            ++line_no;
            auto line = content.substr(line_start, i - line_start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty()) {
                char c0 = line.front();
                bool maybe = (c0 != '}' && c0 != ')' && c0 != ']'
                              && c0 != ';' && c0 != '/');
                if (maybe) {
                    std::cmatch m;
                    if (std::regex_match(line.data(), line.data() + line.size(), m, re))
                        emit_line(line);
                }
            }
            line_start = i + 1;
        }
    }
    if (emitted >= kMaxEntries) {
        out += std::format("\n[outline truncated at {} entries; "
                           "use start_line/end_line to read specific regions]\n",
                           kMaxEntries);
    }
    return out;
}

struct ReadArgs {
    util::WorkspacePath path;
    int                 offset;
    int                 limit;
    std::string         display_description;
    bool                no_explicit_range = true;
};

std::expected<ReadArgs, ToolError> parse_read_args(const json& j) {
    util::ArgReader ar(j);
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    auto wp = util::make_readable_path_checked(*path_opt, "read");
    if (!wp) return std::unexpected(std::move(wp.error()));
    int offset = ar.integer("offset", 1);
    if (offset < 1) offset = 1;
    int limit = ar.integer("limit", 2000);
    if (ar.has("end_line") && !ar.has("limit")) {
        int end_line = ar.integer("end_line", 0);
        if (end_line >= offset) limit = end_line - offset + 1;
    }
    if (limit <= 0) limit = 2000;
    bool explicit_range = ar.has("offset") || ar.has("limit")
                       || ar.has("start_line") || ar.has("end_line");
    return ReadArgs{
        std::move(*wp), offset, limit,
        ar.str("display_description", ""),
        /*no_explicit_range=*/ !explicit_range,
    };
}

ExecResult run_read(const ReadArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));

    fs::file_time_type current_mtime{};
    {
        std::error_code mtime_ec;
        current_mtime = fs::last_write_time(p, mtime_ec);
        if (!mtime_ec) {
            std::error_code canon_ec;
            auto canon = fs::weakly_canonical(p, canon_ec);
            if (!canon_ec) {
                ReadCacheKey key{canon.string(), a.offset, a.limit};
                std::lock_guard lk{read_cache().mu};
                auto it = read_cache().seen.find(key);
                if (it != read_cache().seen.end() && it->second == current_mtime) {
                    return ToolOutput{
                        "File unchanged since last read. The content from the "
                        "earlier Read tool_result in this conversation is still "
                        "current \xe2\x80\x94 refer to that instead of re-reading.",
                        std::nullopt};
                }
            }
        }
    }
    constexpr uintmax_t kMaxBytes = 1024u * 1024u;
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kMaxBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 1 MiB cap). "
            "Read in chunks via offset/limit (or start_line/end_line) — "
            "e.g. {{\"path\":\"{}\",\"offset\":1,\"limit\":500}}. "
            "For a structural overview, run `grep` for the symbols you need.",
            sz / 1024, a.path.string())));
    }
    if (util::is_binary_file(p)) {
        return std::unexpected(ToolError::binary(std::format(
            "cannot read binary file: {} ({} bytes). "
            "Use the bash tool with `file`, `hexdump`, or similar.",
            a.path.string(), static_cast<uintmax_t>(ec ? 0 : sz))));
    }
    auto content = util::read_file(a.path);

    if (a.no_explicit_range && content.size() > kAutoOutlineSize) {
        std::size_t kib = content.size() / 1024;
        std::string outline = render_outline(content);
        std::string out;
        if (!outline.empty()) {
            out = std::format(
                "SUCCESS: File outline retrieved. This file is {} KiB "
                "and was returned as a structural overview instead "
                "of full content to save context.\n\n"
                "IMPORTANT: Do NOT retry this read without a line range "
                "\xe2\x80\x94 you will get the exact same outline back "
                "and waste a turn. To see real file content you MUST "
                "pass start_line + end_line (or offset + limit).\n\n"
                "# Outline of {}\n\n{}\n"
                "NEXT STEPS: to read a specific symbol's body, call "
                "read again with this path plus start_line and "
                "end_line covering the lines around the symbol "
                "(e.g. for `[L120] fn foo()`, try start_line=120, "
                "end_line=180).",
                kib, a.path.string(), outline);
        } else {
            constexpr std::size_t kPeekBytes = 1024;
            std::size_t cut = std::min(kPeekBytes, content.size());
            while (cut > 0
                   && (static_cast<unsigned char>(content[cut]) & 0xC0) == 0x80)
                --cut;
            std::size_t nl = content.rfind('\n', cut == 0 ? 0 : cut - 1);
            if (nl != std::string::npos && nl > 0) cut = nl + 1;
            int total_lines = 0;
            for (char c : content) if (c == '\n') ++total_lines;
            if (!content.empty() && content.back() != '\n') ++total_lines;
            std::string_view peek{content.data(), cut};
            out = std::format(
                "SUCCESS: First 1 KiB of a {} KiB file with no "
                "recognisable code structure (README / log / data dump). "
                "Returned a leading slice instead of the full body to "
                "save context.\n\n"
                "IMPORTANT: Do NOT retry this read without a line range "
                "\xe2\x80\x94 you will get the exact same slice back. To "
                "see more, pass start_line + end_line (or offset + "
                "limit); the file has {} lines total.\n\n"
                "# First 1 KiB of {}\n\n{}",
                kib, total_lines, a.path.string(), peek);
        }
        if (!a.display_description.empty())
            out = a.display_description + "\n" + out;
        if (current_mtime.time_since_epoch().count() != 0) {
            std::error_code canon_ec;
            auto canon = fs::weakly_canonical(p, canon_ec);
            if (!canon_ec) {
                ReadCacheKey key{canon.string(), a.offset, a.limit};
                std::lock_guard lk{read_cache().mu};
                read_cache().seen[std::move(key)] = current_mtime;
            }
        }
        util::record_file_seen(p, current_mtime,
                               static_cast<std::uintmax_t>(content.size()),
                               util::content_fnv1a(content));
        return ToolOutput{std::move(out), std::nullopt};
    }
    std::string out;
    out.reserve(content.size() < 1024 * 1024 ? content.size() : 1024 * 1024);
    int total_lines = 0;
    int shown = 0;
    size_t line_start = 0;
    const size_t N = content.size();
    for (size_t i = 0; i < N; ++i) {
        char c = content[i];
        if (c == '\0') {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path.string(), N)));
        }
        if (c == '\n') {
            ++total_lines;
            int n = total_lines;
            if (n >= a.offset && shown < a.limit) {
                size_t end = i;
                if (end > line_start && content[end - 1] == '\r') --end;
                out.append(content.data() + line_start, end - line_start);
                out.push_back('\n');
                ++shown;
            }
            line_start = i + 1;
        }
    }
    if (line_start < N) {
        ++total_lines;
        int n = total_lines;
        if (n >= a.offset && shown < a.limit) {
            size_t end = N;
            if (end > line_start && content[end - 1] == '\r') --end;
            out.append(content.data() + line_start, end - line_start);
            out.push_back('\n');
            ++shown;
        }
    }
    if (a.offset > 1 || shown < total_lines) {
        std::string hint = std::format("\n[showing lines {}-{} of {}",
                                       a.offset, a.offset + shown - 1, total_lines);
        int remaining = total_lines - (a.offset + shown - 1);
        if (remaining > 0)
            hint += std::format("; {} more — pass offset={} (or start_line={}) "
                                "for the next chunk",
                                remaining, a.offset + shown, a.offset + shown);
        hint += "]";
        out += hint;
    }
    if (!a.display_description.empty())
        out = a.display_description + "\n" + out;
    if (current_mtime.time_since_epoch().count() != 0) {
        std::error_code canon_ec;
        auto canon = fs::weakly_canonical(p, canon_ec);
        if (!canon_ec) {
            ReadCacheKey key{canon.string(), a.offset, a.limit};
            std::lock_guard lk{read_cache().mu};
            read_cache().seen[std::move(key)] = current_mtime;
        }
    }
    util::record_file_seen(p, current_mtime,
                           static_cast<std::uintmax_t>(content.size()),
                           util::content_fnv1a(content));
    return ToolOutput{std::move(out), std::nullopt};
}

// ─────────────────────────────────────────────────────────────────────────
//  write
// ─────────────────────────────────────────────────────────────────────────

struct WriteArgs {
    util::WorkspacePath path;
    std::string content;
    std::string display_description;
    std::string coercion_note;
};

constexpr std::string_view kMetadataKeys[] = {
    "path", "file_path", "filepath", "filename",
    "display_description", "description",
    "append", "mode", "encoding", "overwrite",
};

bool is_metadata_key(std::string_view k) noexcept {
    for (auto m : kMetadataKeys) if (k == m) return true;
    return false;
}

std::optional<std::string> salvage_largest_string(const json& j, std::string& which) {
    if (!j.is_object()) return std::nullopt;
    const std::string* best_key = nullptr;
    const std::string* best_val = nullptr;
    std::size_t best_len = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (is_metadata_key(it.key())) continue;
        if (!it->is_string()) continue;
        const auto& s = it->get_ref<const std::string&>();
        if (s.size() > best_len) { best_len = s.size(); best_key = &it.key(); best_val = &s; }
    }
    if (!best_val || best_len < 4) return std::nullopt;
    which = *best_key;
    return *best_val;
}

std::string describe_keys(const json& j) {
    if (!j.is_object() || j.empty()) return "(no object / empty)";
    std::string out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!out.empty()) out += ", ";
        out += it.key();
    }
    return out;
}

std::expected<WriteArgs, ToolError> parse_write_args(const json& j) {
    util::ArgReader ar(j);
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args(
            std::format("path required (received keys: {})", describe_keys(j))));
    auto wp = util::make_workspace_path_checked(*raw, "write");
    if (!wp) return std::unexpected(std::move(wp.error()));
    std::string note;
    std::string content;
    if (ar.has("content")) {
        content = ar.str("content", "", &note);
    } else {
        std::string picked_key;
        auto rescued = salvage_largest_string(j, picked_key);
        if (!rescued) {
            return std::unexpected(ToolError::invalid_args(std::format(
                "content required — no `content` field or known alias "
                "(file_text, text, body, data, contents, file_content) "
                "was present. Received keys: {}. "
                "Re-run with the full file body in the `content` field.",
                describe_keys(j))));
        }
        content = std::move(*rescued);
        note = std::format(" (content was pulled from non-standard key "
                           "`{}` — please use `content` next time)", picked_key);
    }
    return WriteArgs{std::move(*wp), std::move(content),
                     ar.str("display_description", ""), std::move(note)};
}

ExecResult run_write(const WriteArgs& a) {
    const auto& p = a.path.path();
    constexpr std::size_t kMaxWriteBytes = 5u * 1024u * 1024u;
    if (a.content.size() > kMaxWriteBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "write body is {} KiB (> 5 MiB cap). Split into multiple writes "
            "or stage the file via bash (cat > file <<EOF).",
            a.content.size() / 1024)));
    }
    std::string original;
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (exists && fs::is_directory(p, ec))
        return std::unexpected(ToolError::not_a_file(
            "'" + a.path.string() + "' is a directory — write needs a file path."));
    auto parent = p.parent_path();
    if (!parent.empty() && fs::exists(parent, ec) && !fs::is_directory(parent, ec))
        return std::unexpected(ToolError::not_a_directory(
            "parent of '" + a.path.string() + "' exists but is not a directory."));
    uintmax_t original_size = 0;
    if (exists) {
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError::not_a_file(
                "not a regular file: " + a.path.string()));
        original_size = fs::file_size(p, ec);
        if (ec) original_size = 0;
        if (original_size <= kMaxWriteBytes)
            original = util::read_file(a.path);
    }
    std::string staleness_warning;
    if (exists && util::staleness_of(p) == util::StaleVerdict::Stale) {
        staleness_warning =
            "\xe2\x9a\xa0  The file has changed on disk since the last time a tool "
            "observed it this session. The write OVERWROTE those changes — "
            "if that's not what you wanted, re-read the file and rewrite "
            "with the intended merged content.\n\n";
    }
    if (exists && !original.empty() && original == a.content)
        return ToolOutput{"File already matches content — no changes written.",
                          std::nullopt};
    FileChange change;
    if (!exists || (!original.empty() && original_size <= kMaxWriteBytes))
        change = make_change(a.path.string(), original, a.content);
    else
        change.path = a.path.string();
    if (auto err = util::write_file(a.path, a.content); !err.empty())
        return std::unexpected(ToolError::io(err));
    {
        std::error_code mt_ec;
        auto new_mtime = fs::last_write_time(p, mt_ec);
        if (!mt_ec) {
            util::record_file_seen(p, new_mtime,
                                   static_cast<std::uintmax_t>(a.content.size()),
                                   util::content_fnv1a(a.content));
        }
    }
    std::string prefix;
    if (!staleness_warning.empty()) prefix += staleness_warning;
    if (!a.display_description.empty()) prefix += a.display_description + "\n";
    auto msg = std::format("{}{} {} ({}+ {}-){}",
                           prefix, exists ? "Overwrote" : "Created",
                           a.path.string(), change.added, change.removed,
                           a.coercion_note);
    return ToolOutput{std::move(msg), std::move(change)};
}

// ─────────────────────────────────────────────────────────────────────────
//  list_dir
// ─────────────────────────────────────────────────────────────────────────

struct ListDirArgs {
    std::string root;
    bool recursive;
    int max_depth;
    std::string display_description;
};

std::expected<ListDirArgs, ToolError> parse_list_dir_args(const json& j) {
    util::ArgReader ar(j);
    int max_depth = std::clamp(ar.integer("max_depth", 3), 1, 16);
    return ListDirArgs{
        ar.str("path", "."),
        ar.boolean("recursive", false),
        max_depth,
        ar.str("display_description", ""),
    };
}

ExecResult run_list_dir(const ListDirArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "list_dir");
    if (!wp) return std::unexpected(std::move(wp.error()));
    std::error_code ec;
    if (!fs::exists(wp->path(), ec))
        return std::unexpected(ToolError::not_found("directory not found: " + a.root));
    if (!fs::is_directory(wp->path(), ec))
        return std::unexpected(ToolError::not_a_directory("not a directory: " + a.root));

    std::ostringstream out;
    int count = 0;
    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[32];
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0); return buf; }
        if (bytes < 1024*1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024.0)); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fG", bytes/(1024.0*1024.0*1024.0)); return buf;
    };
    auto list_entry = [&](const fs::directory_entry& entry, int depth) {
        if (count > 1000) return;
        std::string indent(depth * 2, ' ');
        auto fn = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            out << indent << fn << "/\n";
        } else if (entry.is_regular_file(ec)) {
            auto sz = entry.file_size(ec);
            out << indent << fn << "  " << format_size(ec ? 0 : sz) << "\n";
        } else if (entry.is_symlink(ec)) {
            std::error_code link_ec;
            auto target = fs::read_symlink(entry.path(), link_ec);
            if (link_ec)
                out << indent << fn << " -> <unreadable: " << link_ec.message() << ">\n";
            else
                out << indent << fn << " -> " << target.string() << "\n";
        }
        count++;
    };
    if (a.recursive) {
        for (auto it = fs::recursive_directory_iterator(wp->path(),
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > a.max_depth) { it.disable_recursion_pending(); continue; }
            const auto& entry = *it;
            auto fn = entry.path().filename().string();
            const bool is_dir = entry.is_directory(ec);
            if (is_dir && util::should_skip_dir(fn)) {
                list_entry(entry, it.depth());
                it.disable_recursion_pending();
                if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
                continue;
            }
            if (is_dir && it.depth() > 0 && fn.starts_with(".")) {
                it.disable_recursion_pending();
                continue;
            }
            list_entry(*it, it.depth());
            if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
        }
    } else {
        std::vector<fs::directory_entry> entries;
        for (auto& e : fs::directory_iterator(wp->path(), ec))
            entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            bool da = a.is_directory(), db = b.is_directory();
            if (da != db) return da > db;
            return a.path().filename() < b.path().filename();
        });
        for (auto& e : entries) list_entry(e, 0);
    }
    if (count == 0) return ToolOutput{"empty directory", std::nullopt};
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ── Schemas / descriptions ─────────────────────────────────────────────────

json read_schema() {
    return json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",       {{"type","string"}, {"description","Absolute or relative path"}}},
            {"offset",     {{"type","integer"}, {"description","Start line (1-based)"}}},
            {"limit",      {{"type","integer"}, {"description","Max lines"}}},
            {"start_line", {{"type","integer"}, {"description","Alias for offset (Zed-style)"}}},
            {"end_line",   {{"type","integer"}, {"description","Inclusive last line (Zed-style)"}}},
        }},
    };
}

json write_schema() {
    return json{
        {"type","object"},
        {"required", {"file_path","content"}},
        {"properties", {
            {"file_path", {{"type","string"},
                {"description","The absolute path to the file to write "
                               "(must be absolute, not relative)."}}},
            {"content",   {{"type","string"},
                {"description","The content to write to the file."}}},
        }},
    };
}

json list_dir_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",      {{"type","string"}, {"description","Directory to list (default: cwd)"}}},
            {"recursive", {{"type","boolean"}, {"description","List recursively (default: false)"}}},
            {"max_depth", {{"type","integer"}, {"description","Max depth for recursive listing (default: 3)"}}},
        }},
    };
}

} // namespace

void register_fs_tools(Shells& sh) {
    sh.add("read",
        "Read a file from the filesystem. Returns up to 2000 lines "
        "starting at an optional offset. For files over 32 KiB, "
        "reading without an explicit line range returns a SYMBOL "
        "OUTLINE (function / class / heading names with line "
        "numbers) \xe2\x80\x94 or, if the file has no code "
        "structure, its first 1 KiB \xe2\x80\x94 instead of the "
        "full content; use start_line + end_line (or offset + "
        "limit) on a follow-up read to fetch the specific section "
        "you want. Include a brief `display_description` so the "
        "user sees why you're reading.",
        read_schema(), EffectSet{Effect::ReadFs},
        body<ReadArgs>(run_read, parse_read_args), 80'000);

    sh.add("write",
        "Writes a file to the local filesystem.\n\n"
        "Usage:\n"
        "- This tool will overwrite the existing file if there is one at the "
        "provided path.\n"
        "- If this is an existing file, you MUST use the Read tool first to "
        "read the file's contents.\n"
        "- Prefer the Edit tool for modifying existing files — it only sends "
        "the diff. Only use this tool to create new files or for complete "
        "rewrites.",
        write_schema(), EffectSet{Effect::WriteFs},
        body<WriteArgs>(run_write, parse_write_args), 40'000);

    sh.add("list_dir",
        "List the contents of a directory. Shows file type, size, and name. "
        "Use this to explore project structure before reading files.",
        list_dir_schema(), EffectSet{Effect::ReadFs},
        body<ListDirArgs>(run_list_dir, parse_list_dir_args), 25'000);

    // edit (own TU — fuzzy splice logic is large)
    register_edit_tool(sh);
}

} // namespace mcp::tools::detail
