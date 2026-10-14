// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/state.hpp — what the builtin tools remember between calls, as
// one value the host owns.
//
//   The tools keep a little state across calls: the workspace boundary and
//   the extra roots a read may reach; what each reader has already been
//   shown (so a repeat read can answer "unchanged"); the last snapshot of
//   every file a tool read or wrote (so a write refuses to clobber a file
//   that changed underneath it); the background processes `process_start`
//   left running; the repo_map graphs.
//
//   None of it is a static. ToolState is plain data. The host decides how
//   it is shared: it implements StateAccess, whose with() runs a function on
//   the state with whatever exclusion the host uses (agentty: a
//   maya::guarded). Tool bodies never hold the state across I/O — they copy
//   out what they need, work, then write back.
//
//   Per-call inputs (whose read this is, has the caller given up) arrive
//   with the call (Call), not here.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mcp::tools {

struct Session;    // host.hpp
class  FileSystem; // files.hpp
struct Splitter;   // host.hpp

namespace state {

// repo_map's def/ref graph and its cache, kept in the tool state so a
// repeat call on an unchanged tree skips the parse. Plain data: everything
// the state reaches is visible here, so a host that guards the state can
// check destroying it never waits on a thread.
struct RepoDef {
    std::string name;
    int         line = 0;
    std::string signature;   // the trimmed defining line (≤ ~120 chars)
    int         inbound = 0; // distinct OTHER files referencing this name
    double      rank = 0.0;  // this symbol's share of its file's PageRank
};
struct RepoFile {
    std::string          rel;   // workspace-relative path
    std::vector<RepoDef> defs;
    // Names referenced here that are DEFINED in another file: outbound
    // edges, weighted by sqrt(#refs)/#definers.
    std::unordered_map<std::uint32_t, double> out_edges;   // file idx → weight
    double rank = 0.0;
};
struct RepoGraph {
    std::vector<RepoFile> files;
    std::uint64_t         signature = 0;   // Σ(size+mtime) stat fingerprint
};
// A few built graphs keyed by root, least recently used evicted.
struct RepoGraphCache {
    struct Slot {
        std::string                      root;
        std::shared_ptr<const RepoGraph> graph;
        std::uint64_t                    seq = 0;
    };
    std::vector<Slot> slots;
    std::uint64_t     seq = 0;
};

// A file as a tool last saw it.
struct FileSnapshot {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t                  size = 0;
    std::uint64_t                   content_hash = 0;
};

// What one reader still has of one file, from `read`.
struct ReadSeen {
    std::filesystem::file_time_type mtime{};
    int         offset = 1;   // first line of the range still in context
    int         limit  = 0;   // line count; 0 = whole file
    int         total  = 0;   // the file's length when read; 0 = unknown
    std::string content;      // the bytes served
};

// A background process started by process_start. Immutable once it is in
// the map: everything that changes (output, cursor, exit) lives in the
// host's Session, which is thread-safe by contract. So a tool can take the
// entry out of the map and use it without holding the state.
struct Proc {
    std::string                           id;
    std::string                           command;
    std::shared_ptr<tools::Session>       session;
};

}  // namespace state

struct ToolState {
    // The workspace boundary (canonical). Empty: the process cwd at first use.
    std::filesystem::path workspace_root;
    // Extra directories a read may reach (skills). Canonical, deduplicated.
    std::vector<std::filesystem::path> read_roots;
    // What `~` in a path argument expands to. The host fills it ($HOME, or
    // %USERPROFILE% on Windows); empty leaves `~` literal.
    std::filesystem::path home;
    // Appended to an out-of-workspace refusal: how THIS host lets a user
    // widen the boundary ("pass --workspace <dir>"). Empty says nothing,
    // since only the host knows its own flags.
    std::string widen_hint;

    // `read`'s memory, keyed (reader, canonical path).
    std::map<std::pair<std::string, std::string>, state::ReadSeen> reads;
    // The last snapshot of every file read or written, keyed by canonical path.
    std::unordered_map<std::string, state::FileSnapshot> files;

    // process_start's sessions. Entries are const: a tool takes one out and
    // drops the state before talking to its Session.
    std::unordered_map<std::string, std::shared_ptr<const state::Proc>> procs;
    static_assert(std::is_const_v<decltype(procs)::mapped_type::element_type>,
                  "a Proc in the map is immutable; mutable process state belongs "
                  "to the host's Session");
    unsigned long long next_proc = 1;

    // repo_map's built graphs (see state::RepoGraphCache).
    std::shared_ptr<state::RepoGraphCache> repo_graphs;

    // Is ripgrep on the host's PATH? Probed once, on first need.
    std::optional<bool> have_rg;
};

// How tool bodies reach the state. The host implements it.
struct StateAccess {
    StateAccess()                              = default;
    StateAccess(const StateAccess&)            = delete;
    StateAccess& operator=(const StateAccess&) = delete;
    virtual ~StateAccess()                     = default;

    // Run `f` on the state, excluding other with() calls for its duration.
    virtual void with(const std::function<void(ToolState&)>& f) = 0;
};

// For a host that runs one tool call at a time (tests, a single-threaded
// server): the state, with no exclusion at all.
struct SoleStateAccess final : StateAccess {
    ToolState state;
    void with(const std::function<void(ToolState&)>& f) override { f(state); }
};

// What one call carries besides its arguments.
struct Call {
    StateAccess*          state = nullptr;      // never null inside a tool body
    const Splitter*       splitter = nullptr;   // null: scans run inline
    FileSystem*           files = nullptr;      // the host's disk; see fs()
    std::string           reader;               // whose context a `read` serves
    std::function<bool()> cancelled;            // may be empty
    std::stop_token       stop;                 // the same cancel, as an event
    // Live output for the host to show while the call runs; may be empty.
    std::function<void(std::string_view)> progress;
    // The host's per-call data (cap::Request::host), handed back to the
    // host's own services. Never read here.
    const void*           host = nullptr;
    // Optional per-call scope, for a host serving sessions in different
    // folders (an ACP agent). Empty fields fall back to the shared state.
    std::filesystem::path              scope_workspace;
    std::vector<std::filesystem::path> scope_extra_roots;
    std::filesystem::path              scope_project;

    [[nodiscard]] bool cancel_requested() const { return cancelled && cancelled(); }

    // The host's filesystem. Every file a tool touches goes through it.
    [[nodiscard]] FileSystem& fs() const noexcept { return *files; }

    // A scan's parallel region: run fn(0..n-1) through the host's splitter, or
    // inline without one. width() is how many shares are worth making.
    void split(std::size_t n, const std::function<void(std::size_t)>& fn) const;
    [[nodiscard]] std::size_t width() const noexcept;

    // Run `f` on the state and return its result.
    template <class F>
    auto with(F&& f) const -> std::invoke_result_t<F&, ToolState&> {
        using R = std::invoke_result_t<F&, ToolState&>;
        if constexpr (std::is_void_v<R>) {
            state->with([&](ToolState& s) { f(s); });
        } else {
            std::optional<R> out;
            state->with([&](ToolState& s) { out.emplace(f(s)); });
            return std::move(*out);
        }
    }
};

}  // namespace mcp::tools
