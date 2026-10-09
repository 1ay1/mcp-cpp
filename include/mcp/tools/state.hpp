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
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mcp::tools {

struct Session;    // host.hpp
struct Executor;   // host.hpp

namespace state {

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

// A background process started by process_start.
struct Proc {
    std::string id;
    std::string command;
    std::shared_ptr<tools::Session>       proc;
    std::string                           output;          // rolling window
    std::size_t                           output_base = 0; // offset of output[0]
    std::size_t                           delivered = 0;   // read up to here
    std::size_t                           dropped_unseen = 0;
    std::optional<int>                    exit_code;
    std::chrono::seconds                  uptime{0};   // as the host last reported
    bool                                  running = true;
    bool                                  stopped = false;
    bool                                  eof = false;      // no more output will come
};

}  // namespace state

struct ToolState {
    // The workspace boundary (canonical). Empty: the process cwd at first use.
    std::filesystem::path workspace_root;
    // Extra directories a read may reach (skills). Canonical, deduplicated.
    std::vector<std::filesystem::path> read_roots;

    // `read`'s memory, keyed (reader, canonical path).
    std::map<std::pair<std::string, std::string>, state::ReadSeen> reads;
    // The last snapshot of every file read or written, keyed by canonical path.
    std::unordered_map<std::string, state::FileSnapshot> files;

    // process_start's sessions.
    std::unordered_map<std::string, std::shared_ptr<state::Proc>> procs;
    unsigned long long next_proc = 1;

    // repo_map's built graphs, opaque to everyone else.
    std::shared_ptr<void> repo_graphs;

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
    Executor*             executor = nullptr;   // null: scans run inline
    std::string           reader;               // whose context a `read` serves
    std::function<bool()> cancelled;            // may be empty

    [[nodiscard]] bool cancel_requested() const { return cancelled && cancelled(); }

    // A scan's parallel region: run fn(0..n-1) on the host's executor, or
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
