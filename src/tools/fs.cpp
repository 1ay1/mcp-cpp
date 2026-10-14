// SPDX-License-Identifier: Apache-2.0
//
// fs.cpp — register_fs_tools: read / write / list_dir / outline / move /
// remove. edit is in fs_edit.cpp, outline rendering in outline.cpp.

#include "tool_shell.hpp"
#include "call_state.hpp"
#include "tool_body.hpp"
#include "outline.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/utf8.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
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

// `read`'s memory: what the caller still HAS, per (reader, file). It lives
// in the tool state (ToolState::reads) so the host owns it.
//
// mtime alone is not enough: the sentinel says "refer to the earlier
// tool_result", which is a lie unless that result is still in the
// conversation AND covered the range now being asked for. agentty collapses
// every read of a file except the most recent one, and a cache that refused
// any repeat of a range it had ever served pointed callers at bytes that
// existed nowhere. So mirror supersession: ONE live range per file,
// overwritten on every read. A repeat is answered from the entry only when
// it asks for the same mtime AND a range the live entry actually covers.
//
// The reader is part of the key: a fresh subagent must not be told it
// already has a file its parent read.
using ReadCacheEntry = state::ReadSeen;

// Does the range still in context contain the range being asked for?
//
// Conservative on purpose: anything we cannot prove is covered gets served.
// A wrong "yes" is unrecoverable (the caller is told to look at bytes that
// do not exist), a wrong "no" costs one re-read. That asymmetry decides
// every ambiguous case here.
//
// A NEGATIVE offset is a tail request ("last N lines"), which resolves
// against the line count and cannot be compared without reading the file.
// Never refuse one.
//
// `total_lines` is the file's real length, used for one case the range
// arithmetic alone gets wrong: a request starting past EOF. The earlier
// whole-file read technically "covers" line 1000 of a 3-line file, but the
// caller asking for it has made a mistake and needs the diagnostic
// ("offset 1000 is past the end of the file, which has 3 lines"), not a
// pointer back to a result that cannot answer them. Pass 0 when unknown.
[[nodiscard]] inline bool covers(const ReadCacheEntry& e,
                                int want_offset, int want_limit,
                                int total_lines = 0) noexcept {
    if (want_offset < 0 || e.offset < 0) return false;   // tail: always serve
    if (total_lines > 0 && want_offset > total_lines)
        return false;                                    // past EOF: diagnose
    if (e.limit <= 0) return true;                       // held the whole file
    if (want_limit <= 0) return false;                   // wants whole, held part
    const long long have_lo = e.offset;
    const long long have_hi = have_lo + e.limit - 1;
    const long long want_lo = want_offset;
    const long long want_hi = want_lo + want_limit - 1;
    return want_lo >= have_lo && want_hi <= have_hi;
}


constexpr std::size_t kAutoOutlineSize = 32 * 1024;

// How much to reserve for a windowed read's output.
//
// Not the file size: a 250-line window of a 900 KiB file emits a few KiB, so
// reserving the whole file is a large allocation to hold a small answer.
// Guess ~120 bytes per requested line (generous for source; long-line files
// just grow the string a few times), clamped so a huge `limit` cannot ask
// for more than the file could possibly contain.
[[nodiscard]] inline std::size_t N_reserve_hint(std::size_t file_size,
                                               int limit) noexcept {
    if (limit <= 0) return file_size;
    const std::size_t guess = static_cast<std::size_t>(limit) * 120u;
    return guess < file_size ? guess : file_size;
}

struct ReadArgs {
    std::string         raw_path;
    std::optional<int>  offset_in;   // NEGATIVE = tail
    std::optional<int>  limit_in;    // a count
    std::optional<int>  end_line;    // a position; wins over limit_in
    std::string         symbol;      // read just this symbol's definition + body
    std::string         display_description;
    json                raw;         // which keys arrived (multi-path refusal)
    std::optional<util::WorkspacePath> path;
    int                 offset = 1;
    int                 limit = 2000;
    bool                no_explicit_range = true;
};

// A range written as "10-20", "10:20", "10", [10,20] or {start,end}.
// Returns {from, to}; to is 0 when open-ended.
std::optional<std::pair<int, int>> parse_line_range(const json& v) {
    auto as_int = [](const json& x) -> std::optional<int> {
        if (x.is_number_integer()) return x.get<int>();
        if (x.is_string()) {
            try { return std::stoi(x.get<std::string>()); } catch (...) {}
        }
        return std::nullopt;
    };
    if (v.is_array() && !v.empty()) {
        auto a = as_int(v[0]);
        if (!a) return std::nullopt;
        auto z = v.size() > 1 ? as_int(v[1]) : std::nullopt;
        return std::pair{*a, z.value_or(0)};
    }
    if (v.is_object()) {
        for (auto [s, e] : {std::pair{"start", "end"}, std::pair{"from", "to"}})
            if (v.contains(s))
                if (auto a = as_int(v[s])) {
                    auto z = v.contains(e) ? as_int(v[e]) : std::nullopt;
                    return std::pair{*a, z.value_or(0)};
                }
        return std::nullopt;
    }
    if (v.is_number_integer()) return std::pair{v.get<int>(), 0};
    if (!v.is_string()) return std::nullopt;
    std::string s = v.get<std::string>();
    std::erase_if(s, [](char c) { return c == ' ' || c == 'L' || c == 'l'; });
    const auto sep = s.find_first_of("-:,");
    try {
        if (sep == std::string::npos) return std::pair{std::stoi(s), 0};
        const int a = std::stoi(s.substr(0, sep));
        const std::string rest = s.substr(sep + 1);
        return std::pair{a, rest.empty() ? 0 : std::stoi(rest)};
    } catch (...) {
        return std::nullopt;
    }
}

// The shapes models send instead of path/offset/end_line, folded into those:
// a quoted path, "file:10-20" / "file#L10-L20", lines:"10-20",
// line_range:[10,20], and a one-entry paths:[...] array.
json tidy_read_args(const json& j, const util::Bounds& b) {
    if (!j.is_object()) return j;
    json out = j;
    if (!out.contains("path")) {
        for (const char* k : {"paths", "files", "file_paths"})
            if (auto it = out.find(k); it != out.end() && it->is_array() && it->size() == 1
                && (*it)[0].is_string()) {
                out["path"] = (*it)[0];
                break;
            }
    }
    bool has_range = out.contains("offset") || out.contains("start_line")
                  || out.contains("end_line") || out.contains("limit");
    if (!has_range)
        for (const char* k : {"lines", "line_range", "range", "line"})
            if (auto it = out.find(k); it != out.end())
                if (auto r = parse_line_range(*it); r && r->first > 0) {
                    if (std::string_view{k} == "line" && r->second == 0) {
                        // One line asked for: show it with some context.
                        out["offset"] = std::max(1, r->first - 20);
                        out["end_line"] = r->first + 40;
                    } else {
                        out["offset"] = r->first;
                        if (r->second >= r->first) out["end_line"] = r->second;
                    }
                    has_range = true;
                    break;
                }
    // Path under whatever key the model used.
    const char* pkey = "path";
    for (const char* k : {"path", "file_path", "file", "filename", "filepath", "filePath"})
        if (out.contains(k)) { pkey = k; break; }
    if (auto it = out.find(pkey); it != out.end() && it->is_string() && b.files) {
        auto t = util::tidy_model_path(*b.files, b, it->get<std::string>());
        *it = t.path;
        if (t.line_from > 0 && !has_range) {
            out["offset"] = t.line_from;
            if (t.line_to >= t.line_from) out["end_line"] = t.line_to;
        }
    }
    // One spelling per field: `end_line` is a POSITION (to_line, endLine say
    // the same); start_line / start / from_line are the offset.
    for (const char* k : {"to_line", "endLine"})
        if (auto it = out.find(k); it != out.end() && !out.contains("end_line")) {
            out["end_line"] = *it;
            out.erase(it);
        }
    return out;
}

// What the spec can't say: one path, not several; the path is readable; and
// the line window, which depends on which of offset / limit / end_line came.
std::expected<ReadArgs, ToolError> check_read_args(ReadArgs a, const util::Bounds& b) {
    if (a.raw_path.empty()) {
        if (a.raw.is_object())
            for (const char* k : {"paths", "files", "file_paths"})
                if (auto it = a.raw.find(k); it != a.raw.end() && it->is_array() && it->size() > 1)
                    return std::unexpected(ToolError::invalid_args(std::format(
                        "read takes one `path`; got {} in `{}`. Make one read call per file "
                        "(they run in parallel).", it->size(), k)));
        // Name the keys that DID arrive: a typo in the key and an argument
        // object that never arrived look the same otherwise.
        return std::unexpected(ToolError::invalid_args(std::format(
            "path required (received keys: {})", util::ArgReader(a.raw).received_keys())));
    }
    auto wp = util::make_readable_path_checked(a.raw_path, "read", b);
    if (!wp) return std::unexpected(std::move(wp.error()));

    // NEGATIVE offset = tail (offset:-50 is the last 50 lines), resolved
    // against the real line count in run_read.
    int offset = a.offset_in.value_or(1);
    if (offset == 0) offset = 1;
    // A start with no end wants a focused window, not the rest of the file;
    // a bare read(path) keeps the whole-file default. An explicit end wins.
    constexpr int kPageWindow = 250;
    const bool anchored_start = a.offset_in.has_value();
    const bool explicit_end   = a.limit_in.has_value() || a.end_line.has_value();
    const int default_limit = (anchored_start && !explicit_end) ? kPageWindow : 2000;
    int limit = a.limit_in.value_or(default_limit);
    // end_line is a POSITION: the window is [offset, end_line].
    if (a.end_line && *a.end_line > 0 && offset > 0 && *a.end_line < offset)
        return std::unexpected(ToolError::invalid_args(std::format(
            "end_line {} is before start_line {}; give the lower line first "
            "(start_line: {}, end_line: {})", *a.end_line, offset, *a.end_line, offset)));
    if (a.end_line && *a.end_line >= offset) limit = *a.end_line - offset + 1;
    if (limit <= 0) limit = default_limit;

    a.path   = std::move(*wp);
    a.offset = offset;
    a.limit  = limit;
    // symbol= is an explicit selection too: no whole-file auto-outline.
    a.no_explicit_range = !anchored_start && !explicit_end && a.symbol.empty();
    return a;
}

const util::ArgSpec<ReadArgs> kReadArgs = [] {
    util::ArgSpec<ReadArgs> s{
        util::arg(&ReadArgs::display_description, "display_description",
                  "One-line summary shown in the UI. Optional."),
        util::arg(&ReadArgs::raw_path, "path", "Absolute or relative path").advertise_required(),
        util::arg(&ReadArgs::symbol, "symbol", "Read ONLY this symbol's definition + body (function/class/etc.), resolved to its enclosing block — no line numbers or `sed` needed. e.g. symbol=\"parse_args\"."),
        util::arg(&ReadArgs::offset_in, "offset", "Start line (1-based). NEGATIVE = tail: offset:-50 returns the LAST 50 lines (like tail -n 50) — ideal for logs and build output, no need to know the file length."),
        util::arg(&ReadArgs::limit_in, "limit", "Max lines"),
        util::arg(&ReadArgs::end_line, "end_line", "Inclusive last line (Zed-style)"),
    };
    s.tidy(&tidy_read_args).raw(&ReadArgs::raw);
    return s;
}();

ExecResult run_read(const Call& call, const ReadArgs& a) {
    const auto& p = a.path->path();
    std::error_code ec;
    if (!call.fs().stat(p).exists) {
        // Same name somewhere else in the parent, or a different case/extension.
        std::string near;
        const auto parent = p.parent_path();
        const auto want = p.filename().string();
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        if (call.fs().stat(parent).directory) {
            int shown = 0;
            for (const auto& e : call.fs().list(parent, ec)) {
                const auto name = e.path.filename().string();
                const bool close = lower(name) == lower(want)
                    || e.path.stem() == p.stem()
                    || (want.size() >= 4 && lower(name).find(lower(p.stem().string())) != std::string::npos);
                if (!close) continue;
                near += (shown++ ? ", " : "") + name;
                if (shown == 5) break;
            }
        }
        return std::unexpected(ToolError::not_found("file not found: " + a.path->string()
            + (near.empty() ? std::string{". Run `list_dir` on the parent directory or `glob` by name to verify."}
                            : ". Similar names in " + parent.string() + ": " + near + ".")));
    }
    if (call.fs().stat(p).directory) {
        // A directory: show what's in it rather than fail.
        auto entries = call.fs().list(p, ec);
        std::ranges::sort(entries, [](const DirEntry& x, const DirEntry& y) {
            return x.directory != y.directory ? x.directory : x.path.filename() < y.path.filename();
        });
        std::string out = a.path->string() + " is a directory. Its entries:\n";
        std::size_t n = 0;
        for (const auto& e : entries) {
            if (++n > 200) { out += "... (" + std::to_string(entries.size() - 200) + " more)\n"; break; }
            out += "  " + e.path.filename().string() + (e.directory ? "/" : "") + "\n";
        }
        out += "Read a file by its full path.";
        return ToolOutput{std::move(out), std::nullopt};
    }
    if (!call.fs().stat(p).regular)
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path->string()));

    fs::file_time_type current_mtime{};
    {
        std::error_code mtime_ec;
        current_mtime = call.fs().stat(p).mtime;
        if (!mtime_ec && a.symbol.empty()) {
            std::error_code canon_ec;
            auto canon = call.fs().canonical(p, canon_ec);
            if (!canon_ec) {
                auto seen = call.with([&](ToolState& st) -> std::optional<ReadCacheEntry> {
                    auto it = st.reads.find({call.reader, canon.string()});
                    if (it == st.reads.end()) return std::nullopt;
                    return it->second;
                });
                // Refuse ONLY when the live entry still covers what is being
                // asked for. Same file, same mtime, but a range the earlier
                // result did not include means those bytes are nowhere in
                // the caller's context, and the sentinel would be a lie that
                // no retry can get past. Coverage is computed against the
                // RAW args, before symbol/tail resolution, because that is
                // what the caller can see and reason about.
                if (seen
                    && seen->mtime == current_mtime
                    && covers(*seen, a.offset, a.limit, seen->total)
                    && !seen->content.empty()) {
                    // SERVE, don't refuse.
                    //
                    // This used to return a bare "refer to the earlier
                    // tool_result instead" and nothing else, which is a
                    // pointer into the caller's context -- sound only while
                    // that result is still there. Compaction, a wire-level
                    // read-collapse and subagent boundaries all break that
                    // assumption, and when it breaks the caller is told to
                    // consult bytes that exist nowhere. Measured outcome: the
                    // model falls back to `cat`, which is the read-loop
                    // KhazAkar reported and the same failure Claude Code has
                    // open as issues #53578 and #60684.
                    //
                    // Annotating rather than refusing keeps the token saving
                    // that motivated the cache (no re-read of the file, no
                    // second line scan) while making the answer unconditional.
                    // The tag is for the MODEL: it says "you have seen this,
                    // it has not changed" so a careful one does not re-reason
                    // over it, and a confused one still gets the content.
                    return ToolOutput{
                        "[cached \xe2\x80\x94 unchanged since your last read of this "
                        "file]\n" + seen->content,
                        std::nullopt};
                }
            }
        }
    }
    constexpr uintmax_t kMaxBytes = 1024u * 1024u;
    uintmax_t sz = call.fs().stat(p).size;

    // Image files: hand the picture to a vision model instead of refusing it
    // as binary. Sniffed by magic bytes (extension-agnostic), capped larger
    // than text since a screenshot is legitimately a few MiB. The host maps
    // ToolOutput.images onto this call's tool_result image blocks; a non-vision
    // model just sees the text note. Checked BEFORE the 1 MiB text cap so a
    // normal-sized image isn't rejected for being "too large".
    if (a.symbol.empty()) {
        if (auto mime = util::sniff_image_media_type(call.fs(), p); !mime.empty()) {
            constexpr uintmax_t kMaxImageBytes = 8u * 1024u * 1024u;
            if (!ec && sz > kMaxImageBytes)
                return std::unexpected(ToolError::too_large(std::format(
                    "image is {} KiB (> 8 MiB cap): {}",
                    sz / 1024, a.path->string())));
            std::string raw = util::read_file(*a.path);
            if (raw.empty())
                return std::unexpected(ToolError::io(
                    "cannot read image " + a.path->string()));
            ToolOutput out;
            out.text = std::format(
                "[image {} \xc2\xb7 {} \xc2\xb7 {} KiB] attached to this "
                "result; if you can see images it is shown above.",
                a.path->string(), mime, raw.size() / 1024);
            out.images.push_back(util::ToolImage{std::move(mime), std::move(raw)});
            return out;
        }
    }

    if (!ec && sz > kMaxBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 1 MiB cap). "
            "Read in chunks via offset/limit (or start_line/end_line) — "
            "e.g. {{\"path\":\"{}\",\"offset\":1,\"limit\":500}}. "
            "For a structural overview, run `grep` for the symbols you need.",
            sz / 1024, a.path->string())));
    }
    if (util::is_binary_file(call.fs(), p)) {
        return std::unexpected(ToolError::binary(std::format(
            "cannot read binary file: {} ({} bytes). "
            "Use the `shell` tool with `file`, `hexdump`, or similar.",
            a.path->string(), static_cast<uintmax_t>(ec ? 0 : sz))));
    }
    auto content = util::read_file(*a.path);

    // symbol=: resolve to the defining line + enclosing block, then read just
    // that range — no `sed`, no line arithmetic. Overrides offset/limit.
    int eff_offset = a.offset;
    int eff_limit  = a.limit;
    // Tail semantics: offset:-N = the last N lines (limit is ignored — the
    // request IS the window). Count lines once, then convert to a normal
    // forward range.
    //
    // std::count, not a hand-rolled loop and not memchr: this one needs ONLY
    // a count (no line_start to carry), which is exactly the shape the
    // library can vectorise best — 0.15 ms vs 0.24 for memchr and 0.46 for
    // a byte loop that also tracks a position, over an 889 KiB span.
    // Contrast the count phase further down, which does need the position
    // and therefore uses memchr.
    if (eff_offset < 0) {
        int total = static_cast<int>(
            std::count(content.begin(), content.end(), '\n'));
        if (!content.empty() && content.back() != '\n') ++total;
        int want = -eff_offset;
        if (want > total) want = total;
        eff_offset = total - want + 1;
        eff_limit  = want;
    }
    std::string symbol_header;
    if (!a.symbol.empty()) {
        bool decl_only = false;
        auto rng = outline::symbol_range(content, a.symbol, &decl_only);
        // Found only a declaration (`int f(int);` in a header): the body the
        // model wants is usually in the paired source — try there first and
        // fall back to the declaration if it isn't.
        if (!rng || decl_only) {
            // Common miss: the model names the header but the body is in the
            // .cpp (or the reverse), or it's in a sibling file of the same
            // directory. Look there before giving up, and say where we went.
            // Only files next to / paired with the named one: cheap, bounded,
            // and never wanders the tree.
            std::vector<fs::path> near;
            {
                static constexpr std::string_view kSrc[] = {".cpp", ".cc", ".cxx", ".c", ".mm"};
                static constexpr std::string_view kHdr[] = {".hpp", ".h", ".hh", ".hxx"};
                const auto stem = p.stem().string();
                const auto dir  = p.parent_path();
                const auto ext  = p.extension().string();
                auto push = [&](const fs::path& q) {
                    std::error_code qec;
                    if (q != p && call.fs().stat(q).regular
                        && std::find(near.begin(), near.end(), q) == near.end())
                        near.push_back(q);
                };
                const bool is_hdr = std::ranges::find(kHdr, ext) != std::end(kHdr);
                const bool is_src = std::ranges::find(kSrc, ext) != std::end(kSrc);
                // Pair across include/ <-> src/ too: include/x/y/z.hpp <-> src/y/z.cpp
                auto swap_root = [&](std::string_view from, std::string_view to) {
                    std::string s = dir.string();
                    auto at = s.find(std::string{"/"} + std::string{from} + "/");
                    if (at == std::string::npos) return fs::path{};
                    std::string rest = s.substr(at + from.size() + 2);
                    fs::path base = fs::path{s.substr(0, at)} / std::string{to};
                    // include/<project>/a/b -> src/a/b (drop the project dir)
                    if (from == "include") {
                        auto slash = rest.find('/');
                        rest = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);
                    }
                    return rest.empty() ? base : base / rest;
                };
                if (is_hdr) {
                    for (auto e : kSrc) push(dir / (stem + std::string{e}));
                    if (auto d2 = swap_root("include", "src"); !d2.empty())
                        for (auto e : kSrc) push(d2 / (stem + std::string{e}));
                }
                if (is_src) {
                    for (auto e : kHdr) push(dir / (stem + std::string{e}));
                }
                // Siblings with the same extension family, capped.
                std::error_code dec;
                int budget = 60;
                for (const auto& e : call.fs().list(dir, dec)) {
                    if (--budget < 0) break;
                    if (!e.regular) continue;
                    const auto x = e.path.extension().string();
                    if (std::ranges::find(kSrc, x) != std::end(kSrc)
                        || std::ranges::find(kHdr, x) != std::end(kHdr)
                        || x == ext)
                        push(e.path);
                }
            }
            for (const auto& q : near) {
                if (call.fs().stat(q).size > kMaxBytes) continue;
                std::string other;
                try { other = util::read_file(call.fs(), q); } catch (...) { continue; }
                bool other_decl = false;
                auto r2 = outline::symbol_range(other, a.symbol, &other_decl);
                if (!r2 || (rng && other_decl)) continue;
                // Render the span from that file, same plain-lines shape as
                // a normal read.
                std::string body;
                int ln = 1; std::size_t st = 0;
                for (std::size_t i = 0; i <= other.size(); ++i) {
                    if (i == other.size() || other[i] == '\n') {
                        if (ln >= r2->first && ln <= r2->second) {
                            std::size_t e = i;
                            if (e > st && other[e - 1] == '\r') --e;
                            body.append(other, st, e - st);
                            body.push_back('\n');
                        }
                        if (ln > r2->second) break;
                        ++ln; st = i + 1;
                    }
                }
                return ToolOutput{util::to_valid_utf8(std::format(
                    "SUCCESS: `{}` is {} in {}; its definition is in {}:{} "
                    "(lines {}\xe2\x80\x93{}).\n\n{}",
                    a.symbol, rng ? "only declared" : "not defined",
                    a.path->string(), q.string(), r2->first,
                    r2->first, r2->second, body)), std::nullopt};
            }
            if (!rng)
            return std::unexpected(ToolError::not_found(std::format(
                "no definition of `{}` found in {} (or its paired header/source "
                "and sibling files). Use `find_definition` to search the "
                "whole tree, or drop `symbol` to read the whole file.",
                a.symbol, a.path->string())));
        }
        eff_offset = rng->first;
        eff_limit  = rng->second - rng->first + 1;
        symbol_header = std::format(
            "SUCCESS: `{}` defined at {}:{} (lines {}\xe2\x80\x93{}).\n\n",
            a.symbol, a.path->string(), rng->first, rng->first, rng->second);
    }

    if (a.no_explicit_range && content.size() > kAutoOutlineSize) {
        std::size_t kib = content.size() / 1024;
        std::string shape = outline::render(content);
        std::string out;
        if (!shape.empty()) {
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
                kib, a.path->string(), shape);
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
                kib, total_lines, a.path->string(), peek);
        }
        if (!a.display_description.empty())
            out = a.display_description + "\n" + out;
        if (current_mtime.time_since_epoch().count() != 0) {
            std::error_code canon_ec;
            auto canon = call.fs().canonical(p, canon_ec);
            if (!canon_ec) {
                // The OUTLINE path: the caller got a symbol map of the whole
                // file, not its text. Record it as covering nothing, so any
                // later request for actual lines is served. Refusing one
                // with "you already read this" would be answering a request
                // for content with a table of contents.
                call.with([&](ToolState& st) {
                    st.reads[{call.reader, canon.string()}] = ReadCacheEntry{current_mtime, 1, 1, 0, {}};
                });
            }
        }
        record_seen(call, p, current_mtime,
                               static_cast<std::uintmax_t>(content.size()),
                               util::cheap_content_hash(content));
        return ToolOutput{std::move(out), std::nullopt};
    }
    std::string out;
    // Reserve for the WINDOW, not the whole file. A 250-line window of a
    // 900 KiB file asked for ~900 KiB here to emit ~9 KiB.
    out.reserve(N_reserve_hint(content.size(), eff_limit));
    int total_lines = 0;
    int shown = 0;
    size_t line_start = 0;
    const size_t N = content.size();
    // ── Emit phase ──────────────────────────────────────────────────────
    // Byte-at-a-time, but ONLY while output is still being produced.
    //
    // This loop used to run to EOF on every read, because the footer needs
    // total_lines ("of N", "pass offset=..."). So a 250-line window of a
    // 900 KiB file scanned all 900 KiB one byte at a time: measured 1.66 ms,
    // ~92% of the whole call, and IDENTICAL to reading the entire file.
    // Paging a big file cost exactly as much as not paging it.
    size_t i = 0;
    for (; i < N; ++i) {
        char c = content[i];
        if (c == '\0') {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path->string(), N)));
        }
        if (c == '\n') {
            ++total_lines;
            int n = total_lines;
            if (n >= eff_offset && shown < eff_limit) {
                size_t end = i;
                if (end > line_start && content[end - 1] == '\r') --end;
                out.append(content.data() + line_start, end - line_start);
                out.push_back('\n');
                ++shown;
            }
            line_start = i + 1;
            // Window filled: stop emitting and switch to counting.
            if (shown >= eff_limit) { ++i; break; }
        }
    }
    // ── Count phase ─────────────────────────────────────────────────────
    // Past the window every remaining line only contributes to total_lines
    // (and to line_start, for the trailing-line handler below), so this half
    // does no formatting and no appending.
    //
    // memchr, measured 0.24 ms vs 0.46 ms for a byte loop over an 889 KiB
    // tail. Worth stating how that number was nearly got wrong: an isolated
    // microbenchmark said the byte loop was 3x FASTER, because it dropped
    // the line_start bookkeeping and the compiler then vectorised the whole
    // loop into something this code cannot use. Benchmark the work the
    // function actually does, not a simplified version of it.
    //
    // (std::count is faster still at 0.15 ms, but it only returns a count
    // and cannot report where the last newline was.)
    //
    // The NUL scan covers the same span — a file can be text for 250 lines
    // and binary after — and NULs are genuinely sparse there, which is the
    // case memchr is best at.
    if (i < N) {
        const char* base = content.data();
        if (std::memchr(base + i, '\0', N - i) != nullptr) {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path->string(), N)));
        }
        const char* s = base + i;
        const char* e = base + N;
        while (const char* nl = static_cast<const char*>(
                   std::memchr(s, '\n', static_cast<std::size_t>(e - s)))) {
            ++total_lines;
            s = nl + 1;
            line_start = static_cast<std::size_t>(s - base);
        }
    }
    if (line_start < N) {
        ++total_lines;
        int n = total_lines;
        if (n >= eff_offset && shown < eff_limit) {
            size_t end = N;
            if (end > line_start && content[end - 1] == '\r') --end;
            out.append(content.data() + line_start, end - line_start);
            out.push_back('\n');
            ++shown;
        }
    }
    if (eff_offset > 1 || shown < total_lines) {
        std::string hint;
        if (shown == 0) {
            hint = std::format(
                "\n[offset {} is past the end of the file, which has {} line{} "
                "— nothing to show. Re-read with an offset ≤ {}.]",
                eff_offset, total_lines, total_lines == 1 ? "" : "s",
                total_lines);
        } else {
            hint = std::format("\n[showing lines {}-{} of {}",
                               eff_offset, eff_offset + shown - 1, total_lines);
            int remaining = total_lines - (eff_offset + shown - 1);
            if (remaining > 0 && a.symbol.empty())
                hint += std::format("; {} more — pass offset={} (or start_line={}) "
                                    "for the next chunk",
                                    remaining, eff_offset + shown, eff_offset + shown);
            hint += "]";
        }
        out += hint;
    }
    // Heads-up prefixes for files a model can otherwise misread as "read
    // failed" or waste turns paging line-by-line through.
    std::string heads_up;
    if (N == 0) {
        heads_up = "(file is empty — 0 bytes)\n";
    } else if (a.symbol.empty()) {
        // Minified / single-huge-line files (bundled JS, one-line JSON/SVG):
        // line-based paging is nearly useless, so say so once and point at a
        // better strategy. Trigger on a very high average line length across a
        // non-trivial file.
        const std::size_t avg_line = N / static_cast<std::size_t>(
            total_lines > 0 ? total_lines : 1);
        if (N > 4096 && avg_line > 800) {
            heads_up = std::format(
                "(heads-up: this file averages ~{} chars/line over {} line(s) "
                "— it looks minified or machine-generated, so line paging is "
                "coarse. `grep` for a token, or read a byte range, to target a "
                "spot.)\n", avg_line, total_lines);
        }
    }
    if (!heads_up.empty()) out = heads_up + out;
    if (!symbol_header.empty()) out = symbol_header + out;
    if (!a.display_description.empty())
        out = a.display_description + "\n" + out;
    if (current_mtime.time_since_epoch().count() != 0) {
        std::error_code canon_ec;
        auto canon = call.fs().canonical(p, canon_ec);
        if (!canon_ec) {
            // OVERWRITE, never accumulate. This mirrors what the host does
            // to the transcript: agentty keeps only the most recent read of
            // a file and collapses the rest, so the newest range is the only
            // one the caller still has. Keeping a union here would refuse
            // ranges whose bytes have already been collapsed away.
            //
            // Stored as the RAW request (a.offset/a.limit), not the resolved
            // eff_* — a symbol= read resolves to line numbers the caller
            // never asked for and cannot ask for again, so recording those
            // would refuse an unrelated later range that happens to overlap.
            //
            // `out` is exactly what the caller receives, headers and all, so
            // a cache hit replays the identical answer under a [cached] tag
            // rather than pointing at a tool_result that may be gone. Bounded
            // by kMaxBytes per live path, and OVERWRITE means one entry per
            // file, not a growing set.
            call.with([&](ToolState& st) {
                st.reads[{call.reader, canon.string()}] =
                    ReadCacheEntry{current_mtime, a.offset, a.limit, total_lines, out};
            });
        }
    }
    record_seen(call, p, current_mtime,
                           static_cast<std::uintmax_t>(content.size()),
                           util::cheap_content_hash(content));
    return ToolOutput{std::move(out), std::nullopt};
}

// ─────────────────────────────────────────────────────────────────────────
//  write
// ─────────────────────────────────────────────────────────────────────────

struct WriteArgs {
    std::string raw_path;
    std::string content;
    std::string display_description;
    json raw;
    std::optional<util::WorkspacePath> path;
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

// The spec reads path and content. What it can't: resolve the path, and
// rescue the body when a model put it under an unknown key.
std::expected<WriteArgs, ToolError> check_write_args(WriteArgs a, const util::Bounds& b) {
    auto wp = util::make_workspace_path_checked(a.raw_path, "write", b);
    if (!wp) return std::unexpected(std::move(wp.error()));
    if (!util::ArgReader(a.raw).has("content")) {
        std::string picked_key;
        auto rescued = salvage_largest_string(a.raw, picked_key);
        if (!rescued)
            return std::unexpected(ToolError::invalid_args(std::format(
                "content required — no `content` field or known alias "
                "(file_text, text, body, data, contents, file_content) "
                "was present. Received keys: {}. "
                "Re-run with the full file body in the `content` field.",
                util::ArgReader(a.raw).received_keys())));
        a.content = std::move(*rescued);
        a.coercion_note = std::format(" (content was pulled from non-standard key "
                                      "`{}` — please use `content` next time)", picked_key);
    } else if (const auto* v = util::ArgReader(a.raw).raw("content"); v && !v->is_string()) {
        // An array or object body was coerced to text; say so.
        a.coercion_note = v->is_array() ? " (content was an array — joined with newlines)"
                                        : " (content was not a string — coerced)";
    }
    a.path = std::move(*wp);
    return a;
}

const util::ArgSpec<WriteArgs> kWriteArgs = [] {
    util::ArgSpec<WriteArgs> s{
        util::arg(&WriteArgs::display_description, "display_description",
                  "One-line summary shown in the UI. Optional."),
        util::arg(&WriteArgs::raw_path, "file_path",
                  "The absolute path to the file to write (must be absolute, not relative).")
            .required("path required (received keys: {keys})"),
        // check_write_args rescues a body sent under another key.
        util::arg(&WriteArgs::content, "content", "The content to write to the file.")
            .advertise_required(),
    };
    s.raw(&WriteArgs::raw);
    return s;
}();

ExecResult run_write(const Call& call, const WriteArgs& a) {
    const auto& p = a.path->path();
    constexpr std::size_t kMaxWriteBytes = 5u * 1024u * 1024u;
    if (a.content.size() > kMaxWriteBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "write body is {} KiB (> 5 MiB cap). Split into multiple writes "
            "or stage the file via bash (cat > file <<EOF).",
            a.content.size() / 1024)));
    }
    std::string original;
    std::error_code ec;
    bool exists = call.fs().stat(p).exists;
    if (exists && call.fs().stat(p).directory)
        return std::unexpected(ToolError::not_a_file(
            "'" + a.path->string() + "' is a directory — write needs a file path."));
    auto parent = p.parent_path();
    if (!parent.empty() && call.fs().stat(parent).exists && !call.fs().stat(parent).directory)
        return std::unexpected(ToolError::not_a_directory(
            "parent of '" + a.path->string() + "' exists but is not a directory."));
    uintmax_t original_size = 0;
    if (exists) {
        if (!call.fs().stat(p).regular)
            return std::unexpected(ToolError::not_a_file(
                "not a regular file: " + a.path->string()));
        original_size = call.fs().stat(p).size;
        if (ec) original_size = 0;
        if (original_size <= kMaxWriteBytes)
            original = util::read_file(*a.path);
    }
    std::string staleness_warning;
    if (exists && staleness(call, p) == util::StaleVerdict::Stale) {
        staleness_warning =
            "\xe2\x9a\xa0  The file has changed on disk since the last time a tool "
            "observed it this session. The write OVERWROTE those changes — "
            "if that's not what you wanted, re-read the file and rewrite "
            "with the intended merged content.\n\n";
    }
    // Overwriting keeps the file's conventions, like edit does: a CRLF file
    // stays CRLF and a BOM stays put, so a whole-file rewrite isn't a diff
    // on every line.
    std::string content_buf;
    std::string_view content = a.content;
    if (exists && !original.empty()) {
        constexpr std::string_view kBom = "\xEF\xBB\xBF";
        const auto nl = original.find('\n');
        const bool crlf = nl != std::string::npos && nl > 0 && original[nl - 1] == '\r';
        const bool bom = original.starts_with(kBom) && !content.starts_with(kBom);
        if (crlf || bom) {
            if (bom) content_buf.append(kBom);
            for (std::size_t i = 0; i < content.size(); ++i) {
                if (crlf && content[i] == '\n' && (i == 0 || content[i - 1] != '\r'))
                    content_buf.push_back('\r');
                content_buf.push_back(content[i]);
            }
            content = content_buf;
        }
    }
    if (exists && !original.empty() && original == content)
        return ToolOutput{"File already matches content — no changes written.",
                          std::nullopt};
    FileChange change;
    if (!exists || (!original.empty() && original_size <= kMaxWriteBytes))
        change = make_change(a.path->string(), original, std::string{content});
    else
        change.path = a.path->string();
    if (auto err = util::write_file(*a.path, content); !err.empty())
        return std::unexpected(ToolError::io(err));
    {
        std::error_code mt_ec;
        auto new_mtime = call.fs().stat(p).mtime;
        if (!mt_ec) {
            record_seen(call, p, new_mtime,
                                   static_cast<std::uintmax_t>(content.size()),
                                   util::cheap_content_hash(content));
        }
    }
    std::string prefix;
    if (!staleness_warning.empty()) prefix += staleness_warning;
    if (!a.display_description.empty()) prefix += a.display_description + "\n";
    auto msg = std::format("{}{} {} ({}+ {}-){}",
                           prefix, exists ? "Overwrote" : "Created",
                           a.path->string(), change.added, change.removed,
                           a.coercion_note);
    // Teaching nudge (never blocks — the write already succeeded): when a
    // model OVERWRITES a substantial existing file but only changed a small
    // fraction of its lines, `edit` would have been the better call — it
    // ships only the changed snippet instead of the whole body, can't clobber
    // an unseen concurrent change, and renders a tighter diff. Mirrors
    // bash_tool_suggestion's philosophy. Silent for new files, whole-file
    // rewrites, and small files where write is genuinely the right tool.
    if (exists && !original.empty()) {
        const std::size_t orig_lines =
            static_cast<std::size_t>(
                std::count(original.begin(), original.end(), '\n')) + 1;
        const int churn = change.added + change.removed;
        if (orig_lines >= 40 && churn > 0
            && static_cast<std::size_t>(churn) * 5 < orig_lines) {
            msg += std::format(
                "\n\ntip: this overwrote a {}-line file but only changed ~{} "
                "line(s). For a targeted change like this, `edit` (or "
                "`apply_patch`) ships just the snippet instead of the whole "
                "file, won't clobber unseen concurrent edits, and produces a "
                "cleaner diff — prefer it over `write` when the file already "
                "exists and you're changing part of it.", orig_lines, churn);
        }
    }
    return ToolOutput{std::move(msg), std::move(change)};
}

// ─────────────────────────────────────────────────────────────────────────
//  list_dir
// ─────────────────────────────────────────────────────────────────────────

struct ListDirArgs {
    std::string root = ".";
    bool recursive = false;
    int max_depth = 3;
    std::string display_description;
};

const util::ArgSpec<ListDirArgs> kListDirArgs{
    util::arg(&ListDirArgs::display_description, "display_description",
              "One-line summary shown in the UI. Optional."),
    util::arg(&ListDirArgs::root, "path", "Directory to list (default: cwd)"),
    util::arg(&ListDirArgs::recursive, "recursive", "List recursively"),
    util::arg(&ListDirArgs::max_depth, "max_depth", "Max depth for recursive listing").range(1, 16),
};

ExecResult run_list_dir(const Call& call, const ListDirArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "list_dir", bounds(call));
    if (!wp) return std::unexpected(std::move(wp.error()));
    std::error_code ec;
    if (!call.fs().stat(wp->path()).exists)
        return std::unexpected(ToolError::not_found("directory not found: " + a.root));
    if (!call.fs().stat(wp->path()).directory)
        return std::unexpected(ToolError::not_a_directory("not a directory: " + a.root));

    std::ostringstream out;
    int count = 0;
    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[32];
        const double b = static_cast<double>(bytes);
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", b/1024.0); return buf; }
        if (bytes < 1024*1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fM", b/(1024.0*1024.0)); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fG", b/(1024.0*1024.0*1024.0)); return buf;
    };
    auto list_entry = [&](const DirEntry& entry, int depth) {
        if (count > 1000) return;
        std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        auto fn = entry.path.filename().string();
        if (entry.directory) {
            out << indent << fn << "/\n";
        } else if (entry.regular) {
            out << indent << fn << "  " << format_size(entry.size) << "\n";
        } else if (entry.symlink) {
            std::error_code link_ec;
            auto target = call.fs().read_symlink(entry.path, link_ec);
            if (link_ec)
                out << indent << fn << " -> <unreadable: " << link_ec.message() << ">\n";
            else
                out << indent << fn << " -> " << target.string() << "\n";
        }
        count++;
    };
    if (a.recursive) {
        call.fs().walk(wp->path(), [&](const DirEntry& entry) {
            if (entry.depth > a.max_depth) return WalkStep::skip_dir;
            auto fn = entry.path.filename().string();
            if (entry.directory && util::should_skip_dir(fn)) {
                list_entry(entry, entry.depth);
                if (count > 1000) { out << "[>1000 entries, truncated]\n"; return WalkStep::stop; }
                return WalkStep::skip_dir;
            }
            if (entry.directory && entry.depth > 0 && fn.starts_with("."))
                return WalkStep::skip_dir;
            list_entry(entry, entry.depth);
            if (count > 1000) { out << "[>1000 entries, truncated]\n"; return WalkStep::stop; }
            return WalkStep::next;
        });
    } else {
        std::vector<DirEntry> entries = call.fs().list(wp->path(), ec);
        std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) {
            bool da = x.directory, db = y.directory;
            if (da != db) return da > db;
            return x.path.filename() < y.path.filename();
        });
        for (auto& e : entries) list_entry(e, 0);
        // list_entry stops appending AND counting past the cap — signal
        // truncation without claiming an exact remainder.
        if (count > 1000)
            out << "[>1000 entries, truncated — use glob with a narrower "
                   "pattern]\n";
    }
    if (count == 0) return ToolOutput{"empty directory", std::nullopt};
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ── Schemas / descriptions ─────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
//  safe move / remove
// ─────────────────────────────────────────────────────────────────────────

struct MoveArgs { std::string raw_source; std::string raw_destination; bool overwrite = false;
                  fs::path source; fs::path destination; };

const util::ArgSpec<MoveArgs> kMoveArgs{
    util::arg(&MoveArgs::raw_source, "source", "Workspace-relative or absolute source path.").required(),
    util::arg(&MoveArgs::raw_destination, "destination",
              "Workspace-relative or absolute destination path.").required(),
    util::arg(&MoveArgs::overwrite, "overwrite", "Replace an existing destination."),
};

std::expected<MoveArgs, ToolError> check_move_args(MoveArgs a, const util::Bounds& b) {
    auto src = util::make_workspace_path_checked(a.raw_source, "move", b);
    if (!src) return std::unexpected(src.error());
    auto dst = util::make_workspace_path_checked(a.raw_destination, "move", b);
    if (!dst) return std::unexpected(dst.error());
    a.source      = src->path();
    a.destination = dst->path();
    return a;
}

ExecResult run_move(const Call& call, const MoveArgs& a) {
    std::error_code ec;
    const auto src_st = call.fs().stat(a.source);
    if (!src_st.exists)
        return std::unexpected(ToolError::not_found(a.source.string()));
    // Compare resolved paths: "f" and "./f", or a link and its target, are
    // the same file, and the overwrite branch below would delete it first.
    const auto key = [&](const fs::path& p) {
        std::error_code e;
        auto c = call.fs().canonical(p, e);
        if (e || c.empty()) c = p.lexically_normal();
        return c.has_filename() ? c : c.parent_path();
    };
    const auto src_key = key(a.source);
    const auto dst_key = key(a.destination);
    if (src_key == dst_key)
        return std::unexpected(ToolError::invalid_args(
            "source and destination are the same path; nothing to move"));
    // A directory moved under itself fails rename, and the copy fallback then
    // recurses into its own output until the path is too long.
    if (src_st.directory) {
        auto s = src_key.begin(), d = dst_key.begin();
        for (; s != src_key.end() && d != dst_key.end() && *s == *d; ++s, ++d) {}
        if (s == src_key.end())
            return std::unexpected(ToolError::invalid_args(
                "cannot move a directory into itself: " + a.destination.string()));
    }
    const auto dst_st = call.fs().stat(a.destination);
    if (dst_st.exists) {
        if (!a.overwrite)
            return std::unexpected(ToolError::ambiguous(
                "destination exists; pass overwrite:true to replace it"));
        if (dst_st.directory && !call.fs().list(a.destination, ec).empty())
            return std::unexpected(ToolError::invalid_args(
                "refusing to replace a non-empty destination directory"));
        call.fs().remove(a.destination, ec);
        if (ec) return std::unexpected(ToolError::io(ec.message()));
    }
    call.fs().create_directories(a.destination.parent_path(), ec);
    if (ec) return std::unexpected(ToolError::io(ec.message()));
    call.fs().rename(a.source, a.destination, ec);
    if (ec) {
        // Cross-filesystem/device rename (EXDEV: different volume, tmpfs,
        // bind mount) can't be a rename(2). Fall back to copy+delete — the
        // semantics the caller asked for — instead of surfacing a cryptic
        // "Invalid cross-device link".
        std::error_code copy_ec;
        call.fs().copy(a.source, a.destination, copy_ec);
        if (copy_ec)
            return std::unexpected(ToolError::io(
                "move failed: " + ec.message()
                + "; copy fallback also failed: " + copy_ec.message()));
        call.fs().remove_all(a.source, copy_ec);
        if (copy_ec)
            return std::unexpected(ToolError::io(
                "moved (copied) to " + a.destination.string()
                + " but could not remove the source: " + copy_ec.message()));
    }
    return ToolOutput{std::format("Moved {} -> {}", a.source.string(), a.destination.string()), std::nullopt};
}

struct RemoveArgs { std::string raw; bool recursive = false; fs::path path; };

const util::ArgSpec<RemoveArgs> kRemoveArgs{
    util::arg(&RemoveArgs::raw, "path", "Workspace path to remove.").required(),
    util::arg(&RemoveArgs::recursive, "recursive", "Required for non-empty directories."),
};

std::expected<RemoveArgs, ToolError> check_remove_args(RemoveArgs a, const util::Bounds& b) {
    auto checked = util::make_workspace_path_checked(a.raw, "remove", b);
    if (!checked) return std::unexpected(checked.error());
    // Compare resolved paths: ".", "./", "d/.." and "<root>/" all name the
    // workspace root, and a raw == let them through to remove_all.
    std::error_code ec;
    const auto resolve = [&](const fs::path& p) {
        std::error_code e;
        auto c = b.files->canonical(p, e);
        if (e || c.empty()) c = p.lexically_normal();
        return c.has_filename() ? c : c.parent_path();
    };
    const auto target = resolve(checked->path());
    if (target == resolve(b.workspace) || (!b.project.empty() && target == resolve(b.project)))
        return std::unexpected(ToolError::invalid_args("refusing to remove the workspace root"));
    a.path = checked->path();
    return a;
}

ExecResult run_remove(const Call& call, const RemoveArgs& a) {
    std::error_code ec;
    // Already gone = the desired state, so SUCCESS (like `rm -f`). It was a
    // red [not found] failure, which a weak model (#91: gpt-oss) read as
    // "the job is blocked" and gave up on the whole task instead of moving
    // on to writing the new files. A wrong path still shows: the output says
    // nothing was there, so a model that meant something else can check.
    if (!call.fs().stat(a.path).exists)
        return ToolOutput{"Nothing to remove: " + a.path.string()
                          + " does not exist (already gone). Desired state met "
                            "\xe2\x80\x94 continue with the next step.",
                          std::nullopt};
    const bool directory = call.fs().stat(a.path).directory;
    if (directory && !a.recursive && !call.fs().list(a.path, ec).empty())
        return std::unexpected(ToolError::invalid_args(
            "directory is not empty; pass recursive:true to remove its contents"));
    const auto count = a.recursive ? call.fs().remove_all(a.path, ec)
                                   : static_cast<std::uintmax_t>(call.fs().remove(a.path, ec));
    if (ec) return std::unexpected(ToolError::io("remove failed: " + ec.message()));
    return ToolOutput{std::format("Removed {} ({} filesystem entr{})", a.path.string(), count,
                                  count == 1 ? "y" : "ies"), std::nullopt};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  outline — a nested symbol tree for ONE file (no bodies).
// ─────────────────────────────────────────────────────────────────────────
// `read` emits a FLAT outline as a side effect for files >32 KiB; this is the
// first-class, intentional "show me this file's SHAPE" tool: works at any
// size, and NESTS entries by leading-indent depth so a class's methods sit
// under it. Reuses the same def/heading pattern read's outline uses, so what
// you see here is exactly what read would surface — just structured, with a
// line number you can jump to via read(symbol=) or read(offset=).

struct OutlineArgs {
    std::string raw_path;
    int  max_entries = 400;
    std::string display_description;
    std::optional<util::WorkspacePath> path;
};


ExecResult run_outline(const Call& call, const OutlineArgs& a) {
    const auto& p = a.path->path();
    std::error_code ec;
    if (!call.fs().stat(p).exists)
        return std::unexpected(ToolError::not_found(a.path->string()));
    if (!call.fs().stat(p).regular)
        return std::unexpected(ToolError::not_a_file(a.path->string()));
    constexpr std::uintmax_t kMaxBytes = 8u * 1024u * 1024u;
    if (auto sz = call.fs().stat(p).size; !ec && sz > kMaxBytes)
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 8 MiB outline cap).", sz / 1024)));

    std::string content;
    try { content = util::read_file(*a.path); }
    catch (...) { return std::unexpected(ToolError::io("cannot read " + a.path->string())); }
    if (content.find('\0') != std::string::npos)
        return std::unexpected(ToolError::binary(a.path->string()));

    int total = 0;
    std::string tree = outline::render_nested(
        content, static_cast<std::size_t>(a.max_entries), total);

    int lines = static_cast<int>(std::count(content.begin(), content.end(), '\n')) + 1;
    std::ostringstream out;
    if (!a.display_description.empty()) out << a.display_description << "\n";
    if (total == 0)
        return ToolOutput{a.path->string() + " (" + std::to_string(lines)
            + " lines): no top-level definitions or headings detected — "
            "read it directly.", std::nullopt};
    out << a.path->string() << " — " << total << " definition"
        << (total==1?"":"s") << " across " << lines << " lines:\n\n" << tree;
    if (total > a.max_entries)
        out << "\n[" << (total - a.max_entries)
            << " more — raise max_entries]";
    return ToolOutput{util::to_valid_utf8(out.str()), std::nullopt};
}

std::expected<OutlineArgs, ToolError> check_outline_args(OutlineArgs a, const util::Bounds& b) {
    auto wp = util::make_readable_path_checked(a.raw_path, "outline", b);
    if (!wp) return std::unexpected(std::move(wp.error()));
    a.path = std::move(*wp);
    return a;
}

const util::ArgSpec<OutlineArgs> kOutlineArgs = [] {
    util::ArgSpec<OutlineArgs> s{
        util::arg(&OutlineArgs::raw_path, "path", "File to outline.").required("`path` is required"),
        util::arg(&OutlineArgs::display_description, "display_description",
                  "One-line summary shown in the UI. Optional."),
        util::arg(&OutlineArgs::max_entries, "max_entries", "Max symbols shown (default 400).")
            .range(1, 2000),
    };
    return s;
}();

void register_fs_tools(Shells& sh) {
    sh.add("read",
        "Read a file from the filesystem. A plain read (no line range) "
        "returns up to 2000 lines from the top; but when you pass a START "
        "position (offset / start_line) WITHOUT an end, it returns a "
        "FOCUSED ~250-line window (not the whole rest of the file) and the "
        "footer tells you how many lines remain + the exact offset to page "
        "\xe2\x80\x94 so paging stays cheap. For files over 32 KiB, "
        "reading without an explicit line range returns a SYMBOL "
        "OUTLINE (function / class / heading names with line "
        "numbers) \xe2\x80\x94 or, if the file has no code "
        "structure, its first 1 KiB \xe2\x80\x94 instead of the "
        "full content; use start_line + end_line (or offset + "
        "limit) on a follow-up read to fetch the specific section "
        "you want. Best of all, pass `symbol` to read exactly one "
        "function/class/type's definition and body (resolved to its "
        "enclosing block) with no line arithmetic — use that instead "
        "of computing ranges or shelling out to sed/head/tail. Include "
        "a brief `display_description` so the user sees why you're reading.",
        kReadArgs.schema(), EffectSet{Effect::ReadFs},
        body<ReadArgs>(run_read, kReadArgs, check_read_args), 80'000);

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
        kWriteArgs.schema(), EffectSet{Effect::WriteFs},
        body<WriteArgs>(run_write, kWriteArgs, check_write_args), 40'000);

    sh.add("list_dir",
        "List the contents of a directory. Shows file type, size, and name.",
        kListDirArgs.schema(), EffectSet{Effect::ReadFs},
        body<ListDirArgs>(run_list_dir, kListDirArgs), 25'000);

    sh.add("outline",
        "Show a file's SHAPE \xe2\x80\x94 a nested tree of its definitions "
        "(functions / classes / structs / headings) with line numbers, and "
        "NO bodies. The cheap \"what's in this file\" probe before reading: "
        "one call maps a 2000-line source at a fraction of the context cost, "
        "then jump to a symbol with read(symbol=) or read(offset=). Unlike "
        "read's size-triggered outline, this works at any size and nests "
        "methods under their class by indent depth.",
        kOutlineArgs.schema(), EffectSet{Effect::ReadFs},
        body<OutlineArgs>(run_outline, kOutlineArgs, check_outline_args), 25'000);

    sh.add("move",
        "Move or rename a file or directory within the workspace without invoking a shell. "
        "The destination is never overwritten unless overwrite:true is explicit.",
        kMoveArgs.schema(), EffectSet{Effect::ReadFs, Effect::WriteFs},
        body<MoveArgs>(run_move, kMoveArgs, check_move_args), 4'000);

    sh.add("remove",
        "Remove a file or directory within the workspace without invoking a shell. "
        "Non-empty directories require recursive:true; the workspace root is always refused.",
        kRemoveArgs.schema(), EffectSet{Effect::ReadFs, Effect::WriteFs},
        body<RemoveArgs>(run_remove, kRemoveArgs, check_remove_args), 4'000);

    // edit (own TU — fuzzy splice logic is large)
    register_edit_tool(sh);
}

} // namespace mcp::tools::detail
