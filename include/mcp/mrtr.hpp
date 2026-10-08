// SPDX-License-Identifier: Apache-2.0
//
// mcp/mrtr.hpp — Multi Round-Trip Requests (MCP 2026-07-28) client helper.
//
//   MRTR replaces the old server-initiated request pattern. Instead of the
//   server opening a reverse request mid-flight (which needs a sticky session),
//   the server answers a client request with a NON-final result:
//
//       { "resultType": "input_required",
//         "inputRequests": { "<key>": { "method": ..., "params": ... }, ... },
//         "requestState":  "<opaque server blob>" }
//
//   The client fulfils each inputRequest LOCALLY using the same handlers it
//   would use for sampling / elicitation / roots, then RETRIES the original
//   request — with a fresh JSON-RPC id — carrying the fulfilled `inputResponses`
//   and the echoed `requestState` in the request `_meta`. The server, holding
//   no session, reconstructs its context purely from the (integrity-protected)
//   requestState. This loops until the server returns a final result.
//
//   `mrtr_retry()` is one turn of that loop, as a pure function: given the
//   params sent and the result received, it returns the params to send next,
//   or Nothing when the result is final. The caller owns the loop and the
//   sending, so this works for any request the spec allows an
//   InputRequiredResult on (tools/call, completion/complete, prompts/get,
//   resources/read).
//
#pragma once

#include <mcp/methods.hpp>

#include <functional>
#include <string>

namespace mcp {

//==============================================================================
//  MRTR fulfilment callbacks — how the client answers each server sub-request.
//  Each takes the sub-request's raw `params` and returns the raw result Json.
//  Leave a handler empty to signal "capability not supported": run_mrtr() then
//  omits that response (the server SHOULD NOT have asked, per spec, but we fail
//  safe by simply not fulfilling it rather than fabricating a result).
//==============================================================================
struct MrtrHandlers {
    std::function<Json(const Json& params)> on_create_message;  // sampling/createMessage
    std::function<Json(const Json& params)> on_elicit;          // elicitation/create
    std::function<Json(const Json& params)> on_list_roots;      // roots/list
};

//==============================================================================
//  is_input_required — does a decoded result envelope ask for more input?
//==============================================================================
inline bool is_input_required(const Json& result) noexcept {
    return result.is_object()
        && result.value("resultType", std::string{}) == "input_required";
}

//==============================================================================
//  fulfill_input_requests — build the InputResponses map for one round.
//
//  For each { key: { method, params } } in `inputRequests`, dispatch to the
//  matching handler and collect { key: <result> }. Unknown methods and
//  unsupported capabilities are skipped (the server re-asks if it still needs
//  them, per the MRTR error-handling rules).
//==============================================================================
inline Json fulfill_input_requests(const Json& inputRequests,
                                   const MrtrHandlers& h) {
    Json responses = Json::object();
    if (!inputRequests.is_object()) return responses;

    for (auto it = inputRequests.begin(); it != inputRequests.end(); ++it) {
        const Json& req = it.value();
        if (!req.is_object()) continue;
        const std::string m = req.value("method", std::string{});
        const Json params    = req.value("params", Json::object());

        if (m == method::CreateMessage && h.on_create_message)
            responses[it.key()] = h.on_create_message(params);
        else if (m == method::Elicit && h.on_elicit)
            responses[it.key()] = h.on_elicit(params);
        else if (m == method::ListRoots && h.on_list_roots)
            responses[it.key()] = h.on_list_roots(params);
        // else: unknown/unsupported — skip; server re-asks if still needed.
    }
    return responses;
}

// One MRTR turn. `params` is what was sent, `result` what came back. If the
// result is final, Nothing. Otherwise the retry params: the inputs fulfilled
// with `handlers` and the server's requestState echoed (never invented), both
// under `_meta`. Callers should bound the number of turns.
inline Maybe<Json> mrtr_retry(const Json& params, const Json& result,
                              const MrtrHandlers& handlers) {
    if (!is_input_required(result)) return Nothing;
    Json retry = params.is_object() ? params : Json::object();
    Json& meta = retry["_meta"];
    if (!meta.is_object()) meta = Json::object();
    if (result.contains("inputRequests")) {
        Json responses = fulfill_input_requests(result["inputRequests"], handlers);
        if (!responses.empty())
            meta[std::string(meta_key::InputResponses)] = std::move(responses);
    }
    if (result.contains("requestState"))
        meta[std::string(meta_key::RequestState)] = result["requestState"];
    return retry;
}

// How many input_required turns a caller should allow before giving up.
inline constexpr int kMrtrMaxRounds = 8;

} // namespace mcp
