// SPDX-License-Identifier: Apache-2.0
//
// host_tools.cpp — the remaining host-coupled tool SHELLS: todo, skill,
// search_docs, search_code, task. Each owns its protocol surface; the host
// owns the work via an injected backend. A tool is registered only when its
// backend exists. Arguments are declared once (an ArgSpec) for both the
// schema and the decoder.

#include "tool_body.hpp"
#include "tool_shell.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mcp::tools::detail {

using mcp::Json;
using util::arg;
using util::ArgSpec;

namespace {
// Render the retriever's `mode` string as the results header. The adapter may
// return a compact one-line mode ("hybrid+ctx, convex-fusion, confidence 0.82")
// or a headline plus an indented "funnel" of how the candidate set narrowed;
// the headline goes inline with the count, the funnel on its own lines.
std::string render_mode_header(std::size_t n_results, const std::string& mode) {
    const std::string label = mode.empty() ? std::string("default") : mode;
    const auto nl = label.find('\n');
    if (nl == std::string::npos)
        return std::to_string(n_results) + " results (mode: " + label + ")\n";
    return std::to_string(n_results) + " results (mode: " + label.substr(0, nl) +
           ")" + label.substr(nl) + "\n";
}

// ── retrieval (search_docs, search_code) ──────────────────────────────────
struct RetrieveArgs {
    std::string query;
    int         k = 6;
    std::string display_description;
};

ArgSpec<RetrieveArgs> retrieve_spec(const char* tool, const char* query_desc, const char* k_desc) {
    return ArgSpec<RetrieveArgs>{
        arg(&RetrieveArgs::query, "query", query_desc)
            .required(std::string{tool} + ": `query` is required."),
        arg(&RetrieveArgs::k, "k", k_desc).range(1, 20),
        arg(&RetrieveArgs::display_description, "display_description",
            "One-line summary shown in the UI. Optional."),
    };
}
}  // namespace

// ── todo ──────────────────────────────────────────────────────────────────
// The list is rendered to text the host's UI observes; an optional TodoSink
// also receives the structured items for hosts that track session state.
namespace {
struct TodoArgs {
    Json        todos;   // [{content, status}], read leniently below
    std::string display_description;
};
}  // namespace

void register_todo_tool(Shells& sh, const std::shared_ptr<TodoSink>& sink) {
    auto spec = share(ArgSpec<TodoArgs>{
        arg(&TodoArgs::display_description, "display_description",
            "One-line summary shown in the UI. Optional."),
        arg(&TodoArgs::todos, "todos")
            .shape(Json{{"type", "array"},
                        {"items", {{"type", "object"},
                                   {"properties", {{"content", {{"type", "string"}}},
                                                   {"status", {{"type", "string"},
                                                               {"enum", {"pending", "in_progress", "completed"}}}}}},
                                   {"required", {"content", "status"}}}}})
            .advertise_required(),
    });
    sh.add(
        "todo",
        "Maintain the session todo list. Overwrites with the provided list.",
        spec->schema(),
        EffectSet{},
        spec_body(spec, [sink](const TodoArgs& a) -> mcp::cap::Result {
            std::string out;
            std::vector<TodoItem> items;
            if (a.todos.is_array()) {
                // Each item is a small record; malformed ones are skipped.
                for (const auto& td : a.todos) {
                    if (!td.is_object()) continue;
                    util::ArgReader r(td);
                    std::string content = r.str("content", "");
                    std::string status  = r.str("status", "pending");
                    char mark = status == "completed"   ? 'x'
                              : status == "in_progress" ? '-'
                                                        : ' ';
                    out += std::string{"["} + mark + "] " + content + "\n";
                    items.push_back(TodoItem{content, status});
                }
            }
            if (sink) {
                std::string set_err = sink->set(std::move(items));
                if (!set_err.empty())
                    return mcp::cap::Result::error(std::move(set_err));
            }
            // Never an empty success: models read "" as "nothing happened"
            // and retry the same call in a loop. Say what the list now is.
            if (out.empty())
                out = "(todo list is now empty — 0 items)";
            return mcp::cap::Result::ok(out);
        }));
}

// ── skill ─────────────────────────────────────────────────────────────────
namespace {
struct SkillArgs {
    std::string name;
    std::string display_description;
};
}  // namespace

void register_skill_tool(Shells& sh, const std::shared_ptr<SkillResolver>& res) {
    if (!res) return;
    auto spec = share(ArgSpec<SkillArgs>{
        arg(&SkillArgs::name, "name", "Exact skill name from the catalog.")
            .required("skill: `name` is required."),
        arg(&SkillArgs::display_description, "display_description",
            "One-line summary shown in the UI. Optional."),
    });
    sh.add(
        "skill",
        "Load the full instructions for a named skill. Call BEFORE attempting "
        "a task a skill covers \u2014 the catalog only lists names + summaries.",
        spec->schema(),
        EffectSet{},
        spec_body(spec, [res](const Call& call, const SkillArgs& a) -> mcp::cap::Result {
            std::string err;
            auto body = res->load(call, a.name, err);
            if (!body)
                return mcp::cap::Result::error(
                    err.empty() ? ("skill: unknown skill '" + a.name + "'.") : err);
            return mcp::cap::Result::ok(*body);
        }));
}

// ── search_docs ─────────────────────────────────────────────────────────
void register_search_docs_tool(Shells& sh, const std::shared_ptr<DocRetriever>& ret) {
    if (!ret) return;
    auto spec = share(retrieve_spec("search_docs", "Natural-language or keyword query.",
        "Number of passages to return (default 6, max 20). Raise to 10-15 for "
        "BROAD/survey questions (\"how does X work end-to-end\", \"what are all "
        "the Y\") where one facet per passage isn't enough; keep the default "
        "for pinpoint lookups."));
    sh.add(
        "search_docs",
        "Search the user's KNOWLEDGE BASE — their documentation, your installed "
        "SKILLS, and your LEARNED MEMORY (facts remembered across sessions) — "
        "and return the most relevant passages, each tagged with its source "
        "(docs:/skill:/memory:) and path. This is separate from source code.\n"
        "CALL THIS when the user references project conventions, past decisions, "
        "domain terms, an in-house tool/DSL, or anything you might have stored "
        "or been given docs about — even if no docs directory is configured, "
        "skills and learned memory are always indexed and searchable. A hit on "
        "a skill:// path means a SKILL covers that topic: activate it with the "
        "`skill` tool. Cheaper and more precise than guessing from training "
        "data on anything project-specific.",
        spec->schema(),
        EffectSet{Effect::Net},
        spec_body(spec, [ret](const RetrieveArgs& a) -> mcp::cap::Result {
            DocQuery q;
            q.query = a.query;
            q.k     = a.k;
            std::string mode, err;
            auto hits = ret->retrieve(q, mode, err);
            if (!err.empty()) return mcp::cap::Result::error("search_docs: " + err);

            std::string body;
            if (hits.empty())
                return mcp::cap::Result::ok(body + "No matching documents for: " + q.query);

            body += render_mode_header(hits.size(), mode);
            char score_buf[32];
            for (const auto& h : hits) {
                std::snprintf(score_buf, sizeof score_buf, "%.4f", h.score);
                std::string tag = h.source.empty() ? std::string{} : h.source + ":";
                body += "\n\u2500\u2500 " + tag + h.path + ":" +
                        std::to_string(h.line_start) + "-" + std::to_string(h.line_end) +
                        "  (score " + score_buf + ")\n" + h.text;
                if (!body.empty() && body.back() != '\n') body += '\n';
            }
            return mcp::cap::Result::ok(body);
        }));
}

// ── search_code ────────────────────────────────────────────────────
// Semantic code retrieval: the conceptual complement to grep. Same
// DocRetriever seam as search_docs; the host owns chunking and indexing.
void register_search_code_tool(Shells& sh, const std::shared_ptr<DocRetriever>& ret) {
    if (!ret) return;
    auto spec = share(retrieve_spec("search_code",
        "Natural-language description of the code you're looking for.",
        "Number of code passages to return (default 6, max 20). Raise to 10-15 "
        "when tracing a feature across MANY files (\"everywhere we handle "
        "retries\"); keep the default when hunting one function."));
    sh.add(
        "search_code",
        "Semantic search over SOURCE CODE by meaning, not literal text. Use "
        "when you don't know the identifier: conceptual queries (\"where is "
        "retry backoff handled\", \"code that validates auth tokens\") "
        "match relevant functions even with zero shared keywords. For exact "
        "names/strings, grep is better.",
        spec->schema(),
        EffectSet{Effect::ReadFs, Effect::Net},
        spec_body(spec, [ret](const RetrieveArgs& a) -> mcp::cap::Result {
            DocQuery q;
            q.query = a.query;
            q.k     = a.k;
            std::string mode, err;
            auto hits = ret->retrieve(q, mode, err);
            if (!err.empty()) return mcp::cap::Result::error("search_code: " + err);

            std::string body;
            if (hits.empty())
                return mcp::cap::Result::ok(body + "No semantically similar code for: "
                                            + q.query + "\nTry grep for exact tokens.");

            body += render_mode_header(hits.size(), mode);
            char score_buf[32];
            for (const auto& h : hits) {
                std::snprintf(score_buf, sizeof score_buf, "%.4f", h.score);
                body += "\n\u2500\u2500 " + h.path + ":" +
                        std::to_string(h.line_start) + "-" + std::to_string(h.line_end) +
                        "  (score " + score_buf + ")\n" + h.text;
                if (!body.empty() && body.back() != '\n') body += '\n';
            }
            return mcp::cap::Result::ok(body);
        }));
}

// ── task ─────────────────────────────────────────────────────────────────
namespace {
struct TaskArgs {
    std::string prompt;
    std::string agent_type = "general";
    std::string display_description;
};
}  // namespace

void register_task_tool(Shells& sh, const std::shared_ptr<SubagentRunner>& runner) {
    if (!runner) return;
    // Built-in five + any host-defined types (user-authored agents), listed
    // so they're discoverable in the schema the model sees.
    std::vector<std::string> types{"explorer", "reviewer", "tester", "coder", "general"};
    std::string type_desc = "Subagent specialisation (default general).";
    if (auto extra = runner->extra_agent_types(); !extra.empty()) {
        type_desc += " User-defined:";
        for (auto& t : extra) {
            types.push_back(t);
            type_desc += " " + t;
        }
        type_desc += ".";
    }
    // suggest, not one_of: the runner decides what an unknown type means.
    auto spec = share(ArgSpec<TaskArgs>{
        arg(&TaskArgs::prompt, "prompt", "Complete, self-contained task description.")
            .required("task: `prompt` is required."),
        arg(&TaskArgs::agent_type, "agent_type", type_desc).suggest(std::move(types)),
        arg(&TaskArgs::display_description, "display_description",
            "One-line summary shown in the UI. Optional."),
    });
    sh.add(
        "task",
        "Spawn an autonomous subagent to complete a self-contained task in "
        "isolation (own context window). It returns ONE condensed report; give "
        "a complete, unambiguous prompt with all the context it needs.",
        spec->schema(),
        EffectSet{Effect::ReadFs, Effect::Net},
        spec_body(spec, [runner](const Call& call, const TaskArgs& a) -> mcp::cap::Result {
            if (auto reason = runner->unavailable_reason(call); !reason.empty())
                return mcp::cap::Result::error("task: " + std::move(reason));
            SubagentRequest req;
            req.prompt     = a.prompt;
            req.agent_type = a.agent_type;
            bool is_error = false;
            std::string report = runner->run(call, req, is_error);
            return is_error ? mcp::cap::Result::error(report)
                            : mcp::cap::Result::ok(report);
        }));
}

} // namespace mcp::tools::detail
