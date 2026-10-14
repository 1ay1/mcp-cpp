// SPDX-License-Identifier: Apache-2.0
//
// memory.cpp — the remember / forget / wipe tool SHELLS. The protocol surface
// (names, schemas, arg parsing, scope validation, dedup/pin/tag/supersede
// messaging, dry-run UX) lives here; the actual persistence is the host's
// injected MemoryStore. No file I/O, no JSONL — the library owns the shape,
// the host owns the bytes.

#include "tool_body.hpp"
#include "tool_shell.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::detail {

using mcp::Json;
using util::arg;
using util::ArgSpec;

namespace {

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(b, e - b + 1)};
}

// Weak models send tags as "a,b" instead of ["a","b"]: split every entry.
std::vector<std::string> split_tags(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    for (const auto& t : in) {
        std::size_t start = 0;
        while (start <= t.size()) {
            auto comma = t.find(',', start);
            auto piece = trim(std::string_view{t}.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!piece.empty()) out.push_back(std::move(piece));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return out;
}

struct RememberArgs {
    std::string                text;
    std::optional<std::string> scope;   // default is the store's first scope
    bool                       pin = false;
    std::vector<std::string>   tags;
    std::string                supersedes;
};

struct ForgetArgs {
    std::string id;
    std::string substring;
    bool        dry_run = false;
};

struct WipeArgs {
    std::string scope;
    bool        confirm = false;
};

}  // namespace

void register_memory_tools(Shells& sh, const std::shared_ptr<MemoryStore>& mem) {
    if (!mem) return;

    auto scopes = mem->scopes();
    if (scopes.empty()) return;
    const std::string default_scope = scopes.front();

    std::string scope_desc = "Scope: ";
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        if (i) scope_desc += " | ";
        scope_desc += scopes[i];
    }
    scope_desc += " (default: " + default_scope + ").";

    auto known = [scopes](const std::string& s) {
        return std::find(scopes.begin(), scopes.end(), s) != scopes.end();
    };

    // ── remember ───────────────────────────────────────────────────────────
    auto remember = share(ArgSpec<RememberArgs>{
        arg(&RememberArgs::text, "text",
            "The fact to remember; one short self-contained sentence.")
            .required("remember: `text` is required (one short sentence)."),
        arg(&RememberArgs::scope, "scope", scope_desc).suggest(scopes),
        arg(&RememberArgs::pin, "pin", "Mark cap-exempt; pinned facts survive cap rollover."),
        arg(&RememberArgs::tags, "tags", "Optional grouping labels."),
        arg(&RememberArgs::supersedes, "supersedes", "Record id to atomically replace."),
    });
    sh.add(
        "remember",
        "Persist a fact for future turns. Use when the user asks you to "
        "remember something. The fact is reloaded into context on later turns.",
        remember->schema(),
        EffectSet{},   // pure: persistence is the host's; no fs/net/exec tag for the policy
        spec_body(remember, [mem, scopes, default_scope, known](const RememberArgs& a)
                                -> mcp::cap::Result {
            std::string text = trim(a.text);
            if (text.empty())
                return mcp::cap::Result::error("remember: `text` is required (one short sentence).");

            std::string scope = a.scope.value_or(default_scope);
            if (scope == "global" || scope == "all") scope = scopes.back();
            if (!known(scope))
                return mcp::cap::Result::error("remember: unknown scope '" + scope + "'.");

            MemoryAppendRequest req;
            req.text          = std::move(text);
            req.scope         = std::move(scope);
            req.pinned        = a.pin;
            req.tags          = split_tags(a.tags);
            req.supersedes_id = a.supersedes;

            MemoryAppendResult r = mem->append(req);
            if (!r.error.empty()) return mcp::cap::Result::error("remember: " + r.error);

            std::string msg = r.deduped ? "Already knew that (refreshed " + r.id + ")."
                                        : "Remembered [" + r.id + "].";
            if (!r.note.empty()) msg += " " + r.note;
            if (r.rolled > 0)    msg += " (" + std::to_string(r.rolled) + " old record(s) rolled.)";
            return mcp::cap::Result::ok(msg);
        }));

    // ── forget ───────────────────────────────────────────────────────────
    auto forget = share(ArgSpec<ForgetArgs>{
        arg(&ForgetArgs::id, "id", "Exact record id."),
        arg(&ForgetArgs::substring, "substring", "Remove every record whose text contains this."),
        arg(&ForgetArgs::dry_run, "dry_run", "Preview substring matches without removing."),
    });
    sh.add(
        "forget",
        "Remove a previously-remembered fact. Provide `id` for an exact "
        "record, or `substring` to remove every fact containing that text. "
        "Pass `dry_run:true` with `substring` to preview matches first.",
        forget->schema(),
        EffectSet{},
        spec_body(forget, [mem](const ForgetArgs& a) -> mcp::cap::Result {
            if (!a.id.empty()) {
                auto n = mem->forget_by_id(a.id);
                return mcp::cap::Result::ok(
                    n ? ("Forgot [" + a.id + "].") : ("No record with id " + a.id + "."));
            }
            if (trim(a.substring).empty())
                return mcp::cap::Result::error("forget: provide `id` or a non-empty `substring`.");

            const std::string& sub = a.substring;
            if (a.dry_run) {
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
        }));

    // ── wipe_memory ──────────────────────────────────────────────────────
    auto wipe = share(ArgSpec<WipeArgs>{
        arg(&WipeArgs::scope, "scope", scope_desc)
            .suggest(scopes)
            .required("wipe_memory: `scope` is required."),
        arg(&WipeArgs::confirm, "confirm", "Required true to actually wipe."),
    });
    sh.add(
        "wipe_memory",
        "Wipe every remembered fact in a scope. Call once WITHOUT confirm to "
        "preview the count; re-call with confirm:true to actually wipe.",
        wipe->schema(),
        EffectSet{},
        spec_body(wipe, [mem, known](const WipeArgs& a) -> mcp::cap::Result {
            const std::string& scope = a.scope;
            if (!known(scope))
                return mcp::cap::Result::error("wipe_memory: unknown scope '" + scope + "'.");
            if (!a.confirm) {
                auto n = mem->preview_wipe(scope);
                if (!n) return mcp::cap::Result::error(
                    "wipe_memory: scope '" + scope + "' unresolvable.");
                return mcp::cap::Result::ok(
                    "This will wipe " + std::to_string(*n) + " record(s) from scope '"
                    + scope + "'. Re-call with confirm:true to proceed.");
            }
            auto n = mem->wipe(scope);
            if (!n) return mcp::cap::Result::error("wipe_memory: scope '" + scope + "' unresolvable.");
            return mcp::cap::Result::ok(
                "Wiped " + std::to_string(*n) + " record(s) from scope '" + scope + "'.");
        }));
}

}  // namespace mcp::tools::detail
