// SPDX-License-Identifier: Apache-2.0
//
// mcp/rpc.hpp — JSON-RPC 2.0, lifted into the codec algebra.
//
//   The wire is a tagged sum:
//
//     RpcMessage  ≅  Request      { id; method; params }
//                 +  Notification {     method; params }
//                 +  Response     { id; (result | error) }
//
//   (`result | error` is itself a sum; we model it as `Sum<Json, RpcError>`.)
//
//   The engine is two coroutine-like things glued by an in-flight table:
//
//     • inbound  : bytes →  RpcMessage  →  dispatch
//     • outbound : either notify(method, params)             (fire-and-forget)
//                    or    request(method, params) -> future<Json>
//
//   Transport is abstracted by a single function value:
//
//       using Transport = std::function<void(std::string_view utf8_no_newlines)>;
//
//   See mcp/stdio.hpp for the canonical stdio transport.
//
#pragma once

#include <mcp/codec.hpp>


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

namespace mcp {

//==============================================================================
//  Error codes (standard JSON-RPC 2.0 + a custom slot for auth).
//==============================================================================
namespace errc {
// Standard JSON-RPC 2.0 codes (schema.ts: PARSE_ERROR … INTERNAL_ERROR).
inline constexpr int ParseError     = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams  = -32602;
inline constexpr int InternalError  = -32603;
// MCP-specific: a URL-mode elicitation is required to proceed (schema.ts
// URL_ELICITATION_REQUIRED).
inline constexpr int UrlElicitationRequired = -32042;
// Engine-local synthetic codes (never sent by a conforming peer).
inline constexpr int Timeout        = -32001;   // request deadline exceeded
inline constexpr int Cancelled      = -32002;   // request cancelled locally
inline constexpr int ConnectionLost = -32003;   // transport closed mid-flight
} // namespace errc

//==============================================================================
//  RpcError — both an exception (thrown out of handlers / awaited futures) and
//             a value type (sent on the wire).
//==============================================================================
struct RpcError : std::runtime_error {
    int  code;
    Json data;
    RpcError(int c, std::string msg, Json d = Json())
        : std::runtime_error(std::move(msg)), code(c), data(std::move(d)) {}
};
template <> struct CodecOf<RpcError> {
    static Codec<RpcError> get() {
        return {
            [](const RpcError& e) -> Json {
                Json j = Json{{"code", e.code}, {"message", std::string(e.what())}};
                if (!e.data.is_null()) j["data"] = e.data;
                return j;
            },
            [](const Json& j) -> RpcError {
                int code = j.value("code", errc::InternalError);
                std::string msg = j.value("message", std::string{"rpc error"});
                Json data = j.value("data", Json());
                return RpcError(code, std::move(msg), std::move(data));
            }};
    }
};

//==============================================================================
//  Transport — an opaque sink that accepts a single UTF-8 string with NO
//              embedded newlines (per the stdio spec). The transport itself
//              is responsible for framing (e.g. appending '\n' before writing).
//==============================================================================
using Transport = std::function<void(std::string_view)>;

//==============================================================================
//  Observability hooks.
//
//      WireTrace   — called for every frame crossing the boundary, in both
//                    directions, with the raw JSON text. Wire it to a logger
//                    to debug a protocol exchange. Never throws into the engine
//                    (the engine swallows exceptions from the hook).
//
//      ErrorCallback — called when the engine detects a transport-level or
//                    dispatch-level fault that isn't tied to a single in-flight
//                    request (e.g. the reader thread hit EOF or an exception).
//                    A clean EOF reports errc::ConnectionLost with "eof".
//==============================================================================
enum class WireDir { Inbound, Outbound };
using WireTrace     = std::function<void(WireDir, std::string_view)>;
using ErrorCallback = std::function<void(int code, std::string_view message)>;

//==============================================================================
//  RpcEngine — bidirectional JSON-RPC dispatcher.
//
//      Handler registration is partitioned by whether the message has an id:
//
//        on_request(method, h)        h : Json → Json | throws RpcError
//        on_notification(method, h)   h : Json → ()   (any throw is swallowed)
//
//      Typed overloads use the codec algebra: the handler signature is
//
//        on<Params, Result>(method, h)             where h : Params → Result
//        on_note<Params>   (method, h)             where h : Params → ()
//
//      Outbound:
//
//        notify<Params>(method, params)            fire-and-forget
//        request<Result, Params>(method, params)   → std::future<Result>
//
//      Engine is thread-safe across handler registration, dispatch, and
//      outbound calls. A single reader thread should call `feed_line` for each
//      received frame; outbound calls may happen from any thread.
//==============================================================================
class RpcEngine {
public:
    explicit RpcEngine(Transport write) : write_(std::move(write)) {}

    ~RpcEngine() { stop_timer(); }

    RpcEngine(const RpcEngine&)            = delete;
    RpcEngine& operator=(const RpcEngine&) = delete;

    // ---------------------------------------------------------- observability
    // Install a wire tracer (every inbound/outbound frame) and/or an error
    // callback (transport faults not tied to one request). Both are optional.
    void set_wire_trace(WireTrace t)    { std::lock_guard lk(mu_); trace_ = std::move(t); }
    void set_error_callback(ErrorCallback e) { std::lock_guard lk(mu_); on_error_ = std::move(e); }

    // Default deadline applied to every typed/raw request that doesn't pass an
    // explicit timeout. Zero (the default) means "wait forever".
    void set_default_timeout(std::chrono::milliseconds d) {
        default_timeout_.store(d.count(), std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- handlers
    //
    //   A request handler returns Maybe<Json>:
    //     • Just(result)  → the engine writes {result} synchronously (the
    //                        common, fully-synchronous case).
    //     • Nothing       → the handler took ownership of the reply and will
    //                        send it later from any thread via respond_raw()
    //                        / respond_error_raw() using the captured RpcId.
    //
    //   The deferred path is what lets a long-running handler (e.g. an agent
    //   driving a whole turn) hand the work to a worker thread WITHOUT blocking
    //   the reader thread — so the engine stays free to read the responses to
    //   any outbound requests (request_permission, fs/*, terminal/*) the turn
    //   makes. Blocking inline would deadlock a single-reader transport.
    using RawRequest      = std::function<Maybe<Json>(const RpcId&, const Json&)>;
    using RawNotification = std::function<void(const Json&)>;

    void on_request(std::string method, RawRequest h) {
        std::lock_guard lk(mu_);
        requests_[std::move(method)] = std::move(h);
    }
    void on_notification(std::string method, RawNotification h) {
        std::lock_guard lk(mu_);
        notifications_[std::move(method)] = std::move(h);
    }

    // Typed registrations: the codec algebra handles ser/de at the boundary.
    template <class Params, class Result, class F>
    void on(std::string method, F handler) {
        on_request(std::move(method),
            [h = std::move(handler)](const RpcId&, const Json& j) -> Maybe<Json> {
                Params p = j.is_null() ? Params{} : from_json<Params>(j);
                Result r = h(p);
                return Just<Json>(to_json(r));
            });
    }
    template <class Params, class F>
    void on_note(std::string method, F handler) {
        on_notification(std::move(method),
            [h = std::move(handler)](const Json& j) {
                Params p = j.is_null() ? Params{} : from_json<Params>(j);
                h(p);
            });
    }

    // -------------------------------------------------- deferred (async) replies
    //
    //   Responder<Result> — a one-shot handle to a pending inbound request. The
    //   async handler captures it, hands it to a worker, and the worker calls
    //   .ok(result) or .error(...) when the work completes. Safe to move; the
    //   reply is sent exactly once (subsequent calls are no-ops).
    template <class Result>
    class Responder {
    public:
        Responder(RpcEngine* e, RpcId id) : eng_(e), id_(std::move(id)) {}
        Responder(Responder&& o) noexcept
            : eng_(o.eng_), id_(std::move(o.id_)), done_(o.done_) { o.eng_ = nullptr; }
        Responder& operator=(Responder&&) = delete;
        Responder(const Responder&)        = delete;
        Responder& operator=(const Responder&) = delete;

        void ok(const Result& r) {
            if (!eng_ || done_) return;
            done_ = true;
            if constexpr (std::is_same_v<Result, Unit>)
                eng_->respond_raw(id_, Json::object());
            else
                eng_->respond_raw(id_, to_json(r));
        }
        void error(int code, std::string message, Json data = Json()) {
            if (!eng_ || done_) return;
            done_ = true;
            eng_->respond_error_raw(id_, code, std::move(message), std::move(data));
        }
        void error(const RpcError& e) { error(e.code, e.what(), e.data); }
        const RpcId& id() const noexcept { return id_; }

    private:
        RpcEngine* eng_;
        RpcId      id_;
        bool       done_ = false;
    };

    // Register a handler that answers asynchronously. The handler runs on the
    // reader thread but returns immediately (void); it must move the Responder
    // somewhere that eventually calls .ok()/.error(). The engine writes NO
    // synchronous reply for this method.
    template <class Params, class Result, class F>
    void on_async(std::string method, F handler) {
        on_request(std::move(method),
            [this, h = std::move(handler)](const RpcId& id, const Json& j) -> Maybe<Json> {
                Params p = j.is_null() ? Params{} : from_json<Params>(j);
                h(p, Responder<Result>(this, id));
                return Nothing;   // deferred — reply travels later
            });
    }

    // Send a late reply to a previously-deferred request. Thread-safe; the
    // write is serialised through the transport's sink.
    void respond_raw(const RpcId& id, const Json& result) {
        Json env = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        write_line(env.dump());
    }
    void respond_error_raw(const RpcId& id, int code, std::string message,
                           Json data = Json()) {
        send_error(id, code, std::move(message), std::move(data));
    }

    // ---------------------------------------------------------------- outbound
    // Raw (Json) notification — for custom/extension methods.
    void notify_raw(std::string_view method, const Json& params) {
        Json env = {{"jsonrpc", "2.0"}, {"method", std::string(method)}};
        if (!params.is_null()) env["params"] = params;
        write_line(env.dump());
    }
    // Typed notification.
    template <class Params>
    void notify(std::string_view method, const Params& params) {
        notify_raw(method, to_json(params));
    }
    // Notification with no params (e.g. `logout` is technically a request, but
    // many extension notifications carry no payload).
    void notify(std::string_view method) { notify_raw(method, Json::object()); }

    // Raw request — returns the future as raw Json.
    //
    //   `timeout` of zero means "use the engine default" (set_default_timeout);
    //   if that is also zero the request waits forever. A non-zero timeout that
    //   elapses before a response fails the future with RpcError(errc::Timeout).
    //
    //   The returned future is backed DIRECTLY by the promise the reader thread
    //   fulfils — not a deferred std::async — so wait_for / wait_until behave
    //   correctly (a deferred future would report `deferred` and never block).
    std::future<Json> request_raw(std::string_view method, const Json& params,
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
        std::int64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        Json id_json = id;
        std::string key = id_json.dump();

        auto promise = std::make_shared<std::promise<Json>>();
        std::future<Json> fut = promise->get_future();

        long long ms = timeout.count();
        if (ms == 0) ms = default_timeout_.load(std::memory_order_relaxed);
        const bool has_deadline = ms > 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(ms);
        {
            std::lock_guard lk(mu_);
            Waiter w;
            w.promise      = std::move(promise);
            w.has_deadline = has_deadline;
            w.deadline     = deadline;
            waiters_.emplace(std::move(key), std::move(w));
        }
        if (has_deadline) ensure_timer();

        Json env = {{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}};
        if (!params.is_null()) env["params"] = params;
        write_line(env.dump());
        if (has_deadline) timer_cv_.notify_all();
        return fut;
    }
    // Typed request : Params → future<Result>.
    template <class Result, class Params>
    std::future<Result> request(std::string_view method, const Params& params,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
        auto raw = std::make_shared<std::future<Json>>(
            request_raw(method, to_json(params), timeout));
        // A deferred wrapper here is fine: it only performs the cheap decode
        // step; the actual blocking happens inside raw->get(), which is backed
        // by a real promise (so timed waits on the wrapper still work via the
        // underlying shared state once .get() is reached).
        return std::async(std::launch::deferred,
            [raw]() -> Result {
                Json j = raw->get();
                if constexpr (std::is_same_v<Result, Unit>) return Unit{};
                else return j.is_null() ? Result{} : from_json<Result>(j);
            });
    }
    // Result-only typed request (no params).
    template <class Result>
    std::future<Result> request(std::string_view method,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
        return request<Result, Unit>(method, Unit{}, timeout);
    }

    // ------------------------------------------------------------- ext methods
    //
    //   ExtRequest / ExtNotification escape hatches — send/receive any custom
    //   method that is not part of the spec. Convention: method names starting
    //   with "_" are reserved for ext use.
    //
    //   These are thin wrappers over request_raw / notify_raw / on_request /
    //   on_notification, but they make the intent explicit at call sites.
    //
    std::future<Json> ext_request(std::string_view method, const Json& params = Json::object()) {
        return request_raw(method, params);
    }
    void ext_notify(std::string_view method, const Json& params = Json::object()) {
        notify_raw(method, params);
    }
    void on_ext_request(std::string method, std::function<Json(const Json&)> h) {
        on_request(std::move(method),
            [h = std::move(h)](const RpcId&, const Json& p) -> Maybe<Json> {
                return Just<Json>(h(p));
            });
    }
    void on_ext_notification(std::string method, std::function<void(const Json&)> h) {
        on_notification(std::move(method), std::move(h));
    }

    // -------------------------------------------------------------- inbound
    // Feed a single received line (one JSON-RPC envelope, exactly).
    void feed_line(std::string_view line) {
        if (line.empty()) return;
        emit_trace(WireDir::Inbound, line);
        Json msg;
        try {
            msg = Json::parse(line);
        } catch (const std::exception& e) {
            send_error(Json(nullptr), errc::ParseError, e.what());
            return;
        }
        if (msg.is_array()) {
            for (const auto& m : msg) dispatch_one(m);
        } else {
            dispatch_one(msg);
        }
    }

    // Report a transport-level fault (EOF, reader exception). The supplied
    // transports call this when the read pump stops. Fails every in-flight
    // request and notifies the error callback. Idempotent in effect.
    void on_transport_closed(std::string reason = "connection closed") {
        report_error(errc::ConnectionLost, reason);
        shutdown(std::move(reason), errc::ConnectionLost);
    }

    // ------------------------------------------------------------- lifecycle
    // Cancel every outstanding outbound request with an error. Idempotent.
    void shutdown(std::string reason = "engine shutdown",
                  int code = errc::InternalError) {
        std::unordered_map<std::string, Waiter> taken;
        {
            std::lock_guard lk(mu_);
            taken.swap(waiters_);
        }
        for (auto& [id, w] : taken) {
            if (!w.promise) continue;
            try {
                w.promise->set_exception(std::make_exception_ptr(
                    RpcError(code, reason)));
            } catch (...) {}
        }
        stop_timer();
    }

private:
    // ----------------------------------------------------------- bookkeeping
    void write_line(std::string s) {
        // Spec: each frame is one JSON-RPC envelope with NO embedded newlines.
        // nlohmann::json::dump() never inserts literal '\n' (we don't pass
        // pretty-print). The transport appends framing.
        emit_trace(WireDir::Outbound, s);
        if (write_) write_(s);
    }

    void send_error(const RpcId& id, int code, std::string msg, Json data = Json()) {
        Json env = {{"jsonrpc", "2.0"}, {"id", id}};
        Json err = {{"code", code}, {"message", std::move(msg)}};
        if (!data.is_null()) err["data"] = std::move(data);
        env["error"] = std::move(err);
        write_line(env.dump());
    }

    void dispatch_one(const Json& msg) {
        if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0") {
            send_error(msg.value("id", Json(nullptr)), errc::InvalidRequest,
                       "missing or invalid 'jsonrpc' field");
            return;
        }
        const bool has_method = msg.contains("method");
        const bool has_id     = msg.contains("id");
        const bool has_result = msg.contains("result");
        const bool has_error  = msg.contains("error");

        if (has_method && has_id)                return handle_request(msg);
        if (has_method && !has_id)               return handle_notification(msg);
        if (has_id && (has_result || has_error)) return handle_response(msg);
        send_error(msg.value("id", Json(nullptr)), errc::InvalidRequest,
                   "envelope is neither request, notification, nor response");
    }

    void handle_request(const Json& msg) {
        const auto& method = msg.at("method").get_ref<const std::string&>();
        const RpcId id     = msg.at("id");
        const Json params  = msg.value("params", Json::object());

        RawRequest h;
        {
            std::lock_guard lk(mu_);
            if (auto it = requests_.find(method); it != requests_.end()) h = it->second;
        }
        if (!h) {
            return send_error(id, errc::MethodNotFound, "Method not found: " + method);
        }
        Maybe<Json> result;
        try {
            result = h(id, params);
        } catch (const RpcError& e) {
            return send_error(id, e.code, e.what(), e.data);
        } catch (const CodecError& e) {
            return send_error(id, errc::InvalidParams, e.what());
        } catch (const std::exception& e) {
            return send_error(id, errc::InternalError, e.what());
        } catch (...) {
            return send_error(id, errc::InternalError, "unknown exception");
        }
        // Nothing ⇒ the handler deferred; it will send the reply itself later.
        if (!result) return;
        Json env = {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(*result)}};
        write_line(env.dump());
    }

    void handle_notification(const Json& msg) {
        const auto& method = msg.at("method").get_ref<const std::string&>();
        const Json params  = msg.value("params", Json::object());

        RawNotification h;
        {
            std::lock_guard lk(mu_);
            if (auto it = notifications_.find(method); it != notifications_.end()) h = it->second;
        }
        if (!h) return;        // unknown notifications: ignore silently per spec
        try { h(params); } catch (...) { /* notifications never respond */ }
    }

    void handle_response(const Json& msg) {
        // Key on the canonical text form of the id (id.dump()). This matches
        // exactly how request_raw stored the waiter, so number ids, string ids,
        // and any other valid id resolve correctly — no coercion, no leak.
        const std::string key = msg.at("id").dump();

        std::shared_ptr<std::promise<Json>> p;
        {
            std::lock_guard lk(mu_);
            if (auto it = waiters_.find(key); it != waiters_.end()) {
                p = std::move(it->second.promise);
                waiters_.erase(it);
            }
        }
        if (!p) return;   // dropped: unknown / already-resolved id

        try {
            if (msg.contains("error")) {
                p->set_exception(std::make_exception_ptr(
                    from_json<RpcError>(msg.at("error"))));
            } else {
                p->set_value(msg.value("result", Json()));
            }
        } catch (const std::future_error&) { /* already satisfied */ }
    }

    // ----------------------------------------------------------- hooks/timer
    void emit_trace(WireDir dir, std::string_view frame) {
        WireTrace t;
        { std::lock_guard lk(mu_); t = trace_; }
        if (t) { try { t(dir, frame); } catch (...) {} }
    }
    void report_error(int code, std::string_view msg) {
        ErrorCallback e;
        { std::lock_guard lk(mu_); e = on_error_; }
        if (e) { try { e(code, msg); } catch (...) {} }
    }

    // Start the deadline-monitor thread once, on the first timed request.
    void ensure_timer() {
        bool expected = false;
        if (!timer_started_.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel))
            return;   // already started
        timer_running_.store(true, std::memory_order_release);
        timer_thread_ = std::thread([this] { timer_loop(); });
    }

    void stop_timer() {
        if (!timer_started_.load(std::memory_order_acquire)) return;
        timer_running_.store(false, std::memory_order_release);
        timer_cv_.notify_all();
        if (timer_thread_.joinable() &&
            timer_thread_.get_id() != std::this_thread::get_id())
            timer_thread_.join();
    }

    // Wakes on the nearest deadline; fails any waiter whose deadline passed.
    void timer_loop() {
        std::unique_lock lk(mu_);
        while (timer_running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            auto next = std::chrono::steady_clock::time_point::max();
            bool any = false;

            for (auto it = waiters_.begin(); it != waiters_.end(); ) {
                auto& w = it->second;
                if (!w.has_deadline) { ++it; continue; }
                if (w.deadline <= now) {
                    auto p = std::move(w.promise);
                    it = waiters_.erase(it);
                    if (p) {
                        try {
                            p->set_exception(std::make_exception_ptr(
                                RpcError(errc::Timeout, "request timed out")));
                        } catch (...) {}
                    }
                } else {
                    any = true;
                    if (w.deadline < next) next = w.deadline;
                    ++it;
                }
            }

            if (any) timer_cv_.wait_until(lk, next);
            else     timer_cv_.wait(lk);
        }
    }

    Transport write_;
    std::atomic<std::int64_t> next_id_{1};
    std::mutex mu_;
    std::unordered_map<std::string, RawRequest>      requests_;
    std::unordered_map<std::string, RawNotification> notifications_;

    // A pending outbound request: its promise plus an optional deadline. Keyed
    // by the CANONICAL string form of the JSON-RPC id (id.dump()), so string
    // ids, integer ids, and any other valid id round-trip exactly and never
    // leak a waiter on an unexpected echo.
    struct Waiter {
        std::shared_ptr<std::promise<Json>> promise;
        std::chrono::steady_clock::time_point deadline{};   // zero == none
        bool has_deadline = false;
        bool done = false;
    };
    std::unordered_map<std::string, Waiter> waiters_;

    WireTrace     trace_;
    ErrorCallback on_error_;
    std::atomic<long long> default_timeout_{0};   // ms; 0 == no timeout

    // Deadline monitor — a lazily-started background thread that fails any
    // waiter whose deadline has passed. Started on the first timed request.
    std::thread             timer_thread_;
    std::condition_variable timer_cv_;
    std::atomic<bool>       timer_running_{false};
    std::atomic<bool>       timer_started_{false};
};

} // namespace mcp
