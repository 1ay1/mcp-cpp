// SPDX-License-Identifier: Apache-2.0
//
// mcp/runtime.hpp — what mcp-cpp needs from the program it runs in.
//
//   mcp-cpp is a library: it owns no thread. The engine is a state machine
//   (frames in, frames out, deadlines as values). Anything that has to WAIT
//   — reading a stream until it ends, waking at the nearest request deadline,
//   running a blocking call off the caller's thread — is asked of a Runtime
//   the host installs, and the host decides what thread it runs on.
//
//       mcp::set_runtime(my_runtime);      // once, at startup
//
//   agentty implements it on its own runtime. A standalone program that
//   installs nothing gets default_runtime(): plain std::thread, joined on
//   stop, good enough for an example or a test. It is the ONLY place in
//   mcp-cpp that names std::thread.
//
//   Jobs are cancellable: each gets a std::stop_token, and Job::stop()
//   requests it and waits for the body to return.
//
//   A body can block in a call no token can interrupt (a getline on a pipe
//   whose peer never closes it). Its owner should close the stream first so
//   it wakes. For when that fails, Job::stop(grace) waits only that long and
//   then gives the job up: the runtime keeps it alive somewhere it can't
//   hurt anyone and reclaims it later. The body must therefore not touch its
//   owner after a stop was requested — check the token after every wake.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

class Runtime {
public:
    virtual ~Runtime() = default;

    // A running background job. Destroying it MUST behave like stop():
    // request stop and wait for the body to return. Owners rely on that for
    // exception safety (an unwinding owner still joins its job).
    class Job {
    public:
        virtual ~Job() = default;
        // Wait for the body to return on its own, without asking it to stop.
        virtual void wait() noexcept = 0;
        // Request stop and wait for the body to return.
        virtual void stop() noexcept = 0;
        // Request stop, wait up to `grace`; return false if the body is still
        // running and was given up (see above).
        virtual bool stop(std::chrono::milliseconds grace) noexcept = 0;
    };

    // Run `body` in the background until it returns or is asked to stop.
    // `name` is for diagnostics only.
    [[nodiscard]] virtual std::unique_ptr<Job>
    spawn(const char* name, std::function<void(std::stop_token)> body) = 0;

    // Sleep up to `d`; return true if `st` was asked to stop meanwhile.
    virtual bool sleep_for(std::stop_token st, std::chrono::milliseconds d) = 0;

    // Run fn(i) for every i in [0, n) and return when all have finished.
    // Calls may run concurrently.
    virtual void parallel_for(std::size_t n,
                              const std::function<void(std::size_t)>& fn) = 0;
};

// The batteries-included fallback for standalone use: one std::jthread per
// job, stop requested and joined on Job::stop().
class ThreadRuntime final : public Runtime {
    class ThreadJob final : public Job {
    public:
        explicit ThreadJob(std::function<void(std::stop_token)> body)
            : t_([b = std::move(body), d = done_](std::stop_token st) {
                  b(st);
                  d->store(true, std::memory_order_release);
              }) {}
        ~ThreadJob() override { stop(); }
        void wait() noexcept override {
            if (t_.joinable() && t_.get_id() != std::this_thread::get_id())
                t_.join();
        }
        void stop() noexcept override {
            if (!t_.joinable()) return;
            t_.request_stop();
            if (t_.get_id() != std::this_thread::get_id()) t_.join();
            else t_.detach();   // a job stopping itself: let it unwind
        }
        bool stop(std::chrono::milliseconds grace) noexcept override {
            if (!t_.joinable()) return true;
            t_.request_stop();
            const auto until = std::chrono::steady_clock::now() + grace;
            while (!done_->load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (done_->load(std::memory_order_acquire)) { t_.join(); return true; }
            t_.detach();        // standalone fallback only; a host runtime
            return false;       // abandons through its own pool instead
        }
    private:
        std::shared_ptr<std::atomic<bool>> done_ =
            std::make_shared<std::atomic<bool>>(false);
        std::jthread t_;
    };
public:
    std::unique_ptr<Job> spawn(const char*,
                               std::function<void(std::stop_token)> body) override {
        return std::make_unique<ThreadJob>(std::move(body));
    }
    void parallel_for(std::size_t n,
                      const std::function<void(std::size_t)>& fn) override {
        if (n <= 1) { if (n) fn(0); return; }
        std::vector<std::jthread> ts;
        ts.reserve(n - 1);
        for (std::size_t i = 1; i < n; ++i) ts.emplace_back([&fn, i] { fn(i); });
        fn(0);
    }   // jthreads join here
    bool sleep_for(std::stop_token st, std::chrono::milliseconds d) override {
        if (st.stop_requested()) return true;
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lk(m);
        return cv.wait_for(lk, st, d, [] { return false; }), st.stop_requested();
    }
};

inline Runtime& default_runtime() {
    static ThreadRuntime rt;
    return rt;
}

namespace detail {
inline std::shared_ptr<Runtime>& installed_runtime() {
    static std::shared_ptr<Runtime> rt;
    return rt;
}
} // namespace detail

// Install the process runtime, once, before any engine or transport starts.
// set_runtime(nullptr) restores the default. Not for swapping while jobs run.
inline void set_runtime(std::shared_ptr<Runtime> rt) {
    detail::installed_runtime() = std::move(rt);
}

inline Runtime& runtime() {
    auto& rt = detail::installed_runtime();
    return rt ? *rt : default_runtime();
}

} // namespace mcp
