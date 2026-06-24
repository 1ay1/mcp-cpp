// SPDX-License-Identifier: Apache-2.0
//
// memory.cpp — the remember / forget / wipe tool SHELLS. The protocol surface
// (names, schemas, arg parsing, scope validation, dedup/pin/tag/supersede
// messaging, dry-run UX) lives here; the actual persistence is the host's
// injected MemoryStore. No file I/O, no JSONL — the library owns the shape,
// the host owns the bytes.

#include "tool_shell.hpp"

#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace mcp::tools::detail {

using mcp::Json;

namespace {

// Parse the tags argument: accept a JSON array of strings OR a single
// comma-separated string (weak models emit "a,b" instead of ["a","b"]).
std::vector<std::string> parse_tags(const Json& j) {
    std::vector<std::string> out;
    if (!j.contains("tags")) return out;
    const auto& t = j["tags"];
    if (t.is_array()) {
        for (const auto& x : t) if (x.is_string()) out.push_back(x.get<std::string>());
    } else if (t.is_string()) {
        std::string s = t.get<std::string>();
        std::size_t start = 0;
        while (start <= s.size()) {
            auto comma = s.find(',', start);
            auto piece = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            // trim
            auto b = piece.find_first_not_of(" \t");
            auto e = piece.find_last_not_of(" \t");
            if (b != std::string::npos) out.push_back(piece.substr(b, e - b + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return out;
}

} // namespace

void register_memory_tools(Shells& sh, const std::shared_ptr<MemoryStore>& mem) {
    if (!mem) return;

    auto scopes = mem->scopes();
    if (scopes.empty()) return;
    const std::string default_scope = scopes.front();

    // Build a "user|project" style enum description for the schema.
    std::string scope_desc = "Scope: ";
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        if (i) scope_desc += " | ";
        scope_desc += scopes[i];
    }
    scope_desc += " (default: " + default_scope + ").";

    // ── remember ───────────────────────────────────────────────────────────
    sh.add(
        "remember",
        "Persist a fact for future turns. Use when the user asks you to "
        "remember something. The fact is reloaded into context on later turns.",
        Json{
            {"type", "object"},
            {"required", {"text"}},
            {"properties", {
                {"text",      {{"type","string"}, {"description","The fact to remember; one short self-contained sentence."}}},
                {"scope",     {{"type","string"}, {"description", scope_desc}}},
                {"pin",       {{"type","boolean"},{"description","Mark cap-exempt; pinned facts survive cap rollover."}}},
                {"tags",      {{"type","array"}, {"items",{{"type","string"}}}, {"description","Optional grouping labels."}}},
                {"supersedes",{{"type","string"}, {"description","Record id to atomically replace."}}},
            }},
        },
        EffectSet{},   // pure: persistence is the host's; no fs/net/exec tag for the policy
        [mem, scopes, default_scope](const Json& args) -> mcp::cap::Result {
            std::string text = args.value("text", std::string{});
            // trim
            auto b = text.find_first_not_of(" \t\r\n");
            auto e = text.find_last_not_of(" \t\r\n");
            text = (b == std::string::npos) ? std::string{} : text.substr(b, e - b + 1);
            if (text.empty())
                return mcp::cap::Result::error("remember: `text` is required (one short sentence).");

            std::string scope = args.value("scope", default_scope);
            if (scope == "global" || scope == "all") scope = scopes.back();
            if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end())
                return mcp::cap::Result::error(
                    "remember: unknown scope '" + scope + "'.");

            MemoryAppendRequest req;
            req.text          = std::move(text);
            req.scope         = std::move(scope);
            req.pinned        = args.value("pin", false);
            req.tags          = parse_tags(args);
            req.supersedes_id = args.value("supersedes", std::string{});

            MemoryAppendResult r = mem->append(req);
            if (!r.error.empty()) return mcp::cap::Result::error("remember: " + r.error);

            std::string msg;
            if (r.deduped) msg = "Already knew that (refreshed " + r.id + ").";
            else           msg = "Remembered [" + r.id + "].";
            if (!r.note.empty())   msg += " " + r.note;
            if (r.rolled > 0)      msg += " (" + std::to_string(r.rolled) + " old record(s) rolled.)";
            return mcp::cap::Result::ok(msg);
        });

    // ── forget ───────────────────────────────────────────────────────────
    sh.add(
        "forget",
        "Remove a previously-remembered fact. Provide `id` for an exact "
        "record, or `substring` to remove every fact containing that text. "
        "Pass `dry_run:true` with `substring` to preview matches first.",
        Json{
            {"type", "object"},
            {"properties", {
                {"id",        {{"type","string"}, {"description","Exact record id."}}},
                {"substring", {{"type","string"}, {"description","Remove every record whose text contains this."}}},
                {"dry_run",   {{"type","boolean"},{"description","Preview substring matches without removing."}}},
            }},
        },
        EffectSet{},
        [mem](const Json& args) -> mcp::cap::Result {
            std::string id  = args.value("id", std::string{});
            std::string sub = args.value("substring", std::string{});
            bool dry = args.value("dry_run", false);

            if (!id.empty()) {
                auto n = mem->forget_by_id(id);
                return mcp::cap::Result::ok(
                    n ? ("Forgot [" + id + "].") : ("No record with id " + id + "."));
            }
            // trim substring
            auto b = sub.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return mcp::cap::Result::error(
                    "forget: provide `id` or a non-empty `substring`.");

            if (dry) {
                auto matches = mem->preview_forget(sub);
                if (matches.empty())
                    return mcp::cap::Result::ok("No records match \"" + sub + "\".");
                std::string out = "Would remove " + std::to_string(matches.size()) + " record(s):\n";
                for (const auto& m : matches) out += "  [" + m.id + "] " + m.text + "\n";
                return mcp::cap::Result::ok(out);
            }
            auto n = mem->forget_by_substring(sub);
            return mcp::cap::Result::ok(
                "Forgot " + std::to_string(n) + " record(s) matching \"" + sub + "\".");
        });

    // ── wipe (a per-scope tool, registered once per scope vocabulary) ──────
    sh.add(
        "wipe_memory",
        "Wipe every remembered fact in a scope. Call once WITHOUT confirm to "
        "preview the count; re-call with confirm:true to actually wipe.",
        Json{
            {"type", "object"},
            {"required", {"scope"}},
            {"properties", {
                {"scope",   {{"type","string"}, {"description", scope_desc}}},
                {"confirm", {{"type","boolean"},{"description","Required true to actually wipe."}}},
            }},
        },
        EffectSet{},
        [mem, scopes](const Json& args) -> mcp::cap::Result {
            std::string scope = args.value("scope", std::string{});
            if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end())
                return mcp::cap::Result::error("wipe_memory: unknown scope '" + scope + "'.");
            bool confirm = args.value("confirm", false);
            if (!confirm) {
                auto preview = mem->preview_forget("");   // host may return all for empty
                return mcp::cap::Result::ok(
                    "This will wipe scope '" + scope + "'. Re-call with confirm:true to proceed.");
            }
            auto n = mem->wipe(scope);
            if (!n) return mcp::cap::Result::error("wipe_memory: scope '" + scope + "' unresolvable.");
            return mcp::cap::Result::ok(
                "Wiped " + std::to_string(*n) + " record(s) from scope '" + scope + "'.");
        });
}

} // namespace mcp::tools::detail
