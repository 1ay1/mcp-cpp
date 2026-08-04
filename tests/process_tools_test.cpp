// SPDX-License-Identifier: Apache-2.0
//
// process_tools_test.cpp — exercises the long-running-process tools
// (process_start / process_poll / process_stop) end-to-end through
// make_provider(). Focuses on the UX guarantees a model relies on:
//   • an immediate crash is reported AT START with its exit code + output,
//   • poll surfaces running-uptime vs. exited-with-code,
//   • poll returns incremental output since the previous poll,
//   • stop returns the exit code and a session-recovery hint on a bad id.
//
// POSIX-only: uses `sh -c`, `sleep`, `echo`, and exit codes.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

using namespace mcp::tools;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Pull the "proc-N" id out of a process_start result's text.
static std::string extract_id(const std::string& text) {
    auto pos = text.find("proc-");
    if (pos == std::string::npos) return {};
    auto end = pos;
    while (end < text.size()
           && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-'))
        ++end;
    return text.substr(pos, end - pos);
}

int main() {
    auto root = fs::temp_directory_path() / ("mcp_proc_test_" + std::to_string(::getpid()));
    fs::create_directories(root);
    util::set_workspace_root(root);

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── 1. immediate crash reported at START with exit code ──────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "echo boom >&2; exit 7";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        CHECK(contains(r.text, "exited immediately"));
        CHECK(contains(r.text, "exit 7"));
        CHECK(contains(r.text, "boom"));          // stderr captured
        // No session kept → polling its id must fail with a recovery hint.
        auto id = extract_id(r.text);
        if (!id.empty()) {
            mcp::Json p = mcp::Json::object(); p["id"] = id;
            auto pr = call(*provider, "process_poll", p);
            CHECK(pr.is_error);   // unknown session
        }
        std::puts("crash-at-start: ok");
    }

    // ── 2. running process → poll shows running + incremental output ─────
    {
        mcp::Json a = mcp::Json::object();
        // Emit one line immediately, one after a short delay, then live a bit.
        a["command"] = "echo first; sleep 0.4; echo second; sleep 2";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        CHECK(contains(r.text, "running"));
        auto id = extract_id(r.text);
        CHECK(!id.empty());
        // The 300ms settle window usually catches "first" in the start body.
        const bool first_at_start = contains(r.text, "first");

        // Poll with a wait long enough to catch "second".
        mcp::Json p = mcp::Json::object();
        p["id"] = id; p["wait_ms"] = 1500;
        auto pr = call(*provider, "process_poll", p);
        CHECK(!pr.is_error);
        CHECK(contains(pr.text, "running"));
        CHECK(contains(pr.text, "second"));
        if (!first_at_start) CHECK(contains(pr.text, "first"));
        std::puts("running + incremental poll: ok");

        // Stop it: still running → "Stopped", exit code present (signal path).
        mcp::Json s = mcp::Json::object(); s["id"] = id;
        auto sr = call(*provider, "process_stop", s);
        CHECK(!sr.is_error);
        CHECK(contains(sr.text, "Stopped"));
        // Second stop of the same id → unknown, with a live-sessions hint.
        auto sr2 = call(*provider, "process_stop", s);
        CHECK(sr2.is_error);
        std::puts("stop running: ok");
    }

    // ── 3. process exits on its own → poll reports exit code ─────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "echo hello; sleep 0.5; exit 3";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        CHECK(contains(r.text, "running"));   // still alive at 300ms
        auto id = extract_id(r.text);
        CHECK(!id.empty());

        mcp::Json p = mcp::Json::object();
        p["id"] = id; p["wait_ms"] = 1500;
        // Poll may return as soon as "hello" arrives (before the 0.5s exit) —
        // that's the honest incremental contract. Keep polling until the
        // session reports it exited, exactly as a model would.
        std::string acc;
        bool saw_exit = false;
        // "hello" may already appear in the start result (drained during the
        // 300ms settle window) rather than in a poll — count both.
        acc += r.text;
        for (int i = 0; i < 10 && !saw_exit; ++i) {
            auto pr = call(*provider, "process_poll", p);
            CHECK(!pr.is_error);
            acc += pr.text;
            if (contains(pr.text, "exited")) saw_exit = true;
        }
        CHECK(saw_exit);
        CHECK(contains(acc, "hello"));
        CHECK(contains(acc, "exit 3"));
        std::puts("exit-code on poll: ok");

        // Reaping an already-exited session → "Reaped" + code.
        mcp::Json s = mcp::Json::object(); s["id"] = id;
        auto sr = call(*provider, "process_stop", s);
        CHECK(!sr.is_error);
        CHECK(contains(sr.text, "exit 3"));
        std::puts("reap exited: ok");
    }

    // ── 4. bad id gives a recovery hint, never a crash ───────────────────
    {
        mcp::Json p = mcp::Json::object(); p["id"] = "proc-does-not-exist";
        auto pr = call(*provider, "process_poll", p);
        CHECK(pr.is_error);
        CHECK(contains(pr.text, "unknown process session")
              || contains(pr.text, "does-not-exist"));
        std::puts("bad-id hint: ok");
    }

    fs::remove_all(root);
    if (g_failures == 0) std::puts("ALL PASS");
    else std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return g_failures ? 1 : 0;
}
