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

#include "test_state.hpp"
#include "test_exec.hpp"
#include <mcp/tools/native_files.hpp>
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
#include <vector>
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
    auto ws_state = mcp::test::state_at(root);

    HostServices svc;

    svc.files = std::make_shared<NativeFileSystem>();

    svc.state = ws_state;

    svc.exec  = std::make_shared<mcp::test::PopenExec>();
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

    // ── 1b. FAST clean exit (exit 0) is not mislabeled as a failure ──────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "echo quickresult; exit 0";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        CHECK(contains(r.text, "exited immediately"));
        CHECK(contains(r.text, "quickresult"));    // output preserved
        // Exit 0 is SUCCESS — the guidance must NOT tell the model to "fix"
        // a command that worked; it should point at bash for one-shot runs.
        CHECK(contains(r.text, "cleanly"));
        CHECK(!contains(r.text, "Fix the command"));
        std::puts("clean-exit-0: ok");
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

    // ── 4. dying breath: final line + exit land in ONE poll ─────────────
    // A server that prints its fatal error and dies mid-poll must deliver
    // that tail to the SAME poll (the reader-EOF drain), not report a bare
    // "exited" with the error swallowed. Before the reader_finished() fix,
    // poll broke the moment the child died — racing the reader thread.
    {
        mcp::Json a = mcp::Json::object();
        // Quiet through the 300ms settle window, then one last line + death.
        a["command"] = "sleep 0.6; echo FATAL-BIND-FAIL; exit 9";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        CHECK(contains(r.text, "running"));
        auto id = extract_id(r.text);
        CHECK(!id.empty());

        mcp::Json p = mcp::Json::object();
        p["id"] = id; p["wait_ms"] = 2000;   // spans the exit
        auto pr = call(*provider, "process_poll", p);
        CHECK(!pr.is_error);
        // The single poll that observes death must carry the dying words.
        CHECK(contains(pr.text, "FATAL-BIND-FAIL"));
        CHECK(contains(pr.text, "exited"));
        std::puts("dying-breath tail in one poll: ok");

        mcp::Json s = mcp::Json::object(); s["id"] = id;
        auto sr = call(*provider, "process_stop", s);
        CHECK(!sr.is_error);
    }

    // ── 5. grandchild keeps the pipe open → poll says so ────────────────
    // The shell exits but a backgrounded child inherits the write end: the
    // pipe has no EOF, so "no further output" would be a lie. Poll must
    // distinguish this from the fully-drained case.
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "(sleep 3; echo late) & exit 0";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        auto id = extract_id(r.text);
        // The shell may exit inside the settle window (session dropped, exit 0
        // clean path) — only probe the open-pipe hint when a session survives.
        if (!contains(r.text, "exited immediately") && !id.empty()) {
            mcp::Json p = mcp::Json::object();
            p["id"] = id; p["wait_ms"] = 1200;   // < the 3s grandchild sleep
            auto pr = call(*provider, "process_poll", p);
            CHECK(!pr.is_error);
            if (contains(pr.text, "exited") && !contains(pr.text, "late"))
                CHECK(contains(pr.text, "pipe is still open"));
            mcp::Json s = mcp::Json::object(); s["id"] = id;
            (void)call(*provider, "process_stop", s);
        }
        std::puts("grandchild-held pipe: ok");
    }

    // ── 6. bad id gives a recovery hint, never a crash ───────────────────
    {
        mcp::Json p = mcp::Json::object(); p["id"] = "proc-does-not-exist";
        auto pr = call(*provider, "process_poll", p);
        CHECK(pr.is_error);
        CHECK(contains(pr.text, "unknown process session")
              || contains(pr.text, "does-not-exist"));
        std::puts("bad-id hint: ok");
    }

    // ── 7. a large wait_ms (past the OLD 5 s cap) is honoured and STILL
    //       returns the instant output arrives — not after the full wait.
    //       Regression: wait_ms used to clamp to 5000, so a poll meant to
    //       block on a slow build returned early every time and the model
    //       had to busy-spin.
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "sleep 0.4; echo delayed-line";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        auto id = extract_id(r.text);
        CHECK(!id.empty());

        mcp::Json p = mcp::Json::object();
        p["id"] = id;
        p["wait_ms"] = 60000;   // 1 min: would be clamped to 5 s before the fix
        const auto t0 = std::chrono::steady_clock::now();
        auto pr = call(*provider, "process_poll", p);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(!pr.is_error);
        CHECK(contains(pr.text, "delayed-line"));   // got the output
        CHECK(elapsed < 5000);                        // returned when it arrived, not at the deadline
        std::puts("large wait_ms early-return: ok");
    }

    // ── 8. two polls on one id at once get disjoint output ──────────────────────
    //    The old design had a shared rolling buffer and a cursor that two
    //    calls could both read before either advanced it. Each line must be
    //    returned exactly once across the two pollers.
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "i=0; while [ $i -lt 400 ]; do echo L$i; i=$((i+1)); sleep 0.002; done";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        auto id = extract_id(r.text);
        CHECK(!id.empty());
        std::string seen_a, seen_b;
        auto poller = [&](std::string& out) {
            for (int k = 0; k < 2000; ++k) {
                mcp::Json p = mcp::Json::object();
                p["id"] = id; p["wait_ms"] = 50;
                auto pr = call(*provider, "process_poll", p);
                if (pr.is_error) break;
                out += pr.text + "\n";
                if (contains(pr.text, "exited")) break;
            }
        };
        std::thread t1(poller, std::ref(seen_a)), t2(poller, std::ref(seen_b));
        t1.join(); t2.join();
        int dup = 0, missing = 0;
        for (int i = 0; i < 400; ++i) {
            const std::string line = "\nL" + std::to_string(i) + "\n";
            const bool in_a = ("\n" + seen_a).find(line) != std::string::npos;
            const bool in_b = ("\n" + seen_b).find(line) != std::string::npos;
            if (in_a && in_b) ++dup;
            if (!in_a && !in_b) ++missing;
        }
        // Only the first lines may be missing: process_start's settle window
        // takes them. Every other line is returned exactly once.
        CHECK(dup == 0);
        CHECK(missing < 100);
        if (missing >= 100 || dup)
            std::fprintf(stderr, "dup=%d missing=%d\n", dup, missing);
        mcp::Json s = mcp::Json::object(); s["id"] = id;
        (void)call(*provider, "process_stop", s);
        std::puts("concurrent polls disjoint: ok");
    }

    // ── 9. poll and stop racing on one id ──────────────────────────────────────
    //    Either order is fine; neither may crash, hang, or leave the id half
    //    alive. After both, the id is gone.
    for (int round = 0; round < 8; ++round) {
        mcp::Json a = mcp::Json::object();
        a["command"] = "while :; do echo tick; sleep 0.01; done";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        auto id = extract_id(r.text);
        std::thread poll_t([&] {
            mcp::Json p = mcp::Json::object();
            p["id"] = id; p["wait_ms"] = 200;
            (void)call(*provider, "process_poll", p);
        });
        std::thread stop_t([&] {
            mcp::Json s = mcp::Json::object(); s["id"] = id;
            (void)call(*provider, "process_stop", s);
        });
        poll_t.join(); stop_t.join();
        mcp::Json p = mcp::Json::object(); p["id"] = id;
        CHECK(call(*provider, "process_poll", p).is_error);   // gone
    }
    std::puts("poll/stop race: ok");

    // ── 10. stop is quick, even for a program that ignores SIGTERM ────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "trap '' TERM; while :; do sleep 0.05; done";
        auto r = call(*provider, "process_start", a);
        CHECK(!r.is_error);
        auto id = extract_id(r.text);
        const auto t0 = std::chrono::steady_clock::now();
        mcp::Json s = mcp::Json::object(); s["id"] = id;
        auto sr = call(*provider, "process_stop", s);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(!sr.is_error);
        CHECK(ms < 5000);   // bounded by the TERM->KILL grace, never a hang
        std::puts("stop bounded: ok");
    }

    // ── 11. many sessions started and dropped while others poll ──────────────
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                for (int k = 0; k < 6; ++k) {
                    mcp::Json a = mcp::Json::object();
                    a["command"] = "echo x; sleep 0.5";
                    auto r = call(*provider, "process_start", a);
                    auto id = extract_id(r.text);
                    if (id.empty()) continue;
                    mcp::Json p = mcp::Json::object();
                    p["id"] = id; p["wait_ms"] = 20;
                    (void)call(*provider, "process_poll", p);
                    mcp::Json s = mcp::Json::object(); s["id"] = id;
                    (void)call(*provider, "process_stop", s);
                }
            });
        for (auto& t : ts) t.join();
        std::puts("many sessions: ok");
    }

    fs::remove_all(root);
    if (g_failures == 0) std::puts("ALL PASS");
    else std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return g_failures ? 1 : 0;
}
