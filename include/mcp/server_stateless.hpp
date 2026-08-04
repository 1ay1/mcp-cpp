// SPDX-License-Identifier: Apache-2.0
//
// mcp/server_stateless.hpp — the SERVER half of the MCP 2026-07-28 stateless
// core: emit `input_required` results, carry an opaque integrity-protected
// `requestState` across rounds, and read the modern per-request `_meta`
// (negotiated protocolVersion / clientInfo / clientCapabilities / echoed
// inputResponses) WITHOUT a session.
//
//   mrtr.hpp is the CLIENT driver (run_mrtr); this is the SERVER's tool. A
//   stateless server handling e.g. `tools/call`:
//
//     1. Reads the incoming request through `IncomingRequest{params}`:
//          - .protocol_version()  negotiated version the client declared
//          - .client_info()       who's calling (no initialize needed)
//          - .input_responses()   answers to a PRIOR input_required round
//          - .request_state<T>()  the opaque blob it emitted last round,
//                                  verified + decoded back into its own context
//
//     2. If it needs the client to sample / elicit / list roots, it returns a
//        NON-final result built with `input_required(...)`, embedding whatever
//        context it needs to resume as a signed `requestState`. It keeps NO
//        server memory.
//
//     3. When the client retries (fresh id) carrying the fulfilled
//        `inputResponses` + the echoed `requestState`, the server verifies the
//        state, folds in the responses, and either finishes or asks again.
//
//   The `requestState` is signed with an HMAC-FNV keyed on a per-process (or
//   caller-supplied) secret so a client cannot forge or tamper with server
//   context. It is Base64URL-encoded so it survives a JSON string round-trip.
//   This is deliberately dependency-free (no OpenSSL): the goal is integrity +
//   tamper-evidence for a same-process/same-deployment secret, not defence
//   against an offline cryptographic attacker who holds the key.
//
#pragma once

#include <mcp/rpc.hpp>
#include <mcp/methods.hpp>
#include <mcp/ids.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

//==============================================================================
//  Base64URL (no padding) — requestState travels inside a JSON string.
//==============================================================================
namespace detail {

inline std::string b64url_encode(std::string_view in) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        std::uint32_t n = (std::uint8_t(in[i]) << 16) |
                          (std::uint8_t(in[i + 1]) << 8) |
                           std::uint8_t(in[i + 2]);
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);
        out.push_back(kAlpha[n & 63]);
    }
    if (i + 1 == in.size()) {
        std::uint32_t n = std::uint8_t(in[i]) << 16;
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
    } else if (i + 2 == in.size()) {
        std::uint32_t n = (std::uint8_t(in[i]) << 16) | (std::uint8_t(in[i + 1]) << 8);
        out.push_back(kAlpha[(n >> 18) & 63]);
        out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);
    }
    return out;
}

inline std::optional<std::string> b64url_decode(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3 + 3);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) return std::nullopt;   // reject any non-alphabet byte
        buf = (buf << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// FNV-1a 64-bit — the same primitive the tools use; here it seeds a keyed MAC.
inline std::uint64_t fnv1a(std::string_view bytes, std::uint64_t seed) noexcept {
    constexpr std::uint64_t kPrime = 0x00000100000001b3ULL;
    std::uint64_t h = seed;
    for (char ch : bytes) { h ^= static_cast<unsigned char>(ch); h *= kPrime; }
    return h;
}

// Fold a 64-bit value into an FNV state byte-by-byte in a FIXED (big-endian,
// most-significant-first) order, so the MAC is identical regardless of host
// endianness — no reinterpret_cast over the object representation.
inline std::uint64_t fnv1a_u64(std::uint64_t v, std::uint64_t seed) noexcept {
    constexpr std::uint64_t kPrime = 0x00000100000001b3ULL;
    std::uint64_t h = seed;
    for (int shift = 56; shift >= 0; shift -= 8) {
        h ^= (unsigned char)((v >> shift) & 0xFF);
        h *= kPrime;
    }
    return h;
}

// Keyed MAC over `msg`: HMAC-style two-pass FNV so key material never appears
// linearly next to the message. 64-bit tag, hex-encoded. Endianness-stable.
inline std::string keyed_mac(std::string_view msg, std::string_view key) {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    const std::uint64_t k = fnv1a(key, kOffset);
    const std::uint64_t inner = fnv1a(msg, k ^ 0x3636363636363636ULL);
    const std::uint64_t outer = fnv1a_u64(inner, k ^ 0x5c5c5c5c5c5c5c5cULL);
    static constexpr char hex[] = "0123456789abcdef";
    std::string tag(16, '0');
    for (std::size_t i = 0; i < 16; ++i) tag[15 - i] = hex[(outer >> (i * 4)) & 0xF];
    return tag;
}

// Constant-time-ish string compare (length-independent early-out is fine here;
// both tags are fixed 16 chars). Avoids leaking match position via timing.
inline bool tag_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace detail

//==============================================================================
//  RequestStateCodec — sign/verify an opaque server-context blob.
//
//    seal(json)  -> "<b64url(payload)>.<mac>"      goes into a result's
//                                                  requestState field.
//    open(str)   -> optional<Json>                 verifies + decodes; nullopt
//                                                  on tamper / wrong key / junk.
//
//  Construct once with a per-process secret (default: a random-ish constant you
//  SHOULD override in production via RequestStateCodec{my_secret}). The state
//  is opaque to the client, which only echoes it back verbatim.
//==============================================================================
class RequestStateCodec {
public:
    RequestStateCodec() : secret_(default_secret()) {}
    explicit RequestStateCodec(std::string secret) : secret_(std::move(secret)) {}

    [[nodiscard]] std::string seal(const Json& state) const {
        // CBOR (not text JSON) as the payload: binary-exact (no UTF-8
        // validation that dump() would throw on), more compact, and faster to
        // (de)serialise — the blob is opaque to the client anyway.
        const std::vector<std::uint8_t> cbor = Json::to_cbor(state);
        const std::string payload = detail::b64url_encode(
            std::string_view{reinterpret_cast<const char*>(cbor.data()), cbor.size()});
        return payload + "." + detail::keyed_mac(payload, secret_);
    }

    [[nodiscard]] std::optional<Json> open(std::string_view sealed) const {
        const auto dot = sealed.rfind('.');
        if (dot == std::string_view::npos) return std::nullopt;
        const std::string_view payload = sealed.substr(0, dot);
        const std::string_view mac     = sealed.substr(dot + 1);
        if (!detail::tag_equal(mac, detail::keyed_mac(payload, secret_)))
            return std::nullopt;                       // tampered or wrong key
        auto raw = detail::b64url_decode(payload);
        if (!raw) return std::nullopt;
        // from_cbor with exceptions off: a malformed (but correctly-signed)
        // blob yields discarded rather than throwing.
        Json j = Json::from_cbor(*raw, /*strict=*/true, /*allow_exceptions=*/false);
        if (j.is_discarded()) return std::nullopt;
        return j;
    }

private:
    static std::string default_secret() {
        // Deterministic per-build fallback. Callers handling untrusted clients
        // MUST supply a real secret; this only guards against accidental
        // corruption + naive tampering when none is provided.
        return "io.modelcontextprotocol/requestState/default-secret";
    }
    std::string secret_;
};

//==============================================================================
//  IncomingRequest — a read-only view over one incoming request's params +
//  _meta.
//
//  Lets a stateless handler recover everything it would have learned from an
//  initialize handshake, plus MRTR continuation data, straight from the wire.
//==============================================================================
class IncomingRequest {
public:
    explicit IncomingRequest(const Json& params) : params_(params) {
        if (auto it = params.find("_meta"); it != params.end() && it->is_object())
            meta_ = &*it;
    }

    // The raw request params (with _meta still attached).
    const Json& params() const noexcept { return params_; }
    // The `_meta` object (empty object if absent).
    const Json& meta() const noexcept { return meta_ ? *meta_ : empty(); }

    // Modern per-request protocol version the client declared, or "" if legacy
    // (the client relied on a prior initialize instead).
    std::string protocol_version() const {
        return meta_str(meta_key::ProtocolVersion);
    }
    // True iff the declared version is one this build serves.
    bool version_supported() const {
        if (!meta_) return true;                       // legacy / handshake path
        auto it = meta_->find(meta_key::ProtocolVersion.data());
        if (it == meta_->end() || !it->is_string()) return true;
        const std::string_view v = it->get_ref<const std::string&>();
        if (v.empty()) return true;
        for (auto s : kSupportedProtocolVersions)
            if (v == s) return true;
        return false;
    }

    // Who is calling (name/version), if the client attached it. Optional.
    std::optional<Implementation> client_info() const {
        if (const Json* v = meta_obj(meta_key::ClientInfo))
            return from_json<Implementation>(*v);
        return std::nullopt;
    }

    // The client's declared capabilities (for MissingRequiredClientCapability
    // enforcement), if attached.
    std::optional<ClientCapabilities> client_capabilities() const {
        if (const Json* v = meta_obj(meta_key::ClientCapabilities))
            return from_json<ClientCapabilities>(*v);
        return std::nullopt;
    }

    // Fulfilled answers to a PRIOR input_required round: { "<key>": <result> }.
    // Empty object if this is the first round.
    const Json& input_responses() const {
        if (const Json* v = meta_obj(meta_key::InputResponses)) return *v;
        return empty();
    }
    // Convenience: the fulfilled result for one input key, or null. `key` is
    // caller-supplied so we can't assume null-termination — materialise it.
    Json input_response(std::string_view key) const {
        const Json& all = input_responses();
        auto it = all.find(std::string(key));
        return it != all.end() ? *it : Json(nullptr);
    }

    // Raw echoed requestState string ("" if none), for manual verification.
    std::string request_state_raw() const { return meta_str(meta_key::RequestState); }

    // Verify + decode the opaque state the server emitted last round. Returns
    // nullopt when absent, tampered, or signed with a different secret.
    std::optional<Json> request_state(const RequestStateCodec& codec) const {
        const std::string s = request_state_raw();
        if (s.empty()) return std::nullopt;
        return codec.open(s);
    }

private:
    static const Json& empty() { static const Json e = Json::object(); return e; }
    // Allocation-free _meta lookups: meta_key values are literal-backed,
    // null-terminated string_views, so .data() is a valid C-string and
    // nlohmann's find(const char*) needs no temporary std::string.
    const Json* meta_obj(std::string_view key) const {
        if (!meta_) return nullptr;
        auto it = meta_->find(key.data());
        return (it != meta_->end() && it->is_object()) ? &*it : nullptr;
    }
    std::string meta_str(std::string_view key) const {
        if (!meta_) return {};
        auto it = meta_->find(key.data());
        return (it != meta_->end() && it->is_string()) ? it->get<std::string>() : std::string{};
    }
    const Json& params_;
    const Json* meta_ = nullptr;
};

//==============================================================================
//  input_required — build a NON-final MRTR result.
//
//    requests   { "<key>": { method, params }, ... } — the sub-requests the
//               client must fulfil locally (sampling / elicitation / roots).
//    state      opaque server context (already sealed) echoed back on retry.
//               Pass "" to carry none.
//
//  The client answers each request, then retries the ORIGINAL request with the
//  fulfilled inputResponses + this state in its `_meta`. See run_mrtr().
//==============================================================================
inline Json input_required(Json requests, std::string state = {}) {
    Json r = {
        {"resultType",    "input_required"},
        {"inputRequests", std::move(requests)},
    };
    if (!state.empty()) r["requestState"] = std::move(state);
    return r;
}

// One sub-request builder: input_request(method::Elicit, to_json(params)).
inline Json input_request(std::string_view method, Json params) {
    return Json{{"method", std::string(method)}, {"params", std::move(params)}};
}

//==============================================================================
//  Capability enforcement (2026-07-28). A stateless server that needs the
//  client to support e.g. elicitation (because the tool may go MRTR) can
//  REQUIRE it up front and reject with MissingRequiredClientCapability rather
//  than emitting an input_required the client can't fulfil. The client declared
//  its capabilities under _meta[clientCapabilities] (IncomingRequest reads it).
//==============================================================================
enum class ClientCap { Roots, Sampling, Elicitation, Tasks };

inline bool client_has(const ClientCapabilities& c, ClientCap cap) noexcept {
    switch (cap) {
        case ClientCap::Roots:       return c.roots.has_value();
        case ClientCap::Sampling:    return c.sampling.has_value();
        case ClientCap::Elicitation: return c.elicitation.has_value();
        case ClientCap::Tasks:       return c.tasks.has_value();
    }
    return false;
}

inline std::string_view cap_name(ClientCap cap) noexcept {
    switch (cap) {
        case ClientCap::Roots:       return "roots";
        case ClientCap::Sampling:    return "sampling";
        case ClientCap::Elicitation: return "elicitation";
        case ClientCap::Tasks:       return "tasks";
    }
    return "";
}

// Throw MissingRequiredClientCapability (errc -32021) if the incoming request's
// client didn't declare EVERY capability in `needed`. If the client attached no
// clientCapabilities at all (legacy / omitted), we conservatively allow it —
// the operation will still surface its own error if it truly can't proceed.
// The thrown RpcError propagates as a proper JSON-RPC error from a handler.
inline void require_capabilities(const IncomingRequest& req,
                                 std::initializer_list<ClientCap> needed) {
    auto caps = req.client_capabilities();
    if (!caps) return;                       // nothing declared — don't over-reject
    for (ClientCap cap : needed) {
        if (!client_has(*caps, cap)) {
            const std::string name(cap_name(cap));
            throw RpcError(errc::MissingRequiredClientCapability,
                           "missing required client capability: " + name,
                           Json{{"capability", name}});
        }
    }
}

//==============================================================================
//  Header-based routing (SEP-2243). On Streamable HTTP a client MUST send
//  `Mcp-Method` (and, for tools/call, `Mcp-Name`) so a gateway/server can route
//  and authorize WITHOUT parsing the body. A server that trusts these headers
//  for routing MUST verify they match the body, else a caller could route as
//  one method while the body invokes another (confused-deputy). These helpers
//  do that check for the host's HTTP server layer.
//==============================================================================
struct RoutingHeaders {
    std::string_view method;   // Mcp-Method header value ("" if absent)
    std::string_view name;     // Mcp-Name header value  ("" if absent)
};

// Extract the routing signal from a parsed JSON-RPC request body.
inline RoutingHeaders routing_from_body(const Json& body) {
    RoutingHeaders h{};
    if (body.is_object()) {
        if (auto it = body.find("method"); it != body.end() && it->is_string())
            h.method = it->get_ref<const std::string&>();
        if (h.method == "tools/call")
            if (auto p = body.find("params"); p != body.end() && p->is_object())
                if (auto n = p->find("name"); n != p->end() && n->is_string())
                    h.name = n->get_ref<const std::string&>();
    }
    return h;
}

// Returns true iff the declared headers are consistent with the body. An absent
// header is permitted (a header MAY be omitted; only a PRESENT-and-WRONG header
// is a routing lie). `err` receives a reason on mismatch.
inline bool routing_headers_match(const RoutingHeaders& hdr, const Json& body,
                                  std::string& err) {
    const RoutingHeaders want = routing_from_body(body);
    if (!hdr.method.empty() && hdr.method != want.method) {
        err = "Mcp-Method header '" + std::string(hdr.method) +
              "' does not match request method '" + std::string(want.method) + "'";
        return false;
    }
    if (!hdr.name.empty() && hdr.name != want.name) {
        err = "Mcp-Name header '" + std::string(hdr.name) +
              "' does not match tool name '" + std::string(want.name) + "'";
        return false;
    }
    return true;
}

//==============================================================================
//  Cacheable list results (MCP 2026-07-28 caching utility, SEP-2549). A server
//  may tell the client how long a tools/list, prompts/list, resources/list, or
//  resources/read result stays fresh and at what scope, so the client can skip
//  a redundant round-trip and keep upstream prompt caches stable across
//  reconnects. These are hints carried in the result body; a client that
//  ignores them is still correct.
//
//    cacheScope: "public"  — identical for every caller (shareable), or
//                "private" — specific to this client / credential.
//==============================================================================
struct CacheHint {
    std::int64_t ttl_ms = 0;                 // 0 ⇒ omit (not cacheable)
    std::string  scope  = "public";          // "public" | "private"
};

// Attach a cache hint to any result object (adds ttlMs / cacheScope in-place).
inline Json& apply_cache_hint(Json& result, const CacheHint& h) {
    if (h.ttl_ms > 0) {
        result["ttlMs"]      = h.ttl_ms;
        result["cacheScope"] = h.scope;
    }
    return result;
}
inline Json with_cache_hint(Json result, const CacheHint& h) {
    apply_cache_hint(result, h);
    return result;
}

} // namespace mcp
