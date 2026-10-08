// SPDX-License-Identifier: Apache-2.0
//
// acp/coro.hpp — C++20 coroutine layer over the future-based API.
//
//   The engine returns std::future<T>. That is fine for blocking call sites,
//   but a modern client wants to write straight-line async code:
//
//       Task<void> drive(AgentConnection& a) {
//           auto init = co_await a.initialize(ip);
//           auto sess = co_await a.session_new({"."});
//           auto res  = co_await a.session_prompt(pp);
//           // res.stopReason ...
//       }
//
//   Two pieces make this work:
//
//     • make any std::future<T> awaitable  — `co_await some_future`
//     • Task<T>                            — a coroutine return type that is
//                                            itself awaitable, so tasks compose
//
//   The awaiter waits for the future on a job of the installed mcp::Runtime
//   and resumes the coroutine when the value arrives, so the awaiting thread
//   is freed, not spun, and this header starts no thread of its own.
//
//   Blocking bridge: Task::get() does NOT busy-spin on the coroutine state
//   (that races the resuming thread). It blocks on a completion flag that the
//   coroutine's final_suspend signals under a mutex — clean under TSan.
//
//   This header is OPTIONAL — it is not pulled in by <mcp/acp.hpp>. Include it
//   explicitly (<mcp/coro.hpp>) when you want the coroutine surface, so users
//   who don't use coroutines pay nothing for them.
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <mcp/runtime.hpp>
#include <utility>
#include <vector>

// NOTE: the coroutine machinery lives in `mcp::co` because the protocol has a
// `mcp::Task` struct (a durable-request record). `mcp::co::Task<T>` is the
// awaitable coroutine return type; they are unrelated.
namespace mcp::co {

//==============================================================================
//  FutureAwaiter — makes std::future<T> co_await-able.
//
//  A std::future can only be waited on by blocking, so something has to block
//  for it. That is a job on the installed mcp::Runtime (mcp/runtime.hpp), not
//  a thread this header starts: the host decides where waiting happens. The
//  job resumes the coroutine when the value arrives.
//
//  The resumed coroutine runs ON the job and may finish and free the awaiter,
//  so the job's handle can't live in the awaiter (it would be joined from its
//  own thread). It is kept in a small registry instead and reaped the next
//  time something awaits, once its body has returned.
//==============================================================================
namespace detail {
// Await jobs whose bodies have finished, kept until the next await reaps them.
struct AwaitJobs {
    struct Entry {
        std::unique_ptr<Runtime::Job>      job;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex         mu;
    std::vector<Entry> live;

    void add(std::unique_ptr<Runtime::Job> j, std::shared_ptr<std::atomic<bool>> d) {
        std::vector<Entry> finished;
        {
            std::lock_guard lk(mu);
            for (auto it = live.begin(); it != live.end(); )
                if (it->done->load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it));
                    it = live.erase(it);
                } else ++it;
            live.push_back({std::move(j), std::move(d)});
        }
        // `finished` joins here, outside the lock: each body already returned.
    }
};
inline AwaitJobs& await_jobs() { static AwaitJobs j; return j; }
} // namespace detail

template <class T>
struct SharedFutureAwaiter {
    std::shared_ptr<std::future<T>> fut;

    bool await_ready() const noexcept {
        // An already-resolved future needs no waiter at all.
        return fut->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    void await_suspend(std::coroutine_handle<> h) {
        auto f    = fut;
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto job  = runtime().spawn("mcp.co.await",
            [f, h, done](std::stop_token) mutable {
                f->wait();
                h.resume();   // may run the rest of the coroutine, and free us
                done->store(true, std::memory_order_release);
            });
        detail::await_jobs().add(std::move(job), std::move(done));
    }
    T await_resume() { return fut->get(); }
};

// operator co_await for any std::future<T>.
template <class T>
SharedFutureAwaiter<T> operator co_await(std::future<T>&& f) {
    return SharedFutureAwaiter<T>{std::make_shared<std::future<T>>(std::move(f))};
}

//==============================================================================
//  Completion — a shared, synchronized "the coroutine finished" signal that the
//  blocking Task::get() bridge waits on. Notified from final_suspend.
//==============================================================================
namespace detail {

struct Completion {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    void signal() {
        { std::lock_guard lk(mu); done = true; }
        cv.notify_all();
    }
    void wait() {
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return done; });
    }
};

template <class T>
struct TaskPromiseBase {
    std::coroutine_handle<> continuation{};            // who awaits us (or none)
    std::exception_ptr      error{};
    std::shared_ptr<Completion> completion{};          // set when blocking-driven

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            auto& pr = h.promise();
            // Capture everything we need BEFORE signalling: once signal() fires,
            // a blocked get() may destroy the coroutine frame, so the promise
            // must not be touched afterwards.
            std::coroutine_handle<> next = pr.continuation ? pr.continuation
                                                           : std::noop_coroutine();
            auto comp = pr.completion;
            if (comp) comp->signal();
            return next;
        }
        void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { error = std::current_exception(); }
};

} // namespace detail

//==============================================================================
//  Task<T> — a lazily-started, awaitable coroutine resolving to T (or void).
//==============================================================================
template <class T>
class Task {
public:
    struct promise_type : detail::TaskPromiseBase<T> {
        T value{};
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        template <class U>
        void return_value(U&& v) { value = std::forward<U>(v); }
    };

    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { destroy(); h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    bool await_ready() const noexcept { return !h_ || h_.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        h_.promise().continuation = awaiter;
        return h_;   // symmetric transfer into our body
    }
    T await_resume() {
        if (h_.promise().error) std::rethrow_exception(h_.promise().error);
        return std::move(h_.promise().value);
    }

    // Blocking entry point: run to completion and return the value. Installs a
    // Completion the coroutine's final_suspend signals, so we block (not spin)
    // until done — even when resumed from an awaiter's helper thread.
    T get() {
        auto comp = std::make_shared<detail::Completion>();
        h_.promise().completion = comp;
        h_.resume();
        comp->wait();
        if (h_.promise().error) std::rethrow_exception(h_.promise().error);
        return std::move(h_.promise().value);
    }

private:
    void destroy() { if (h_) h_.destroy(); h_ = {}; }
    std::coroutine_handle<promise_type> h_{};
};

// void specialisation.
template <>
class Task<void> {
public:
    struct promise_type : detail::TaskPromiseBase<void> {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void return_void() noexcept {}
    };

    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { destroy(); h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    bool await_ready() const noexcept { return !h_ || h_.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        h_.promise().continuation = awaiter;
        return h_;
    }
    void await_resume() {
        if (h_.promise().error) std::rethrow_exception(h_.promise().error);
    }

    void get() {
        auto comp = std::make_shared<detail::Completion>();
        h_.promise().completion = comp;
        h_.resume();
        comp->wait();
        if (h_.promise().error) std::rethrow_exception(h_.promise().error);
    }

private:
    void destroy() { if (h_) h_.destroy(); h_ = {}; }
    std::coroutine_handle<promise_type> h_{};
};

} // namespace mcp::co
