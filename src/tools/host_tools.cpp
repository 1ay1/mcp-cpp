// SPDX-License-Identifier: Apache-2.0
//
// host_tools.cpp — the remaining host-coupled tool SHELLS: todo, skill,
// search_docs, task. Each owns its protocol surface; the host owns the work
// via an injected backend. A tool is registered only when its backend exists.

#include "tool_shell.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace mcp::tools::detail {

using mcp::Json;

// ── todo ──────────────────────────────────────────────────────────────────
// The list is rendered to text the host's UI observes; an optional TodoSink
// also receives the structured items for hosts that track session state.
void register_todo_tool(Shells& sh, const std::shared_ptr<TodoSink>& sink) {
    sh.add(
        "todo",
        "Maintain the session todo list. Overwrites with the provided list.",
        Json{
            {"type","object"},
            {"required", {"todos"}},
            {"properties", {
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
                {"todos", {{"type","array"},
                    {"items", {{"type","object"},
                        {"properties", {
                            {"content", {{"type","string"}}},
                            {"status",  {{"type","string"},
                                {"enum", {"pending","in_progress","completed"}}}},
                        }},
                        {"required", {"content","status"}},
                    }},
                }},
            }},
        },
        EffectSet{},
        [sink](const Json& args) -> mcp::cap::Result {
            std::string desc = args.value("display_description", std::string{});
            std::string out;
            if (!desc.empty()) out += desc + "\n\n";
            std::vector<TodoItem> items;
            if (args.contains("todos") && args["todos"].is_array()) {
                for (const auto& td : args["todos"]) {
                    if (!td.is_object()) continue;
                    std::string content = td.value("content", std::string{});
                    std::string status  = td.value("status", std::string{"pending"});
                    char mark = status == "completed"   ? 'x'
                              : status == "in_progress" ? '-'
                                                        : ' ';
                    out += std::string{"["} + mark + "] " + content + "\n";
                    items.push_back(TodoItem{content, status});
                }
            }
            if (sink) sink->set(std::move(items));
            return mcp::cap::Result::ok(out);
        });
}

// ── skill ─────────────────────────────────────────────────────────────────
void register_skill_tool(Shells& sh, const std::shared_ptr<SkillResolver>& res) {
    if (!res) return;
    sh.add(
        "skill",
        "Load the full instructions for a named skill. Call BEFORE attempting "
        "a task a skill covers \u2014 the catalog only lists names + summaries.",
        Json{
            {"type","object"},
            {"required", {"name"}},
            {"properties", {
                {"name", {{"type","string"}, {"description","Exact skill name from the catalog."}}},
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
            }},
        },
        EffectSet{},
        [res](const Json& args) -> mcp::cap::Result {
            std::string name = args.value("name", std::string{});
            if (name.empty())
                return mcp::cap::Result::error("skill: `name` is required.");
            std::string err;
            auto body = res->load(name, err);
            if (!body)
                return mcp::cap::Result::error(
                    err.empty() ? ("skill: unknown skill '" + name + "'.") : err);
            return mcp::cap::Result::ok(*body);
        });
}

// ── search_docs ─────────────────────────────────────────────────────────
void register_search_docs_tool(Shells& sh, const std::shared_ptr<DocRetriever>& ret) {
    if (!ret) return;
    sh.add(
        "search_docs",
        "Search the user's KNOWLEDGE BASE — their documentation, your installed "
        "SKILLS, and your LEARNED MEMORY (facts remembered across sessions) — "
        "and return the most relevant passages, each tagged with its source "
        "(docs:/skill:/memory:) and path. This is separate from source code: "
        "for code use grep/read/search_code, NOT this.\n"
        "CALL THIS when the user references project conventions, past decisions, "
        "domain terms, an in-house tool/DSL, or anything you might have stored "
        "or been given docs about — even if no docs directory is configured, "
        "skills and learned memory are always indexed and searchable. A hit on "
        "a skill:// path means a SKILL covers that topic: activate it with the "
        "`skill` tool. Cheaper and more precise than guessing from training "
        "data on anything project-specific.",
        Json{
            {"type","object"},
            {"required", {"query"}},
            {"properties", {
                {"query", {{"type","string"}, {"description","Natural-language or keyword query."}}},
                {"k",     {{"type","integer"},{"description","Number of passages to return (default 6, max 20). Raise to 10-15 for BROAD/survey questions (\"how does X work end-to-end\", \"what are all the Y\") where one facet per passage isn't enough; keep the default for pinpoint lookups."}}},
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
            }},
        },
        EffectSet{Effect::Net},
        [ret](const Json& args) -> mcp::cap::Result {
            DocQuery q;
            q.query = args.value("query", std::string{});
            if (q.query.empty())
                return mcp::cap::Result::error("search_docs: `query` is required.");
            q.k = args.value("k", 6);
            if (q.k < 1)  q.k = 1;
            if (q.k > 20) q.k = 20;

            std::string mode, err;
            auto hits = ret->retrieve(q, mode, err);
            if (!err.empty()) return mcp::cap::Result::error("search_docs: " + err);

            std::string desc = args.value("display_description", std::string{});
            std::string body;
            if (!desc.empty()) body += desc + "\n";
            if (hits.empty())
                return mcp::cap::Result::ok(body + "No matching documents for: " + q.query);

            body += std::to_string(hits.size()) + " results (mode: " +
                    (mode.empty() ? "default" : mode) + ")\n";
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
        });
}

// ── search_code ────────────────────────────────────────────────────
// Semantic code retrieval — the CONCEPTUAL complement to grep. grep answers
// exact/structural questions ("where is X defined"); this answers meaning
// questions ("where do we throttle requests") where the code shares no token
// with the query. Same DocRetriever seam as search_docs; the host owns the
// code chunking/indexing/invalidation strategy.
void register_search_code_tool(Shells& sh, const std::shared_ptr<DocRetriever>& ret) {
    if (!ret) return;
    sh.add(
        "search_code",
        "Semantic search over SOURCE CODE by meaning, not literal text. Use "
        "when you don't know the identifier: conceptual queries (\"where is "
        "retry backoff handled\", \"code that validates auth tokens\") "
        "match relevant functions even with zero shared keywords. For exact "
        "names/strings, grep is better.",
        Json{
            {"type","object"},
            {"required", {"query"}},
            {"properties", {
                {"query", {{"type","string"}, {"description","Natural-language description of the code you're looking for."}}},
                {"k",     {{"type","integer"},{"description","Number of code passages to return (default 6, max 20). Raise to 10-15 when tracing a feature across MANY files (\"everywhere we handle retries\"); keep the default when hunting one function."}}},
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
            }},
        },
        EffectSet{Effect::ReadFs, Effect::Net},
        [ret](const Json& args) -> mcp::cap::Result {
            DocQuery q;
            q.query = args.value("query", std::string{});
            if (q.query.empty())
                return mcp::cap::Result::error("search_code: `query` is required.");
            q.k = args.value("k", 6);
            if (q.k < 1)  q.k = 1;
            if (q.k > 20) q.k = 20;

            std::string mode, err;
            auto hits = ret->retrieve(q, mode, err);
            if (!err.empty()) return mcp::cap::Result::error("search_code: " + err);

            std::string desc = args.value("display_description", std::string{});
            std::string body;
            if (!desc.empty()) body += desc + "\n";
            if (hits.empty())
                return mcp::cap::Result::ok(body + "No semantically similar code for: "
                                            + q.query + "\nTry grep for exact tokens.");

            body += std::to_string(hits.size()) + " results (mode: " +
                    (mode.empty() ? "default" : mode) + ")\n";
            char score_buf[32];
            for (const auto& h : hits) {
                std::snprintf(score_buf, sizeof score_buf, "%.4f", h.score);
                body += "\n\u2500\u2500 " + h.path + ":" +
                        std::to_string(h.line_start) + "-" + std::to_string(h.line_end) +
                        "  (score " + score_buf + ")\n" + h.text;
                if (!body.empty() && body.back() != '\n') body += '\n';
            }
            return mcp::cap::Result::ok(body);
        });
}

// ── task ─────────────────────────────────────────────────────────────────
void register_task_tool(Shells& sh, const std::shared_ptr<SubagentRunner>& runner) {
    if (!runner) return;
    sh.add(
        "task",
        "Spawn an autonomous subagent to complete a self-contained task in "
        "isolation (own context window). It returns ONE condensed report; give "
        "a complete, unambiguous prompt with all the context it needs.",
        Json{
            {"type","object"},
            {"required", {"prompt"}},
            {"properties", {
                {"prompt", {{"type","string"}, {"description","Complete, self-contained task description."}}},
                {"agent_type", {{"type","string"},
                    {"enum", {"explorer","reviewer","tester","coder","general"}},
                    {"description","Subagent specialisation (default general)."}}},
                {"display_description", {{"type","string"},
                    {"description","One-line summary shown in the UI. Optional."}}},
            }},
        },
        EffectSet{Effect::ReadFs, Effect::Net},
        [runner](const Json& args) -> mcp::cap::Result {
            if (!runner->available())
                return mcp::cap::Result::error(
                    "task: subagent unavailable (not configured, or max nesting depth reached).");
            SubagentRequest req;
            req.prompt = args.value("prompt", std::string{});
            if (req.prompt.empty())
                return mcp::cap::Result::error("task: `prompt` is required.");
            req.agent_type = args.value("agent_type", std::string{"general"});
            bool is_error = false;
            std::string report = runner->run(req, is_error);
            return is_error ? mcp::cap::Result::error(report)
                            : mcp::cap::Result::ok(report);
        });
}

} // namespace mcp::tools::detail
