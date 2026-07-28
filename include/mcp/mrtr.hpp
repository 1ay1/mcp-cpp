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
//   `run_mrtr()` drives that loop generically over any raw request, so it works
//   for tools/call, completion/complete, prompts/get, resources/read — every
//   request the spec allows an InputRequiredResult on.
//
#pragma once

#include <mcp/rpc.hpp>
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

//==============================================================================
//  run_mrtr — drive a request to a FINAL result, fulfilling any input_required
//             rounds along the way.
//
//    engine        the RpcEngine to send on (per-request _meta already armed
//                  via Client::enable_modern_metadata()).
//    method        the wire method (e.g. method::CallTool).
//    params        the initial request params (Json).
//    handlers      how to answer sampling / elicitation / roots sub-requests.
//    max_rounds    safety bound on the retry loop (a misbehaving server could
//                  otherwise loop forever); each input_required round counts.
//
//  Returns the final (resultType != "input_required") raw result Json. Throws
//  RpcError on a transport/JSON-RPC error, or if max_rounds is exhausted.
//==============================================================================
inline Json run_mrtr(RpcEngine& engine,
                     std::string_view method,
                     Json params,
                     const MrtrHandlers& handlers,
                     int max_rounds = 8) {
    for (int round = 0; round < max_rounds; ++round) {
        Json result = engine.request_raw(method, params).get();

        if (!is_input_required(result))
            return result;                          // final result — done.

        // Fulfil the requested inputs (if any) and prepare the retry.
        Json retry = params.is_object() ? params : Json::object();
        Json& meta = retry["_meta"];
        if (!meta.is_object()) meta = Json::object();

        if (result.contains("inputRequests")) {
            Json responses = fulfill_input_requests(result["inputRequests"], handlers);
            if (!responses.empty())
                meta[std::string(meta_key::InputResponses)] = std::move(responses);
        }
        // Echo the opaque requestState EXACTLY if present; never include one
        // the server didn't send (spec: MUST NOT).
        if (result.contains("requestState"))
            meta[std::string(meta_key::RequestState)] = result["requestState"];

        params = std::move(retry);                  // loop with the enriched request.
    }
    throw RpcError(errc::InternalError,
                   "MRTR: exceeded max rounds without a final result");
}

} // namespace mcp
