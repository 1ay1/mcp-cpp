// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/guarded.hpp — Guarded<T>: data OWNED BY its mutex.
//
//   The bug class this kills (observed in the field as a SIGSEGV): an object
//   whose methods take a mutex, but whose DESTRUCTOR does not. Thread A drops
//   the last reference and tears the object down while thread B is still
//   inside a method — the mutex protected every path except the one that
//   frees the data. The fix-by-discipline ("remember to lock in the dtor")
//   decays: the next subclass forgets, and the compiler has no opinion.
//
//   Guarded<T> makes the invariant structural, in the Mutex<T> shape Rust
//   uses (see docs/RUST-CRITIQUE.md #2 in the host repo):
//
//     • T is a PRIVATE member; the ONLY access path is use(f), which runs
//       f(T&) under the lock. Unlocked access is not a bug you can write —
//       it does not compile.
//     • The mutex is RECURSIVE: a callee invoked under use() may call a
//       method that itself calls use() on the same thread (e.g. a virtual
//       alive() reached from inside a locked execute()) without deadlocking.
//     • ~Guarded() acquires the lock, so destruction PARKS until every
//       thread already inside use() has drained. The dtor-vs-use race is
//       unrepresentable for any thread that entered before teardown began.
//       (A thread calling use() AFTER destruction starts is a plain C++
//       lifetime bug — keep a shared_ptr if callers can outlive the owner.)
//
#pragma once

#include <mutex>
#include <utility>

namespace mcp::cap {

template <class T>
class Guarded {
public:
    template <class... Args>
    explicit Guarded(Args&&... args) : value_(std::forward<Args>(args)...) {}

    Guarded(const Guarded&)            = delete;
    Guarded& operator=(const Guarded&) = delete;
    Guarded(Guarded&&)                 = delete;
    Guarded& operator=(Guarded&&)      = delete;

    ~Guarded() {
        // Park until every thread inside use() has left. This is the whole
        // point of the type: destruction cannot overtake a live access.
        std::lock_guard<std::recursive_mutex> lk(mu_);
    }

    // The sole access path: run f with the data, under the lock.
    template <class F>
    decltype(auto) use(F&& f) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return std::forward<F>(f)(value_);
    }
    template <class F>
    decltype(auto) use(F&& f) const {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return std::forward<F>(f)(value_);
    }

private:
    mutable std::recursive_mutex mu_;
    T value_;
};

} // namespace mcp::cap
