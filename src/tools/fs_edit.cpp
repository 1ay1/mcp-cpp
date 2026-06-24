// SPDX-License-Identifier: Apache-2.0
//
// fs_edit.cpp — register_edit_tool: the `edit` tool (fuzzy line-level splice).
// Faithful port of agentty's src/tool/tools/edit.cpp. The inline unified diff
// + FileChange use the internal diff engine (diff.hpp). FileChange is carried
// to the host's diff-review UI via the detail::lower() meta bridge.

#include "tool_shell.hpp"
#include "tool_body.hpp"
#include "diff.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/fuzzy_match.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

inline bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

std::string minimal_splice(const std::string& buf,
                           std::size_t region_pos,
                           std::size_t region_len,
                           std::string_view repl) {
    std::string_view matched{buf.data() + region_pos, region_len};
    std::size_t pre = 0;
    const std::size_t maxpre = std::min(matched.size(), repl.size());
    while (pre < maxpre && matched[pre] == repl[pre]) ++pre;
    while (pre > 0 && is_utf8_cont(static_cast<unsigned char>(
               pre < matched.size() ? matched[pre] : '\0')))
        --pre;
    std::size_t suf = 0;
    const std::size_t maxsuf = std::min(matched.size() - pre, repl.size() - pre);
    while (suf < maxsuf
           && matched[matched.size() - 1 - suf] == repl[repl.size() - 1 - suf])
        ++suf;
    while (suf > 0
           && is_utf8_cont(static_cast<unsigned char>(matched[matched.size() - suf])))
        --suf;
    if (pre + suf >= matched.size() && pre + suf >= repl.size())
        return buf;
    if (pre == 0 && suf == 0) {
        std::string out;
        out.reserve(buf.size() - region_len + repl.size());
        out.append(buf, 0, region_pos);
        out.append(repl);
        out.append(buf, region_pos + region_len, std::string::npos);
        return out;
    }
    std::string_view repl_mid{repl.data() + pre, repl.size() - pre - suf};
    const std::size_t cut_begin = region_pos + pre;
    const std::size_t cut_end   = region_pos + region_len - suf;
    std::string out;
    out.reserve(buf.size() - (cut_end - cut_begin) + repl_mid.size());
    out.append(buf, 0, cut_begin);
    out.append(repl_mid);
    out.append(buf, cut_end, std::string::npos);
    return out;
}

struct OneEdit {
    std::string old_text;
    std::string new_text;
    bool        replace_all = false;
    int         expected_replacements = 0;
    std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
};

struct EditArgs {
    util::WorkspacePath   path;
    std::vector<OneEdit>  edits;
    std::string           display_description;
};

std::expected<EditArgs, ToolError> parse_edit_args(const json& j) {
    util::ArgReader ar(j);
    if (!ar.is_object())
        return std::unexpected(ToolError::invalid_args("args must be an object"));
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    auto wp = util::make_workspace_path_checked(*path_opt, "edit");
    if (!wp) return std::unexpected(std::move(wp.error()));

    std::string desc = ar.str("display_description", "");
    std::vector<OneEdit> edits;

    if (auto raw = ar.raw("edits"); raw && raw->is_array()) {
        edits.reserve(raw->size());
        int idx = 0;
        for (const auto& e : *raw) {
            ++idx;
            if (!e.is_object())
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: expected object", idx - 1)));
            util::ArgReader sub(e);
            auto old_opt = sub.require_str("old_text");
            if (!old_opt) old_opt = sub.require_str("old_string");
            if (!old_opt)
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: old_text required", idx - 1)));
            std::string new_text = sub.str("new_text", "");
            if (new_text.empty() && sub.has("new_string"))
                new_text = sub.str("new_string", "");
            int expected = sub.integer("expected_replacements", 0);
            if (expected == 0) expected = sub.integer("count", 0);
            bool replace_all = sub.boolean("replace_all", false);
            if (expected >= 2) replace_all = true;
            std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
            int ln = sub.integer("line", 0);
            if (ln <= 0) ln = sub.integer("line_hint", 0);
            if (ln <= 0) ln = sub.integer("at_line", 0);
            if (ln > 0) line_hint = static_cast<std::uint32_t>(ln - 1);
            edits.push_back(OneEdit{std::move(*old_opt), std::move(new_text),
                                    replace_all, expected, line_hint});
        }
    } else {
        auto old_opt = ar.require_str("old_string");
        if (!old_opt) old_opt = ar.require_str("old_text");
        if (!old_opt)
            return std::unexpected(ToolError::invalid_args(
                "no edits provided — pass either `edits: [...]` or "
                "`old_string`/`new_string` at top level"));
        std::string new_s = ar.str("new_string", "");
        if (new_s.empty() && ar.has("new_text"))
            new_s = ar.str("new_text", "");
        int expected = ar.integer("expected_replacements", 0);
        if (expected == 0) expected = ar.integer("count", 0);
        bool replace_all = ar.boolean("replace_all", false);
        if (expected >= 2) replace_all = true;
        std::uint32_t line_hint = std::numeric_limits<std::uint32_t>::max();
        int ln = ar.integer("line", 0);
        if (ln <= 0) ln = ar.integer("line_hint", 0);
        if (ln <= 0) ln = ar.integer("at_line", 0);
        if (ln > 0) line_hint = static_cast<std::uint32_t>(ln - 1);
        edits.push_back(OneEdit{std::move(*old_opt), std::move(new_s),
                                replace_all, expected, line_hint});
    }

    if (edits.empty())
        return std::unexpected(ToolError::invalid_args(
            "edits array is empty — nothing to change"));
    for (std::size_t i = 0; i < edits.size(); ++i) {
        if (edits[i].old_text.empty())
            return std::unexpected(ToolError::invalid_args(
                std::format("edits[{}]: old_text cannot be empty", i)));
    }
    return EditArgs{std::move(*wp), std::move(edits), std::move(desc)};
}

int line_of_offset(std::string_view s, std::size_t off) noexcept {
    int line = 1;
    auto cap = std::min(off, s.size());
    for (std::size_t i = 0; i < cap; ++i) if (s[i] == '\n') ++line;
    return line;
}

std::vector<int> hit_lines(std::string_view buf, std::string_view needle,
                           std::size_t cap = 5) {
    std::vector<int> out;
    if (needle.empty()) return out;
    std::size_t p = 0;
    while (out.size() < cap) {
        auto pos = buf.find(needle, p);
        if (pos == std::string_view::npos) break;
        out.push_back(line_of_offset(buf, pos));
        p = pos + needle.size();
    }
    return out;
}

std::string join_ints(const std::vector<int>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(v[i]);
    }
    return out;
}

int closest_line_hint(std::string_view buf, std::string_view needle) noexcept {
    std::string_view probe;
    std::size_t i = 0;
    while (i < needle.size()) {
        std::size_t s = i;
        while (i < needle.size() && needle[i] != '\n') ++i;
        std::size_t e = i;
        if (i < needle.size()) ++i;
        while (s < e && (needle[s] == ' ' || needle[s] == '\t' || needle[s] == '\r')) ++s;
        while (e > s && (needle[e-1] == ' ' || needle[e-1] == '\t' || needle[e-1] == '\r')) --e;
        if (e > s) { probe = needle.substr(s, e - s); break; }
    }
    if (probe.size() < 4) return 0;
    int best_ln = 0;
    std::size_t best_score = 0;
    std::size_t pos = 0;
    int ln = 1;
    while (pos < buf.size()) {
        std::size_t le = pos;
        while (le < buf.size() && buf[le] != '\n') ++le;
        std::string_view line = buf.substr(pos, le - pos);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        std::size_t score = 0;
        if (!line.empty()) {
            for (std::size_t plen = probe.size(); plen >= 4; --plen) {
                if (line.find(probe.substr(0, plen)) != std::string_view::npos) {
                    score = plen; break;
                }
            }
        }
        if (score > best_score) { best_score = score; best_ln = ln; }
        pos = (le < buf.size()) ? le + 1 : le;
        ++ln;
    }
    return (best_score * 2 >= probe.size()) ? best_ln : 0;
}

std::string render_context_window(std::string_view buf, int line_1based, int radius = 2) {
    if (line_1based <= 0) return {};
    int lo = std::max(1, line_1based - radius);
    int hi = line_1based + radius;
    std::string out;
    int ln = 1;
    std::size_t pos = 0;
    while (pos < buf.size() && ln <= hi) {
        std::size_t le = pos;
        while (le < buf.size() && buf[le] != '\n') ++le;
        if (ln >= lo) {
            char prefix[16];
            std::snprintf(prefix, sizeof(prefix), "%4d %s ",
                          ln, ln == line_1based ? ">" : " ");
            out += prefix;
            out.append(buf.data() + pos, le - pos);
            out.push_back('\n');
        }
        pos = (le < buf.size()) ? le + 1 : le;
        ++ln;
    }
    return out;
}

struct CandidateRegion { int start_line; int end_line; double score; };

double score_region(const std::vector<std::string_view>& file_lines,
                    std::size_t file_line_idx,
                    const std::vector<std::string_view>& needle_lines) {
    if (needle_lines.empty()) return 0.0;
    std::size_t matched = 0;
    std::size_t fi = file_line_idx;
    for (const auto& nl : needle_lines) {
        if (fi >= file_lines.size()) break;
        const auto& fl = file_lines[fi];
        if (nl == fl) {
            ++matched;
        } else {
            auto shorter = (nl.size() < fl.size()) ? nl : fl;
            auto longer  = (nl.size() < fl.size()) ? fl : nl;
            if (!shorter.empty() && longer.find(shorter) != std::string_view::npos) {
                matched += 1;
            } else if (shorter.size() >= 4) {
                std::size_t overlap = 0;
                std::size_t check = std::min(shorter.size(), longer.size());
                for (std::size_t i = 0; i < check && shorter[i] == longer[i]; ++i)
                    ++overlap;
                if (overlap * 2 >= shorter.size()) matched += 1;
            }
        }
        ++fi;
    }
    return static_cast<double>(matched) / static_cast<double>(needle_lines.size());
}

std::string_view trim_line(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split_trimmed_lines(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = pos;
        while (end < s.size() && s[end] != '\n') ++end;
        auto line = s.substr(pos, end - pos);
        out.push_back(trim_line(line));
        pos = (end < s.size()) ? end + 1 : end;
    }
    return out;
}

std::optional<CandidateRegion> find_best_candidate(std::string_view buf,
                                                    std::string_view needle) {
    auto file_lines = split_trimmed_lines(buf);
    auto needle_lines = split_trimmed_lines(needle);
    if (needle_lines.empty() || file_lines.empty()) return std::nullopt;
    std::size_t first_nonblank = 0;
    while (first_nonblank < needle_lines.size() && needle_lines[first_nonblank].empty())
        ++first_nonblank;
    if (first_nonblank >= needle_lines.size()) return std::nullopt;
    const auto& anchor = needle_lines[first_nonblank];
    if (anchor.size() < 4) return std::nullopt;
    CandidateRegion best{0, 0, 0.0};
    for (std::size_t i = 0; i < file_lines.size(); ++i) {
        const auto& fl = file_lines[i];
        bool might_match = false;
        if (fl == anchor) {
            might_match = true;
        } else if (anchor.size() >= 8) {
            auto prefix = anchor.substr(0, std::min(anchor.size(), std::size_t{8}));
            auto suffix = anchor.substr(anchor.size() > 8 ? anchor.size() - 8 : 0);
            might_match = (fl.find(prefix) != std::string_view::npos ||
                           fl.find(suffix) != std::string_view::npos);
        } else {
            might_match = (fl.find(anchor) != std::string_view::npos);
        }
        if (!might_match) continue;
        std::size_t start_idx = (i >= first_nonblank) ? i - first_nonblank : 0;
        double score = score_region(file_lines, start_idx, needle_lines);
        if (score > best.score) {
            best.start_line = static_cast<int>(start_idx) + 1;
            best.end_line = static_cast<int>(
                std::min(start_idx + needle_lines.size(), file_lines.size()));
            best.score = score;
        }
    }
    if (best.score < 0.3) return std::nullopt;
    return best;
}

std::string render_did_you_mean(std::string_view buf, std::string_view needle,
                                 const std::string& path_str) {
    auto candidate = find_best_candidate(buf, needle);
    if (!candidate) return {};
    std::string out;
    out += std::format(
        "\n\nDid you mean to match some of these actual lines from {}?\n\n", path_str);
    out += "```\n";
    int ln = 1;
    std::size_t pos = 0;
    while (pos < buf.size()) {
        std::size_t le = pos;
        while (le < buf.size() && buf[le] != '\n') ++le;
        if (ln >= candidate->start_line && ln <= candidate->end_line) {
            out.append(buf.data() + pos, le - pos);
            out.push_back('\n');
        }
        pos = (le < buf.size()) ? le + 1 : le;
        ++ln;
        if (ln > candidate->end_line) break;
    }
    out += "```\n\n";
    out += "The old_text must exactly match an existing block of lines ";
    out += "including all whitespace, comments, and indentation.";
    return out;
}

constexpr int kIdempotentNoOp = -1;

int apply_one(std::string& buf, const OneEdit& e,
              const std::string& path_str, std::string& err) {
    auto replace_exact = [&](std::string_view needle,
                             std::string_view replacement) -> int {
        std::string out;
        out.reserve(buf.size());
        std::size_t cursor = 0;
        int n = 0;
        for (;;) {
            auto pos = buf.find(needle, cursor);
            if (pos == std::string::npos) break;
            out.append(buf, cursor, pos - cursor);
            out.append(replacement);
            cursor = pos + needle.size();
            ++n;
        }
        if (n == 0) return 0;
        out.append(buf, cursor, std::string::npos);
        buf = std::move(out);
        return n;
    };
    auto without_cr = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) if (c != '\r') out.push_back(c);
        return out;
    };
    auto with_crlf = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + s.size() / 40);
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\n' && (i == 0 || s[i-1] != '\r'))
                out.push_back('\r');
            out.push_back(c);
        }
        return out;
    };

    if (!e.new_text.empty()
        && e.new_text.size() >= 8
        && e.new_text != e.old_text
        && buf.find(e.new_text) != std::string::npos
        && buf.find(e.old_text) == std::string::npos)
    {
        err.clear();
        return kIdempotentNoOp;
    }

    if (e.replace_all) {
        int n = replace_exact(e.old_text, e.new_text);
        if (n > 0) return n;
        if (buf.find('\r') != std::string::npos
            || e.old_text.find('\r') != std::string::npos) {
            std::string alt_needle = with_crlf(without_cr(e.old_text));
            std::string alt_new    = with_crlf(without_cr(e.new_text));
            if (alt_needle != e.old_text) {
                n = replace_exact(alt_needle, alt_new);
                if (n > 0) return n;
            }
            std::string plain_needle = without_cr(e.old_text);
            std::string plain_new    = without_cr(e.new_text);
            if (plain_needle != e.old_text) {
                n = replace_exact(plain_needle, plain_new);
                if (n > 0) return n;
            }
        }
        auto fm = util::fuzzy_find(buf, e.old_text, e.new_text, e.line_hint);
        if (fm.ok) {
            std::string_view repl = fm.adjusted_new_text.empty()
                ? std::string_view{e.new_text}
                : std::string_view{fm.adjusted_new_text};
            buf = minimal_splice(buf, fm.pos, fm.len, repl);
            return 1;
        }
        if (int ln = closest_line_hint(buf, e.old_text); ln > 0) {
            auto window = render_context_window(buf, ln, 2);
            err = std::format(
                "old_text not found in {} (replace_all tried exact, CRLF, "
                "and fuzzy strategies). The closest matching line is around "
                "line {} — here are the actual bytes at that location:\n"
                "```\n{}```\n"
                "Copy the snippet byte-for-byte from above and retry.",
                path_str, ln, window);
        }
        else
            err = "old_text not found in " + path_str
                + " (replace_all tried exact, CRLF, and fuzzy strategies). "
                  "Re-read the file and copy the snippet byte-for-byte.";
        return 0;
    }

    if (!e.replace_all
        && (buf.find('\r') != std::string::npos
            || e.old_text.find('\r') != std::string::npos))
    {
        std::string plain_needle = without_cr(e.old_text);
        std::string plain_new    = without_cr(e.new_text);
        if (plain_needle != e.old_text) {
            std::string plain_buf = without_cr(buf);
            std::size_t first = plain_buf.find(plain_needle);
            if (first != std::string::npos) {
                std::size_t next = plain_buf.find(plain_needle, first + plain_needle.size());
                if (next == std::string::npos) {
                    std::string crlf_needle = with_crlf(plain_needle);
                    std::string crlf_new    = with_crlf(plain_new);
                    int n = replace_exact(crlf_needle, crlf_new);
                    if (n == 1) return 1;
                    n = replace_exact(plain_needle, plain_new);
                    if (n == 1) return 1;
                }
            }
        }
    }

    auto m = util::fuzzy_find(buf, e.old_text, e.new_text, e.line_hint);
    if (!m.ok) {
        if (m.count >= 2) {
            auto lines = hit_lines(buf, e.old_text, 5);
            std::string at;
            if (!lines.empty()) {
                at = " Matches at line";
                if (lines.size() > 1) at += "s";
                at += " " + join_ints(lines);
                if (m.count > static_cast<int>(lines.size()))
                    at += std::format(" (and {} more)", m.count - static_cast<int>(lines.size()));
                at += ".";
            }
            err = std::format(
                "old_text appears {} times in {}.{} Add surrounding context "
                "to make it unique, or pass replace_all=true.",
                m.count, path_str, at);
        } else {
            std::string hint;
            std::string squashed_old, squashed_buf;
            squashed_old.reserve(e.old_text.size());
            squashed_buf.reserve(buf.size());
            for (char c : e.old_text)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_old += c;
            for (char c : buf)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_buf += c;
            if (!squashed_old.empty()
                && squashed_buf.find(squashed_old) != std::string::npos) {
                auto sq_pos = squashed_buf.find(squashed_old);
                std::size_t consumed = 0, byte_pos = 0;
                for (; byte_pos < buf.size() && consumed < sq_pos; ++byte_pos) {
                    char c = buf[byte_pos];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                        ++consumed;
                }
                int ln = line_of_offset(buf, byte_pos);
                hint = std::format(
                    " The text appears around line {} but differs in "
                    "whitespace/punctuation in a way fuzzy matching "
                    "couldn't reconcile — re-read the file at that line "
                    "and copy the snippet byte-for-byte.", ln);
            }
            if (hint.empty())
                hint = render_did_you_mean(buf, e.old_text, path_str);
            if (hint.empty()) {
                if (int ln = closest_line_hint(buf, e.old_text); ln > 0) {
                    auto window = render_context_window(buf, ln, 2);
                    hint = std::format(
                        " The closest matching line is around line {} — "
                        "here are the actual bytes at that location:\n"
                        "```\n{}```\n"
                        "Copy the snippet byte-for-byte from above and retry.",
                        ln, window);
                }
            }
            err = "old_text not found in " + path_str + "." + hint;
        }
        return 0;
    }
    std::string_view repl =
        m.adjusted_new_text.empty()
            ? std::string_view{e.new_text}
            : std::string_view{m.adjusted_new_text};
    buf = minimal_splice(buf, m.pos, m.len, repl);
    return 1;
}

ExecResult run_edit(const EditArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        auto parent = p.parent_path();
        bool parent_ok = !parent.empty() && fs::exists(parent, ec);
        if (!parent_ok)
            return std::unexpected(ToolError::not_found(
                "file not found: " + a.path.string()
                + " (parent directory doesn't exist either). "
                  "Re-check the path — try `list_dir` on a directory you "
                  "know exists, or `glob` by filename."));
        return std::unexpected(ToolError::not_found(
            "file not found: " + a.path.string()
            + ". To create a new file use the `write` tool; "
              "`edit` only modifies existing files."));
    }
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file(
            "not a regular file: " + a.path.string()
            + " (is it a directory or symlink to one?)"));
    if (util::is_binary_file(p))
        return std::unexpected(ToolError::binary(
            "refusing to edit binary file: " + a.path.string()
            + " (contains NUL bytes — likely an image, archive, or compiled "
              "artifact). If this is a text file with a stray NUL, use "
              "`write` to rewrite it whole."));

    std::string staleness_warning;
    if (util::staleness_of(p) == util::StaleVerdict::Stale) {
        staleness_warning =
            "\xe2\x9a\xa0  The file has changed on disk since the last time a tool "
            "observed it this session. The edit was applied to the CURRENT "
            "bytes; if the result looks wrong, re-read the file before "
            "making further edits.\n\n";
    }

    std::string original = util::read_file(a.path);
    std::string updated  = original;

    struct EditOutcome {
        std::size_t index;
        int  applied;
        bool exact_count_mismatch;
        std::string err;
    };
    std::vector<EditOutcome> outcomes;
    outcomes.reserve(a.edits.size());
    std::size_t skipped_noop = 0;
    std::size_t applied = 0;
    std::size_t idempotent = 0;
    for (std::size_t i = 0; i < a.edits.size(); ++i) {
        const auto& ed = a.edits[i];
        if (ed.old_text == ed.new_text) { ++skipped_noop; continue; }
        std::string err;
        int n = apply_one(updated, ed, a.path.string(), err);
        if (n == kIdempotentNoOp) {
            ++idempotent;
            outcomes.push_back({i, kIdempotentNoOp, false, {}});
            continue;
        }
        bool count_mismatch =
            (n > 0 && ed.expected_replacements > 0 && n != ed.expected_replacements);
        if (count_mismatch) {
            updated = original;
            for (const auto& prior : outcomes) {
                if (prior.applied <= 0) continue;
                std::string tmp_err;
                (void)apply_one(updated, a.edits[prior.index], a.path.string(), tmp_err);
            }
            outcomes.push_back({i, n, true, std::format(
                "expected_replacements={} but matched {} occurrence(s)",
                ed.expected_replacements, n)});
            continue;
        }
        if (n == 0) {
            bool was_in_original = !original.empty()
                && original.find(ed.old_text) != std::string::npos;
            if (was_in_original && applied > 0) {
                err = std::format(
                    "{} The text WAS present in the file before this batch, "
                    "but {} earlier edit(s) altered the region. Re-order "
                    "this edit before the conflicting ones, OR merge them "
                    "into a single larger old_text.",
                    err, applied);
            }
            outcomes.push_back({i, 0, false, std::move(err)});
            continue;
        }
        outcomes.push_back({i, n, false, {}});
        ++applied;
    }

    std::size_t failed = 0;
    for (const auto& o : outcomes) if (o.applied == 0) ++failed;

    if (applied == 0 && skipped_noop == a.edits.size()) {
        return ToolOutput{std::format(
            "{}No edits were applied — all {} edit(s) had identical "
            "old_text and new_text (nothing to change). File on disk is "
            "unchanged.", staleness_warning, a.edits.size()), std::nullopt};
    }
    if (applied == 0 && idempotent > 0 && failed == 0) {
        return ToolOutput{std::format(
            "{}No write needed — all {} edit(s) were already present in "
            "the file (new_text matched the on-disk bytes, old_text was "
            "absent). The desired state is already in place; move on.",
            staleness_warning, idempotent), std::nullopt};
    }
    if (applied == 0 && failed > 0) {
        std::string msg;
        if (a.edits.size() == 1) {
            msg = outcomes.front().err;
        } else {
            msg = std::format(
                "All {} edit(s) failed; file on disk is unchanged.", a.edits.size());
            for (const auto& o : outcomes) {
                if (o.applied == 0 && !o.err.empty())
                    msg += std::format("\n  edits[{}]: {}", o.index, o.err);
            }
        }
        for (const auto& o : outcomes) {
            if (o.applied == 0 && o.err.find("appears") != std::string::npos)
                return std::unexpected(ToolError::ambiguous(std::move(msg)));
        }
        return std::unexpected(ToolError::no_match(std::move(msg)));
    }
    if (original == updated)
        return ToolOutput{staleness_warning
            + "No edits were made — all old_text / new_text pairs "
              "produced identical content (file unchanged on disk).", std::nullopt};

    auto d = diff::compute(a.path.string(), original, updated);
    if (auto werr = util::write_file(a.path, updated); !werr.empty())
        return std::unexpected(ToolError::io(werr));
    {
        std::error_code mt_ec;
        auto new_mtime = fs::last_write_time(p, mt_ec);
        if (!mt_ec) {
            util::record_file_seen(p, new_mtime,
                                   static_cast<std::uintmax_t>(updated.size()),
                                   util::content_fnv1a(updated));
        }
    }

    std::string unified = diff::render_unified(d);
    std::ostringstream msg;
    if (!staleness_warning.empty()) msg << staleness_warning;
    if (!a.display_description.empty())
        msg << a.display_description << "\n\n";
    msg << "Edited " << a.path.string() << " (" << d.added << "+ "
        << d.removed << "-";
    if (a.edits.size() > 1) {
        msg << ", " << applied << "/" << a.edits.size() << " edits applied";
        if (idempotent > 0)   msg << ", " << idempotent   << " already present";
        if (skipped_noop > 0) msg << ", " << skipped_noop << " no-op";
        if (failed > 0)       msg << ", " << failed       << " failed";
    }
    msg << "):\n\n```diff\n" << unified;
    if (unified.empty() || unified.back() != '\n') msg << "\n";
    msg << "```";

    if (a.edits.size() > 1 && (failed > 0 || idempotent > 0)) {
        msg << "\n\nPer-edit results:";
        for (const auto& o : outcomes) {
            msg << std::format("\n  edits[{}]: ", o.index);
            if (o.applied == kIdempotentNoOp)
                msg << "already present (no-op)";
            else if (o.applied > 0)
                msg << std::format("applied ({} replacement(s))", o.applied);
            else
                msg << "FAILED — " << o.err;
        }
        if (failed > 0) {
            msg << "\n\nThe successful edits have been written. To fix the "
                   "failed edits, retry ONLY those entries (don't re-emit "
                   "the ones already applied).";
        }
    }

    // Carry the FileChange (before/after + counts) to the host diff-review UI.
    FileChange change = make_change(a.path.string(), original, updated);
    return ToolOutput{msg.str(), std::move(change)};
}

json edit_schema() {
    return json{
        {"type","object"},
        {"required", {"path","edits"}},
        {"properties", {
            {"path", {{"type","string"},
                {"description","Absolute or relative path of the existing "
                               "file. Stream this FIRST."}}},
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the card while "
                               "edits stream — e.g. 'Fix null-deref in "
                               "auth.cpp'. Stream second."}}},
            {"edits", {
                {"type","array"},
                {"description","One or more edits, applied in order."},
                {"items", {
                    {"type","object"},
                    {"required", {"old_text","new_text"}},
                    {"properties", {
                        {"old_text",  {{"type","string"},
                            {"description","Snippet to find. Matched "
                                "fuzzily by line edit-distance — minor "
                                "whitespace/indent drift and single-char "
                                "typos are tolerated. Should be unique "
                                "in the file OR accompanied by `line` "
                                "to disambiguate."}}},
                        {"new_text",  {{"type","string"},
                            {"description","Replacement text. "
                                "Indentation is auto-adjusted to match "
                                "the surrounding file convention when "
                                "old_text was at a different indent "
                                "level."}}},
                        {"replace_all",{{"type","boolean"},
                            {"default", false},
                            {"description","Replace every occurrence "
                                "instead of exactly one."}}},
                        {"expected_replacements",{{"type","integer"},
                            {"description","If set, edit must match "
                                "exactly this many times or it fails. "
                                "Safer than replace_all for known-count "
                                "refactors — surfaces mismatches as "
                                "errors instead of silent over-replace."}}},
                        {"line",{{"type","integer"},
                            {"description","1-based line number anchor "
                                "for ambiguous matches. If old_text "
                                "could match multiple regions, the one "
                                "closest to this line (within ~200 "
                                "lines) wins. Optional."}}},
                    }},
                }},
            }},
        }},
    };
}

constexpr const char* kEditDescription =
    "Modify an EXISTING file by applying one or more targeted text "
    "substitutions. PREFER this tool over `write` whenever you are "
    "changing only part of a file — it streams less data and produces "
    "a reviewable diff. Pass `edits: [{old_text, new_text}, ...]`; "
    "every edit is applied in order. `old_text` is matched FUZZILY "
    "using line-level edit-distance — minor whitespace, indentation, "
    "smart-quote / dash, and single-character typo drift between the "
    "snippet you saw and the file on disk are tolerated automatically. "
    "Match the snippet as faithfully as you can; if `old_text` could "
    "plausibly refer to more than one region of the file, add "
    "surrounding context to disambiguate, OR pass `line: N` (1-based) "
    "to anchor the match near a known line. To replace multiple "
    "occurrences pass either `replace_all: true` (replace every hit) "
    "or `expected_replacements: N` (replace exactly N hits, fail if "
    "the count differs — safer for refactors). No-op edits (old_text "
    "== new_text) are silently skipped, not errors. Include a brief "
    "`display_description` (e.g. 'Fix null-deref in auth.cpp') — it "
    "shows in the card while edits stream.";

} // namespace

void register_edit_tool(Shells& sh) {
    sh.add("edit", kEditDescription, edit_schema(),
           EffectSet{Effect::ReadFs, Effect::WriteFs},
           body<EditArgs>(run_edit, parse_edit_args), 40'000);
}

} // namespace mcp::tools::detail
