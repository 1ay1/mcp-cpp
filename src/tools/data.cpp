// SPDX-License-Identifier: Apache-2.0
//
// data.cpp — register_data_tools: json_query, a jq-lite for structured data.
//
// grep/extract are line-oriented and blind to structure; a config or data
// file wants a PATH query, not a regex. json_query evaluates a dotted jq-style
// path over a JSON document (a `.json` file, or inline `json`) and returns the
// selected value(s) — `.scripts | keys`, `.dependencies.react`,
// `[].name`, `.services[2].port`. JSON only (the format the toolset already
// parses); YAML/TOML would need a new parser dependency.

#include "tool_body.hpp"
#include "tool_shell.hpp"

#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/utf8.hpp>

#include <algorithm>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::detail {
namespace {

namespace fs = std::filesystem;
using util::ToolError;
using util::ExecResult;
using util::ToolOutput;
using json = nlohmann::json;

constexpr std::size_t kMaxOutputBytes = 20'000;
constexpr int         kMaxResults     = 2000;

// One step in a parsed path.
struct Step {
    enum class Kind { Key, Index, Slice, Iterate,
                      Keys, Values, Length, Type, Has } kind;
    std::string key;        // Key / Has
    long        i = 0;       // Index / Slice.lo
    long        j = 0;       // Slice.hi
    bool        j_set = false;
};

// Parse a jq-lite path into steps. Grammar:
//   .key            object member (bareword: [A-Za-z0-9_-])
//   .["key"]        quoted member (any chars)
//   ["key"]         same, leading form
//   [i]             array index (negative counts from end)
//   [lo:hi]         array slice (either bound optional)
//   []              iterate array/object values
//   | keys | values | length | type | has(x)   pipeline verbs
// A leading "." (identity) selects the whole document.
[[nodiscard]] std::vector<Step> parse_path(std::string_view p, std::string& err) {
    std::vector<Step> out;
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < p.size() && std::isspace((unsigned char)p[i])) ++i; };

    skip_ws();
    while (i < p.size()) {
        skip_ws();
        if (i >= p.size()) break;
        char c = p[i];

        // A bareword verb at the START of a path (or after a `|`) is a
        // top-level filter: `has("k")`, `keys`, `length`, `type`, `values`.
        // jq treats these as standalone filters, so accept them without a
        // leading `.`/`|`. We detect a letter that isn't part of `.key`.
        if (c == '|' || std::isalpha((unsigned char)c)) {
            if (c == '|') { ++i; skip_ws(); }
            // pipeline verb
            std::size_t s = i;
            while (i < p.size() && (std::isalpha((unsigned char)p[i]))) ++i;
            std::string verb{p.substr(s, i - s)};
            if      (verb == "keys")   out.push_back({Step::Kind::Keys});
            else if (verb == "values") out.push_back({Step::Kind::Values});
            else if (verb == "length") out.push_back({Step::Kind::Length});
            else if (verb == "type")   out.push_back({Step::Kind::Type});
            else if (verb == "has") {
                skip_ws();
                if (i >= p.size() || p[i] != '(') { err = "has() needs an argument"; return {}; }
                ++i; std::size_t a = i;
                while (i < p.size() && p[i] != ')') ++i;
                std::string arg{p.substr(a, i - a)};
                if (i < p.size()) ++i;  // consume ')'
                // strip optional quotes
                if (arg.size() >= 2 && (arg.front()=='"'||arg.front()=='\'')) arg = arg.substr(1, arg.size()-2);
                out.push_back({Step::Kind::Has, arg});
            }
            else { err = "unknown pipeline verb `" + verb + "` (keys|values|length|type|has)"; return {}; }
            continue;
        }

        if (c == '.') {
            ++i;
            if (i >= p.size()) break;               // trailing dot / identity
            if (p[i] == '[') continue;              // .[ … ] handled below
            if (p[i] == '"') {                      // .["x"]  is [ form; ."x" also
                ++i; std::size_t s = i;
                while (i < p.size() && p[i] != '"') ++i;
                out.push_back({Step::Kind::Key, std::string{p.substr(s, i - s)}});
                if (i < p.size()) ++i;
                continue;
            }
            std::size_t s = i;
            while (i < p.size() && (std::isalnum((unsigned char)p[i]) || p[i]=='_' || p[i]=='-')) ++i;
            if (i == s) { err = "empty key after '.'"; return {}; }
            out.push_back({Step::Kind::Key, std::string{p.substr(s, i - s)}});
            continue;
        }

        if (c == '[') {
            ++i; skip_ws();
            if (i < p.size() && p[i] == ']') { ++i; out.push_back({Step::Kind::Iterate}); continue; }
            if (i < p.size() && (p[i]=='"' || p[i]=='\'')) {   // ["key"]
                char q = p[i]; ++i; std::size_t s = i;
                while (i < p.size() && p[i] != q) ++i;
                out.push_back({Step::Kind::Key, std::string{p.substr(s, i - s)}});
                if (i < p.size()) ++i;
                skip_ws(); if (i < p.size() && p[i]==']') ++i;
                continue;
            }
            // numeric index or slice
            std::size_t s = i;
            while (i < p.size() && p[i] != ']') ++i;
            std::string body{p.substr(s, i - s)};
            if (i < p.size()) ++i;  // ']'
            auto colon = body.find(':');
            if (colon == std::string::npos) {
                try { out.push_back({Step::Kind::Index, "", std::stol(body)}); }
                catch (...) { err = "bad array index `" + body + "`"; return {}; }
            } else {
                Step st{Step::Kind::Slice};
                std::string lo = body.substr(0, colon), hi = body.substr(colon+1);
                try {
                    st.i = lo.empty() ? 0 : std::stol(lo);
                    if (!hi.empty()) { st.j = std::stol(hi); st.j_set = true; }
                } catch (...) { err = "bad slice `" + body + "`"; return {}; }
                out.push_back(st);
            }
            continue;
        }

        err = std::string("unexpected character '") + c + "' in path";
        return {};
    }
    return out;
}

// Apply one step to a set of values, producing the next set.
[[nodiscard]] bool apply_step(const Step& st, std::vector<const json*>& cur,
                              std::deque<json>& owned, std::string& err) {
    std::vector<const json*> next;
    auto keep = [&](const json& v) { owned.push_back(v); next.push_back(&owned.back()); };

    for (const json* vp : cur) {
        const json& v = *vp;
        switch (st.kind) {
        case Step::Kind::Key:
            if (!v.is_object()) { err = "`." + st.key + "` on a non-object ("
                                        + std::string(v.type_name()) + ")"; return false; }
            if (auto it = v.find(st.key); it != v.end()) next.push_back(&*it);
            // missing key → drops out (jq yields null; we omit for brevity)
            break;
        case Step::Kind::Index: {
            if (!v.is_array()) { err = "index on a non-array"; return false; }
            long n = (long)v.size(); long idx = st.i < 0 ? n + st.i : st.i;
            if (idx >= 0 && idx < n) next.push_back(&v[(std::size_t)idx]);
            break; }
        case Step::Kind::Slice: {
            if (!v.is_array()) { err = "slice on a non-array"; return false; }
            long n = (long)v.size();
            long lo = st.i < 0 ? n + st.i : st.i;
            long hi = st.j_set ? (st.j < 0 ? n + st.j : st.j) : n;
            lo = std::clamp(lo, 0L, n); hi = std::clamp(hi, 0L, n);
            // jq semantics: a slice is ONE array value, not a stream of
            // elements (so `[0:2] | length` == 2, not two `2`s).
            json sub = json::array();
            for (long k = lo; k < hi; ++k) sub.push_back(v[(std::size_t)k]);
            keep(std::move(sub));
            break; }
        case Step::Kind::Iterate:
            if (v.is_array())       for (const auto& e : v) next.push_back(&e);
            else if (v.is_object()) for (const auto& e : v.items()) next.push_back(&e.value());
            else { err = "[] on a non-container"; return false; }
            break;
        case Step::Kind::Keys: {
            json arr = json::array();
            if (v.is_object())      for (auto& e : v.items()) arr.push_back(e.key());
            else if (v.is_array())  for (std::size_t k=0;k<v.size();++k) arr.push_back((long)k);
            else { err = "keys on a non-container"; return false; }
            keep(arr); break; }
        case Step::Kind::Values: {
            json arr = json::array();
            if (v.is_object())      for (auto& e : v.items()) arr.push_back(e.value());
            else if (v.is_array())  arr = v;
            else { err = "values on a non-container"; return false; }
            keep(arr); break; }
        case Step::Kind::Length: {
            long len = v.is_string() ? (long)v.get<std::string>().size()
                     : v.is_array() || v.is_object() ? (long)v.size()
                     : v.is_null() ? 0 : 1;
            keep(json(len)); break; }
        case Step::Kind::Type:
            keep(json(v.type_name())); break;
        case Step::Kind::Has:
            keep(json(v.is_object() && v.contains(st.key))); break;
        }
    }
    cur = std::move(next);
    return true;
}

struct QueryArgs {
    std::string path;      // file path (mutually exclusive-ish with json_text)
    std::string json_text; // inline JSON
    std::string query;
    bool        raw = false;   // print strings unquoted
    bool        compact = true;
    std::string display_description;
};

std::expected<QueryArgs, ToolError> parse_query_args(const json& j) {
    util::ArgReader r(j);
    if (!r.is_object())
        return std::unexpected(ToolError::invalid_args("expected a JSON object"));
    QueryArgs a;
    a.path      = r.str("path");
    a.json_text = r.str("json");
    a.query     = r.str("query", ".");
    if (a.query.empty()) a.query = ".";
    a.raw       = r.boolean("raw", false);
    a.compact   = r.boolean("compact", true);
    a.display_description = r.str("display_description");
    if (a.path.empty() && a.json_text.empty())
        return std::unexpected(ToolError::invalid_args(
            "provide `path` (a .json file) or inline `json`"));
    return a;
}

ExecResult run_json_query(const QueryArgs& a) {
    std::string text;
    if (!a.path.empty()) {
        auto wp = util::make_readable_path_checked(a.path, "json_query");
        if (!wp) return std::unexpected(std::move(wp.error()));
        const auto& p = wp->path();
        std::error_code ec;
        if (!fs::exists(p, ec))  return std::unexpected(ToolError::not_found(wp->string()));
        if (!fs::is_regular_file(p, ec)) return std::unexpected(ToolError::not_a_file(wp->string()));
        if (auto sz = fs::file_size(p, ec); !ec && sz > 16u*1024u*1024u)
            return std::unexpected(ToolError::too_large("file > 16 MiB"));
        try { text = util::read_file(*wp); }
        catch (...) { return std::unexpected(ToolError::io("cannot read " + wp->string())); }
    } else {
        text = a.json_text;
    }

    json doc;
    try { doc = json::parse(text); }
    catch (const json::parse_error& e) {
        return std::unexpected(ToolError::invalid_args(
            std::string("not valid JSON: ") + e.what()));
    }

    std::string perr;
    auto steps = parse_path(a.query, perr);
    if (!perr.empty())
        return std::unexpected(ToolError::invalid_args(
            "bad query `" + a.query + "`: " + perr));

    std::deque<json> owned;
    std::vector<const json*> cur{&doc};
    std::string err;
    for (const auto& st : steps)
        if (!apply_step(st, cur, owned, err))
            return std::unexpected(ToolError::invalid_args(
                "query `" + a.query + "`: " + err));

    if (cur.empty())
        return ToolOutput{"(no match \u2014 the path selected nothing)", std::nullopt};

    std::ostringstream out;
    if (!a.display_description.empty()) out << a.display_description << "\n";
    int shown = 0;
    for (const json* vp : cur) {
        if (shown++ >= kMaxResults) { out << "\u2026 " << (cur.size()-kMaxResults)
                                          << " more results\n"; break; }
        if (a.raw && vp->is_string()) out << vp->get<std::string>() << "\n";
        else out << (a.compact ? vp->dump() : vp->dump(2)) << "\n";
        if ((std::size_t)out.tellp() >= kMaxOutputBytes) { out << "[capped]\n"; break; }
    }
    std::string body = out.str();
    if (!body.empty() && body.back() == '\n') body.pop_back();
    return ToolOutput{util::to_valid_utf8(std::move(body)), std::nullopt};
}

json json_query_schema() {
    return json{{"type","object"},{"required",{"query"}},{"properties",{
        {"display_description",{{"type","string"},{"description","One-line summary shown in the UI. Optional."}}},
        {"query",{{"type","string"},{"description","jq-lite path: `.` (whole doc), `.key`, `.[\"key with space\"]`, `[i]` (negative = from end), `[lo:hi]` slice, `[]` iterate, and pipeline verbs `| keys | values | length | type | has(k)`. E.g. `.scripts | keys`, `.dependencies.react`, `.services[].name`."}}},
        {"path",{{"type","string"},{"description","A .json file to query. Provide this OR inline `json`."}}},
        {"json",{{"type","string"},{"description","Inline JSON to query instead of a file."}}},
        {"raw",{{"type","boolean"},{"description","Print string results unquoted (jq -r)."}}},
        {"compact",{{"type","boolean"},{"description","Compact one-line JSON per result (default true); false pretty-prints."}}},
    }}};
}

} // namespace

void register_data_tools(Shells& sh) {
    sh.add("json_query",
        "Query a JSON document by PATH instead of regex \u2014 a jq-lite for "
        "structured data. Point it at a `.json` file (or inline `json`) and a "
        "jq-style `query`: `.scripts | keys`, `.dependencies.react`, "
        "`.services[].name`, `.compilerOptions.paths`, `[0].id`. Supports "
        "`.key`, `[i]` (negative from end), `[lo:hi]` slices, `[]` iteration, "
        "and `| keys | values | length | type | has(k)`. Each result prints as "
        "one compact JSON line (`raw:true` unquotes strings). Structure-aware "
        "where grep/extract are blind \u2014 reach for it on package.json, "
        "tsconfig, compile_commands.json, API fixtures, MCP configs. JSON only.",
        json_query_schema(), EffectSet{Effect::ReadFs},
        body<QueryArgs>(run_json_query, parse_query_args), 25'000);
}

} // namespace mcp::tools::detail
