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

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <mcp/tools/state.hpp>

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

    // Preview an entire scope without mutating. Returns the exact record count,
    // or nullopt if the scope is unresolvable.
    [[nodiscard]] virtual std::optional<std::size_t>
        preview_wipe(const std::string& scope) = 0;

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
    // `call` is the tool call asking (its host data, reader, cancel).
    [[nodiscard]] virtual std::optional<std::string>
        load(const Call& call, const std::string& name, std::string& err) = 0;
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

    // Whether the retriever can answer an OPPORTUNISTIC query — one issued
    // as a side-effect of another tool (e.g. search_structural's zero-hit
    // leads / over-budget ordering) rather than an explicit search — without
    // paying a heavy cold start (index build, embedder round-trips). Hosts
    // with a lazily-built index should return true only once it's warm.
    // Explicit tools (search_code / search_docs) ignore this and always call
    // retrieve(). Default: always ready.
    [[nodiscard]] virtual bool warm() const { return true; }
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
    // Empty when a subagent can run right now; otherwise an actionable reason
    // suitable for returning directly as a tool execution error.
    [[nodiscard]] virtual std::string unavailable_reason(const Call& call) const = 0;
    [[nodiscard]] bool available(const Call& call) const { return unavailable_reason(call).empty(); }
    // Run a subagent to completion and return its condensed report. On
    // failure returns the error text and sets `is_error`. `call` is the task
    // tool call (its progress sink, cancel, host data).
    [[nodiscard]] virtual std::string
        run(const Call& call, const SubagentRequest&, bool& is_error) = 0;
    // Host-defined EXTRA agent types beyond the built-in five (user-authored
    // .agentty/agents/*.md in agentty). Merged into the task tool's
    // agent_type enum at registration so the model can discover them.
    // Default: none.
    [[nodiscard]] virtual std::vector<std::string> extra_agent_types() const {
        return {};
    }
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
// ── Exec: running another program ───────────────────────────────────────
//
// mcp-cpp does not run processes. It asks.
//
// This is the same decision already made for HttpClient one field below:
// web_fetch does not own a socket, a TLS stack or a redirect policy, it
// states what it wants and the host does it. Exec is the same shape for the
// same reason, and the reason is worth writing down because the library got
// it wrong for exec first.
//
// Owning the exec path meant owning a poll loop, deadline arithmetic, pipe
// draining, signal escalation and fd lifetime -- in a library that also has
// a host doing all of that for its own purposes. Two implementations of one
// thing, and they drifted exactly as you would expect: one grew an absolute
// wall-clock ceiling and the other kept only an idle one, so a command that
// never stopped printing was never reaped in the configuration that shipped.
// The bug was not in either loop. It was in there being two.
//
// So: no loop here, no threads, no file descriptors, no OS headers. A tool
// says what to run and what it is willing to wait for; the host runs it on
// whatever its platform layer is (for agentty, jaal's reactor, clock and
// process capability) and hands back the bytes.
//
// NO DEFAULT, AND NO FALLBACK. A default implementation is a second
// implementation, which is the thing being removed. When `exec` is null the
// tools that need it (shell, diagnostics, git_*, process_*) are simply NOT
// ADVERTISED -- the same rule `code_retriever` already follows. That failure
// mode is the honest one: today a host that installs no sandbox still gets a
// working `shell`, it just runs unconfined. Absence you can see beats a
// capability that quietly degrades.
// A program to run. `exe` is separate from `args` so "argv is non-empty" is
// a property of the TYPE: there is no way to construct a request with no
// program, and so no boundary check to forget. argv[0] is synthesised from
// `exe` by the host.
struct Program {
    std::string              exe;
    std::vector<std::string> args;
};

// What a tool is willing to wait for.
//
// Two clocks, because they answer different questions and a caller almost
// always wants both. `idle` bounds SILENCE: a build that keeps printing is
// never cut off, however long it runs. `wall` bounds total elapsed time and
// never resets, which is the only thing that catches a runaway that stays
// chatty. nullopt is "host's default", not zero -- a budget of zero seconds
// is a meaningful thing to ask for and must not collide with "unset".
//
// Named fields rather than two positional `seconds`, because the one bug
// this shape invites is passing them the other way round.
struct Budgets {
    std::optional<std::chrono::seconds> idle;
    std::optional<std::chrono::seconds> wall;
};

struct ExecRequest {
    Program                  program;
    std::optional<std::string> cwd;        // nullopt ⇒ the host's choice
    std::vector<std::pair<std::string, std::string>> env;   // layered on top
    Budgets                  budgets;
    std::optional<std::size_t> max_output_bytes;

    // No progress sink and no cancellation probe in the request. Both
    // describe the tool CALL, not the program, and the call already carries
    // them: Exec::run takes the Call alongside this request.

    /// Stop once the output so far is enough. Called with everything
    /// captured to that point; true means stop.
    ///
    /// This IS a callback, and the distinction from the two above is the
    /// whole reason it is allowed: the host cannot know when enough is
    /// enough, because "enough" is a property of what the caller asked for.
    /// `grep` wanting its first N matches out of a repo-wide ripgrep is the
    /// case -- bounding bytes would not do it, since the predicate is about
    /// content, and waiting for exit throws away the latency win entirely.
    ///
    /// Empty is the common case and means "run to completion". The host
    /// checks before calling.
    std::function<bool(std::string_view)> stop_when;
};

// ── how it ended ────────────────────────────────────────────────────────
//
// A sum, not a bag of flags. The previous shape had five independent bools
// and an exit code that was present even when nothing had run: thirty-two
// representable states for five real ones, and the nonsense combinations
// ("never started, but hit the wall clock") were reachable by a typo.
//
// Each arm carries exactly what that outcome means and nothing else, so
// reading an exit code off a program that never started is not a mistake you
// can make -- it is a case you did not handle, and the compiler says so.

struct Exited      { int code = 0; };            ///< ran, returned this
struct Signalled   { int signal = 0; };          ///< killed by the OS
struct StartFailed { std::string reason; };      ///< never ran at all
struct Cancelled   {};                           ///< the host asked to stop

/// `stop_when` fired: the caller had what it needed and the rest was stopped.
/// A success, not a failure -- distinct from Signalled, which would be true
/// of the mechanism and misleading about the meaning.
struct StoppedEarly {};

/// Stopped by a budget. WHICH one matters: idle means "it hung, go look at
/// why", wall means "it was working fine and wants longer or a background
/// run". A single `timed_out` flag forces the caller to guess, and the two
/// deserve opposite advice.
struct TimedOut {
    enum class budget : std::uint8_t { idle, wall };
    budget which = budget::idle;
};

using ExecOutcome =
    std::variant<Exited, Signalled, StartFailed, TimedOut, Cancelled,
                 StoppedEarly>;

struct ExecResult {
    std::string output;        ///< stdout+stderr interleaved, UTF-8 valid
    ExecOutcome outcome;

    /// Beside the outcome, not inside it: truncation is ORTHOGONAL. A
    /// command can fill the cap and then exit 0, or fill it and then be
    /// killed. That it does not belong to any one arm is exactly why it is
    /// a field and the rest are cases.
    bool truncated = false;

    /// How long it ran, by the host's clock (the library reads none).
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] bool ok() const noexcept {
        const auto* e = std::get_if<Exited>(&outcome);
        return e && e->code == 0;
    }
};

/// A program that outlives the call which started it: a dev server, a file
/// watcher, a log tail.
///
/// The tool layer used to own these -- a thread per session draining a pipe
/// into a buffer, two mutexes guarding it, and a global map of them. That is
/// concurrency, which is the host's, and it was the last of it in here.
struct Session {
    virtual ~Session() = default;

    struct Update {
        /// Produced since the previous poll. Empty is normal and means
        /// "nothing new", not "finished" -- check `running` for that.
        std::string output;
        bool        running   = true;
        bool        truncated = false;
        /// Set exactly once, on the poll that observes the end.
        std::optional<ExecOutcome> outcome;
        /// How long it has been running, by the host's clock.
        std::chrono::seconds uptime{0};
    };

    /// Wait up to `wait` for new output, then return whatever there is.
    /// Blocking only for that long: a tool call must not hang on a server
    /// that has gone quiet, because quiet is the normal state of a server.
    [[nodiscard]] virtual Update poll(std::chrono::milliseconds wait) = 0;

    /// Ask it to stop, then insist. Idempotent.
    virtual void stop() = 0;
};

/// How a scan splits its work. mcp-cpp owns no threads: the scan tools
/// (grep, structural search, repo map, extract/aggregate) cut a file list
/// into shares and hand them to `run`, which the host supplies. Empty `run`
/// means inline.
struct Splitter {
    /// Run fn(i) for every i in [0, n) and return when all have finished.
    /// Calls may run concurrently; the tools are correct either way.
    std::function<void(std::size_t n, const std::function<void(std::size_t)>& fn)> run;
    /// How many shares are worth making. 1 means "run inline".
    std::size_t width = 1;
};

/// Run one program to completion. Blocking from the caller's point of view;
/// how the host achieves that is the host's business.
struct Exec {
    Exec()                       = default;
    Exec(const Exec&)            = delete;
    Exec& operator=(const Exec&) = delete;
    virtual ~Exec()              = default;

    /// `call` is the tool call this runs for: the host streams output to its
    /// progress sink and stops when it is cancelled.
    [[nodiscard]] virtual ExecResult run(const Call& call, const ExecRequest&) = 0;

    /// Start one and come back for it later. Same request type, because it
    /// is the same question asked with a different lifetime -- budgets still
    /// apply, and a session that goes silent past its idle budget is as dead
    /// as a command that does.
    [[nodiscard]] virtual std::expected<std::shared_ptr<Session>, std::string>
    start(const ExecRequest&) = 0;

    /// Can the host stop a whole process tree, or only the leader? A tool
    /// that reports what confinement is in force needs to ask rather than
    /// assume. (A descendant that calls setsid() escapes a process group.)
    [[nodiscard]] virtual bool stops_whole_tree() const noexcept = 0;
};

/// The host's Exec as one tool call sees it: run() carries that call, so its
/// output streams to the call's progress sink and stops when it is
/// cancelled. Tool bodies hold this, never the bare Exec. Lives on the
/// tool body's stack; neither pointer is owned.
class CallExec {
public:
    CallExec(Exec& exec, const Call& call) noexcept : exec_(&exec), call_(&call) {}
    [[nodiscard]] ExecResult run(const ExecRequest& r) { return exec_->run(*call_, r); }
    [[nodiscard]] std::expected<std::shared_ptr<Session>, std::string>
    start(const ExecRequest& r) { return exec_->start(r); }
    [[nodiscard]] bool stops_whole_tree() const noexcept { return exec_->stops_whole_tree(); }
    [[nodiscard]] const Call& call() const noexcept { return *call_; }
private:
    Exec*       exec_;
    const Call* call_;
};

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
    // Null ⇒ no shell / diagnostics / git_* / process_* tools. See Exec.
    std::shared_ptr<Exec>           exec;
    // How scans split their work; empty runs them inline.
    Splitter                        split;
    // What the tools remember between calls (workspace, caches, background
    // processes). Null ⇒ make_provider makes a SoleStateAccess, fine for a
    // host that runs one call at a time. See state.hpp.
    std::shared_ptr<StateAccess>    state;
};


} // namespace mcp::tools
