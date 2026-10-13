// tests/wire.hpp — two jsonrpc engines and the frames between them, carried
// by hand on one thread. A test answers the server's side with any function
// of a Call, and the client's side with ClientHandlers.
#pragma once

#include <mcp/mcp.hpp>

#include <deque>
#include <functional>
#include <string>

namespace mcp::test {

struct Wire {
    Engine client;
    Engine server;
    std::function<std::string(const Call&)>   answer;          // the server's calls
    std::function<void(const Notification&)>  on_server_note;  // optional
    ClientHandlers handlers;                                     // the client's side
    std::deque<std::string> to_server, to_client;
    List<Completed> client_done, server_done;

    void pump() {
        for (int guard = 0; guard < 1000 && (!to_server.empty() || !to_client.empty()); ++guard) {
            while (!to_server.empty()) {
                auto f = std::move(to_server.front()); to_server.pop_front();
                Effects fx = step(server, Received{f});
                for (auto& s : fx.send) to_client.push_back(std::move(s));
                for (auto& c : fx.calls) to_client.push_back(answer(c));
                for (auto& n : fx.notifications) if (on_server_note) on_server_note(n);
                for (auto& c : fx.completed) server_done.push_back(std::move(c));
            }
            while (!to_client.empty()) {
                auto f = std::move(to_client.front()); to_client.pop_front();
                Effects fx = step(client, Received{f});
                for (auto& s : fx.send) to_server.push_back(std::move(s));
                for (auto& c : fx.calls) to_server.push_back(handlers.handle(c));
                for (auto& n : fx.notifications) handlers.handle(n);
                for (auto& c : fx.completed) client_done.push_back(std::move(c));
            }
        }
    }

    // A raw client → server call, carried to completion.
    std::expected<Json, RpcError> call_raw(std::string_view method, Json params) {
        auto [id, fx] = request_raw(client, method, std::move(params), Nothing);
        for (auto& s : fx.send) to_server.push_back(std::move(s));
        pump();
        for (auto& c : client_done)
            if (c.id == id) return c.outcome;
        return std::unexpected(RpcError(errc::InternalError, "no completion"));
    }

    template <jsonrpc::IsMethod M>
    std::expected<typename M::result, RpcError> call(const typename M::params& p) {
        auto r = call_raw(M::name, to_json(p));
        if (!r) return std::unexpected(r.error());
        return from_json<typename M::result>(*r);
    }

    // A call driven through MRTR: retried with the fulfilled inputs until the
    // server returns a final result.
    std::expected<Json, RpcError> call_mrtr(std::string_view method, Json params) {
        const auto mrtr = handlers.mrtr();
        for (int round = 0; round < kMrtrMaxRounds; ++round) {
            auto r = call_raw(method, params);
            if (!r) return r;
            auto retry = mrtr_retry(params, *r, mrtr);
            if (!retry) return r;
            params = std::move(*retry);
        }
        return std::unexpected(RpcError(errc::InternalError, "MRTR: too many rounds"));
    }

    // A server → client call, carried to completion.
    template <jsonrpc::IsMethod M>
    std::expected<typename M::result, RpcError> server_call(const typename M::params& p) {
        auto [id, fx] = request<M>(server, p);
        for (auto& s : fx.send) to_client.push_back(std::move(s));
        pump();
        for (auto& c : server_done)
            if (c.id == id) return result<M>(c);
        return std::unexpected(RpcError(errc::InternalError, "no completion"));
    }
};

// Answer calls from a Router over some context.
template <class Ctx>
std::function<std::string(const Call&)> answer_with(Router<Ctx>& r, Ctx& ctx) {
    return [&r, &ctx](const Call& c) -> std::string {
        if (auto f = r.handle(ctx, c)) return *f;
        return reply(c.id, std::unexpected(RpcError(errc::InternalError, "deferred")));
    };
}

}  // namespace mcp::test
