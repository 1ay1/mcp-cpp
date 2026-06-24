// SPDX-License-Identifier: Apache-2.0
//
// scheduler_test.cpp — effect-aware parallel tool scheduling (cap/scheduler).
//
//   Two halves:
//     • plan_waves — the PURE planner. Drives hand-built CallFacts through the
//       conflict model and asserts the wave assignment is correct, order-
//       preserving, and conservative (Exec / blind-writer serialise).
//     • run — the executor. A LocalProvider whose handlers sleep proves the
//       wave actually runs CONCURRENTLY (wall-clock ≈ one sleep, not N) and
//       returns results 1:1 with the input batch.
//
#include <mcp/cap/cap.hpp>
#include <mcp/cap/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace mcp;
using namespace mcp::cap;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << "\n";                                      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

CallFacts cf(Effects e, std::vector<std::string> paths = {}) {
    return CallFacts{e, std::move(paths)};
}

// Which wave did original index i land in? (-1 if absent.)
int wave_of(const Plan& p, std::size_t i) {
    for (std::size_t w = 0; w < p.waves.size(); ++w)
        for (std::size_t j : p.waves[w])
            if (j == i) return static_cast<int>(w);
    return -1;
}

// ── (a) All pure reads → ONE wave (full parallelism). ──────────────────────
void test_all_reads_one_wave() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::ReadFs}, {"a.c"}),
        cf(Effects{Eff::ReadFs}, {"b.c"}),
        cf(Effects{Eff::Net}),                 // web_fetch-like
        cf(Effects{Eff::ReadFs}, {"a.c"}),     // same path as #0 — reads don't conflict
    };
    auto p = plan_waves(f);
    CHECK(p.wave_count() == 1);
    CHECK(p.waves[0].size() == 4);
    CHECK(!p.is_fully_serial());
}

// ── (b) A write serialises against an overlapping read; disjoint reads still
//        parallelise alongside. ─────────────────────────────────────────────
void test_write_overlap_serialises() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::ReadFs},  {"src/a.c"}),   // 0
        cf(Effects{Eff::ReadFs},  {"src/b.c"}),   // 1
        cf(Effects{Eff::WriteFs}, {"src/a.c"}),   // 2 — conflicts with 0
    };
    auto p = plan_waves(f);
    // 0 and 1 share wave 0; 2 is forced into a later wave (conflicts with 0).
    CHECK(wave_of(p, 0) == 0);
    CHECK(wave_of(p, 1) == 0);
    CHECK(wave_of(p, 2) == 1);
}

// ── (c) Disjoint writes parallelise. ───────────────────────────────────────
void test_disjoint_writes_parallel() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::WriteFs}, {"a.c"}),
        cf(Effects{Eff::WriteFs}, {"b.c"}),
    };
    auto p = plan_waves(f);
    CHECK(p.wave_count() == 1);   // no overlap → same wave
}

// ── (d) Directory-prefix overlap: writing "src/" conflicts with reading
//        "src/deep/x.c". ──────────────────────────────────────────────────
void test_dir_prefix_overlap() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::WriteFs}, {"src"}),          // 0
        cf(Effects{Eff::ReadFs},  {"src/deep/x.c"}), // 1 — under src/
        cf(Effects{Eff::ReadFs},  {"other/y.c"}),    // 2 — disjoint
    };
    auto p = plan_waves(f);
    CHECK(wave_of(p, 0) != wave_of(p, 1));   // 0 vs 1 conflict
    // "srcfoo" must NOT be considered under "src" (boundary check); 2 is free.
    CHECK(wave_of(p, 2) == 0);               // joins the first clash-free wave
}

void test_no_spurious_prefix() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::WriteFs}, {"src"}),       // 0
        cf(Effects{Eff::WriteFs}, {"srcfoo"}),    // 1 — NOT under "src"
    };
    auto p = plan_waves(f);
    CHECK(p.wave_count() == 1);   // no real overlap → parallel
}

// ── (e) Exec serialises against EVERYTHING. ────────────────────────────────
void test_exec_serialises_all() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::ReadFs}, {"a.c"}),                       // 0
        cf(Effects{Eff::Exec, Eff::ReadFs, Eff::WriteFs}, {}),   // 1 — bash
        cf(Effects{Eff::ReadFs}, {"b.c"}),                       // 2
    };
    auto p = plan_waves(f);
    // 1 conflicts with everything, so it gets its own wave; 0 and 2 are reads
    // but 2 comes after the exec — first-fit puts 0 in wave0, 1 in wave1
    // (clashes with 0), 2 in wave0 (clash-free with 0, the only wave0 member).
    CHECK(wave_of(p, 1) != wave_of(p, 0));
    CHECK(wave_of(p, 1) != wave_of(p, 2));
    CHECK(p.wave_count() == 2);
}

// ── (f) A blind writer (writes, no extractable path) serialises against all
//        fs-touching peers, but a pure-Net peer still parallelises. ──────────
void test_blind_writer() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::ReadFs},  {"a.c"}),   // 0 fs
        cf(Effects{Eff::WriteFs}, {}),        // 1 blind writer
        cf(Effects{Eff::Net}),                // 2 pure net — no fs
    };
    auto p = plan_waves(f);
    CHECK(wave_of(p, 0) != wave_of(p, 1));    // blind writer vs fs read
    CHECK(wave_of(p, 2) == wave_of(p, 0));    // net peer shares the read's wave
}

// ── (g) Order preservation: a later-emitted conflicting call NEVER lands in
//        an earlier wave than the call it conflicts with. ──────────────────
void test_order_preserved() {
    std::vector<CallFacts> f = {
        cf(Effects{Eff::WriteFs}, {"x"}),   // 0
        cf(Effects{Eff::ReadFs},  {"x"}),   // 1 — must come AFTER 0
    };
    auto p = plan_waves(f);
    CHECK(wave_of(p, 0) < wave_of(p, 1));
}

// ── (h) Built-in path extraction pulls the right keys. ──────────────────────
void test_extract_paths() {
    CHECK(extract_paths("read",  Json{{"path","foo.c"}})       == std::vector<std::string>{"foo.c"});
    CHECK(extract_paths("write", Json{{"file_path","bar.c"}})  == std::vector<std::string>{"bar.c"});
    CHECK(extract_paths("grep",  Json{{"dir","src"}})          == std::vector<std::string>{"src"});
    CHECK(extract_paths("glob",  Json{{"pattern","*.c"}}).empty());   // no dir ⇒ whole tree
}

// ── (i) Executor: a parallel wave runs concurrently (wall-clock proves it),
//        and results come back 1:1 with the batch in original order. ─────────
void test_concurrent_execution() {
    auto p = std::make_shared<LocalProvider>("slow");
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    auto handler = [&](const Json& a) -> Result {
        int now = ++in_flight;
        int prev = max_in_flight.load();
        while (now > prev && !max_in_flight.compare_exchange_weak(prev, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        --in_flight;
        return Result::ok("done:" + a.value("path", std::string{}));
    };
    // Four pure reads of distinct paths — one wave, all concurrent.
    p->add("read", "read", Json{{"type","object"}}, handler);
    Registry reg;
    reg.add(p);

    std::vector<Request> batch = {
        Request{"read", Json{{"path","a"}}},
        Request{"read", Json{{"path","b"}}},
        Request{"read", Json{{"path","c"}}},
        Request{"read", Json{{"path","d"}}},
    };
    // EffectFn: everything is a pure read with the named path.
    EffectFn fn = [](const Request& r) {
        return CallFacts{Effects{Eff::ReadFs}, extract_paths("read", r.args)};
    };

    auto t0 = std::chrono::steady_clock::now();
    auto results = run(reg, batch, fn);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK(results.size() == 4);
    CHECK(results[0].text == "done:a");
    CHECK(results[1].text == "done:b");
    CHECK(results[2].text == "done:c");
    CHECK(results[3].text == "done:d");
    // Concurrency proof: all four ran together (>=2 in flight at the peak;
    // expect 4) and wall-clock is far under the 160ms serial sum.
    CHECK(max_in_flight.load() >= 2);
    CHECK(elapsed < 120);
    std::cerr << "  [concurrent] peak in-flight=" << max_in_flight.load()
              << " elapsed=" << elapsed << "ms (serial would be ~160ms)\n";
}

// ── (j) Executor preserves correctness when forced serial (overlapping
//        writes run one at a time, in order). ────────────────────────────────
void test_serial_path_correct() {
    auto p = std::make_shared<LocalProvider>("w");
    std::string log;
    p->add("edit", "edit", Json{{"type","object"}},
           [&](const Json& a) {
               log += a.value("tag", std::string{});
               return Result::ok("ok");
           });
    Registry reg;
    reg.add(p);
    // All target the SAME path → fully serial, in submission order. (`tag`
    // distinguishes them in the log; `path` is identical so they conflict.)
    std::vector<Request> batch = {
        Request{"edit", Json{{"path","same.c"},{"tag","X"}}},
        Request{"edit", Json{{"path","same.c"},{"tag","Y"}}},
        Request{"edit", Json{{"path","same.c"},{"tag","Z"}}},
    };
    EffectFn fn = [](const Request& r) {
        return CallFacts{Effects{Eff::WriteFs}, extract_paths("edit", r.args)};
    };
    auto plan = plan_waves(batch, fn);
    CHECK(plan.is_fully_serial());
    auto results = run(reg, batch, fn);
    CHECK(results.size() == 3);
    CHECK(log == "XYZ");   // strict submission order preserved
}

} // namespace

int main() {
    test_all_reads_one_wave();
    test_write_overlap_serialises();
    test_disjoint_writes_parallel();
    test_dir_prefix_overlap();
    test_no_spurious_prefix();
    test_exec_serialises_all();
    test_blind_writer();
    test_order_preserved();
    test_extract_paths();
    test_concurrent_execution();
    test_serial_path_correct();

    if (g_failures == 0) std::cout << "scheduler_test: all checks passed\n";
    else std::cerr << "scheduler_test: " << g_failures << " FAILURES\n";
    return g_failures == 0 ? 0 : 1;
}
