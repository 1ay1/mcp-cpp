// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/host.hpp — the HOST-SERVICES injection seam for mcp-cpp's
// batteries-included toolset.
//
//   mcp-cpp's `tools` module ships a production-quality set of agent tools —
//   filesystem (read/write/edit/list_dir), shell (bash), code search
//   (grep/glob/find_definition), diagnostics, git, and web (fetch/search).
//   Those are SELF-CONTAINED: their behaviour is fully defined by the library.
//
//   But the most useful agent tools are inherently HOST-COUPLED: "remember a
//   fact", "search my docs", "spawn a subagent", "load a skill", "track a
//   todo list". The DATA and the BACKEND for those live in the host
//   application, not in a protocol library. A naive port would drag the
//   host's database / RAG stack / agent loop into mcp-cpp — the opposite of a
//   reusable library.
//
//   The fix is INVERSION OF CONTROL. mcp-cpp owns each tool's *shell* — its
//   name, JSON schema, argument parsing, output formatting, and protocol
//   surface. The host supplies the one operation the tool actually performs
//   as an injected backend (a small std::function-based interface). A tool
//   is registered ONLY if its backend is installed; absent a backend, the
//   tool simply isn't offered. This is how a host "customises them as it
//   wants": plug in your own memory store, your own retriever, your own
//   subagent runner — agentty is just one consumer.
//
//       HostServices svc;
//       svc.memory   = std::make_shared<MyMemoryStore>();
//       svc.retriever= std::make_shared<MyDocRetriever>();
//       // ... leave svc.subagent null → no `task` tool is registered
//       auto provider = mcp::tools::make_provider(svc, cfg);
//
//   Every interface method is "MUST NOT throw — return an error string"; the
//   tool shells turn that into a clean tool-level error the model can read.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp::tools {

// ─────────────────────────────────────────────────────────────────────────
//  MemoryStore — backend for the remember / forget / wipe tools.
//
//  A scoped key/value-ish fact store. The tool shell owns arg parsing, the
//  scope vocabulary, pin/tag/supersede semantics surface, dedup messaging,
//  and dry-run UX; the host owns persistence (JSONL file, sqlite, a remote
//  service — the library does not care).
// ─────────────────────────────────────────────────────────────────────────
struct MemoryScope {
    // Free-form scope label the host understands ("user", "project", …). The
    // shell passes it through verbatim after light validation against
    // `scopes()`; the host maps it to a storage location.
    std::string name;
};

struct MemoryRecord {
    std::string              id;        // host-assigned stable id
    std::string              text;
    std::string              scope;     // scope name this record lives in
    bool                     pinned = false;
    std::vector<std::string> tags;
    std::int64_t             ts   = 0;  // unix seconds, last-touched
    std::int32_t             hits = 0;  // dedup hit count
};

struct MemoryAppendRequest {
    std::string              text;
    std::string              scope;          // one of scopes()
    bool                     pinned = false;
    std::vector<std::string> tags;
    std::string              supersedes_id;  // empty ⇒ none
};

struct MemoryAppendResult {
    std::string id;            // assigned (or existing, on dedup) id
    std::string error;         // empty ⇒ success
    std::string note;          // human note: truncated / deduped / supersede-missed
    std::size_t rolled  = 0;   // records dropped to satisfy caps
    bool        deduped = false;
};

class MemoryStore {
public:
    virtual ~MemoryStore() = default;

    // The scope vocabulary this store accepts, in preference order. The shell
    // validates the model's `scope` argument against this and uses scopes[0]
    // as the default. Must contain at least one scope.
    [[nodiscard]] virtual std::vector<std::string> scopes() const = 0;

    [[nodiscard]] virtual MemoryAppendResult append(const MemoryAppendRequest&) = 0;

    // Remove by exact id. Returns count removed (0 ⇒ not found).
    [[nodiscard]] virtual std::size_t forget_by_id(const std::string& id) = 0;
    // Remove every record whose text contains `needle` (host decides case
    // sensitivity). Returns count removed.
    [[nodiscard]] virtual std::size_t forget_by_substring(const std::string& needle) = 0;
    // Preview a substring forget without mutating. Returns the matches.
    [[nodiscard]] virtual std::vector<MemoryRecord>
        preview_forget(const std::string& needle) = 0;

    // Wipe an entire scope. Returns count removed, or nullopt if the scope is
    // unresolvable. The confirm gate lives in the shell; this always wipes.
    [[nodiscard]] virtual std::optional<std::size_t> wipe(const std::string& scope) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  TodoSink — backend for the todo tool (session task list).
// ─────────────────────────────────────────────────────────────────────────
struct TodoItem {
    std::string content;
    std::string status;   // host vocabulary: "pending" | "in_progress" | "completed"
};

class TodoSink {
public:
    virtual ~TodoSink() = default;
    // Replace the current list. `error` empty ⇒ success.
    [[nodiscard]] virtual std::string set(std::vector<TodoItem> items) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  SkillResolver — backend for the skill tool ("load a skill body by name").
// ─────────────────────────────────────────────────────────────────────────
class SkillResolver {
public:
    virtual ~SkillResolver() = default;
    // Resolve a skill name to its full instruction body. On success returns
    // the body and leaves `err` empty; on failure returns nullopt + `err`.
    [[nodiscard]] virtual std::optional<std::string>
        load(const std::string& name, std::string& err) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  DocRetriever — backend for the search_docs tool (document/knowledge RAG).
//  The shell owns the k clamp + result formatting; the host owns retrieval.
// ─────────────────────────────────────────────────────────────────────────
struct DocPassage {
    std::string source;       // provenance tag ("docs", "notes:foo", …)
    std::string path;
    int         line_start = 0;
    int         line_end   = 0;
    double      score      = 0.0;
    std::string text;         // the passage body (already compressed by host)
};

struct DocQuery {
    std::string query;
    int         k = 6;
};

class DocRetriever {
public:
    virtual ~DocRetriever() = default;
    // Retrieve up to k passages. `mode` is a human label the host fills
    // ("hybrid" / "BM25-only") for the result header; `err` empty ⇒ success.
    [[nodiscard]] virtual std::vector<DocPassage>
        retrieve(const DocQuery&, std::string& mode, std::string& err) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  SubagentRunner — backend for the task tool (spawn an isolated subagent).
//  The shell owns the schema + nesting/turn guards surface; the host owns the
//  actual agent loop (provider, auth, tool dispatch for the child).
// ─────────────────────────────────────────────────────────────────────────
struct SubagentRequest {
    std::string prompt;
    std::string agent_type;   // "explorer" | "reviewer" | … | "general"
};

class SubagentRunner {
public:
    virtual ~SubagentRunner() = default;
    // True when a subagent can run right now (host installed + depth budget
    // available). The shell refuses with a clear message when false.
    [[nodiscard]] virtual bool available() const = 0;
    // Run a subagent to completion and return its condensed report. On
    // failure returns the error text and sets `is_error`.
    [[nodiscard]] virtual std::string
        run(const SubagentRequest&, bool& is_error) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  HttpClient — backend for the web_fetch / web_search tools. The shells own
//  URL validation, content extraction, result formatting, and search-engine
//  fallback orchestration; the host owns the actual transport (TLS, HTTP/2,
//  redirects, connection reuse). mcp-cpp does not ship an HTTP stack.
// ─────────────────────────────────────────────────────────────────────────
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int         status = 0;     // 0 ⇒ transport failure (see `error`)
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;          // non-empty on transport failure
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // Perform one request, following redirects. MUST NOT throw — map any
    // failure into HttpResponse{.status=0, .error=...}.
    [[nodiscard]] virtual HttpResponse send(const HttpRequest&) = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  HostServices — the bundle a host installs. Any field left null disables
//  the corresponding tool(s). The self-contained Tier-1 tools (fs/shell/
//  search/git) need no host service and are always available (subject to
//  ToolsetConfig toggles). The web tools need an HttpClient.
// ─────────────────────────────────────────────────────────────────────────
struct HostServices {
    std::shared_ptr<MemoryStore>    memory;     // remember / forget / wipe
    std::shared_ptr<TodoSink>       todo;       // todo
    std::shared_ptr<SkillResolver>  skills;     // skill
    std::shared_ptr<DocRetriever>   retriever;  // search_docs
    // Semantic CODE retrieval — the hybrid complement to grep: embeddings/
    // BM25 over source chunks catch CONCEPTUAL queries ("where do we handle
    // rate limiting") that share no token with the code. Same interface as
    // the docs retriever; the host decides how code is chunked/indexed.
    // Null ⇒ no search_code tool.
    std::shared_ptr<DocRetriever>   code_retriever;  // search_code
    std::shared_ptr<SubagentRunner> subagent;   // task
    std::shared_ptr<HttpClient>     http;       // web_fetch / web_search
};

} // namespace mcp::tools
