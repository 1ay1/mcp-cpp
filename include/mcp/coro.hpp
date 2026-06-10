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
//   The awaiter blocks a detached helper thread on the future and resumes the
//   coroutine when the value arrives. That keeps this header self-contained
//   (no scheduler, no executor dependency) while giving real suspension: the
//   awaiting thread is freed, not spun. For a single-threaded event loop you
//   can swap in your own awaiter; this is the batteries-included default.
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

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// NOTE: the coroutine machinery lives in `mcp::co` because the protocol has a
// `mcp::Task` struct (a durable-request record). `mcp::co::Task<T>` is the
// awaitable coroutine return type; they are unrelated.
namespace mcp::co {

//==============================================================================
//  FutureAwaiter — makes std::future<T> co_await-able.
//==============================================================================
template <class T>
struct FutureAwaiter {
    std::future<T> fut;

    bool await_ready() const noexcept {
        // Treat an already-resolved future as ready so we don't spawn a thread.
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    void await_suspend(std::coroutine_handle<> h) {
        std::thread([f = std::move(fut), h]() mutable {
            f.wait();
            // Stash the resolved future back where await_resume can read it.
            // (We move it into a heap cell the handle owns via the awaiter; but
            //  since the awaiter object outlives suspension, just re-assign.)
            h.resume();
        }).detach();
    }
    T await_resume() { return fut.get(); }
};

// Because await_suspend moves `fut` out, keep the awaiter alive across the
// suspension by NOT moving — instead block in a helper that holds a shared_ptr
// to the future. Simpler: specialise the operator below to wrap in shared_ptr.
template <class T>
struct SharedFutureAwaiter {
    std::shared_ptr<std::future<T>> fut;
    bool await_ready() const noexcept {
        return fut->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    void await_suspend(std::coroutine_handle<> h) {
        auto f = fut;
        std::thread([f, h]() mutable { f->wait(); h.resume(); }).detach();
    }
    T await_resume() { return fut->get(); }
};

// operator co_await for any std::future<T>. Wraps in a shared cell so the
// future outlives the suspension regardless of awaiter lifetime.
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
