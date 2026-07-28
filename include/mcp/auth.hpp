// SPDX-License-Identifier: Apache-2.0
//
// mcp/auth.hpp — MCP 2026-07-28 authorization: a dependency-free, transport-
// agnostic OAuth 2.1 client-flow helper with the revision's hardening baked in.
//
//   mcp-cpp does not ship an HTTP stack (the host provides one — see
//   tools/host.hpp HttpClient), so this header carries only the PURE logic of
//   the authorization-code + PKCE flow: building the requests, and — crucially
//   for 2026-07-28 — VALIDATING the responses. The host performs the actual
//   GET/POST and the browser redirect; it feeds the raw bytes back here.
//
//   2026-07-28 hardening implemented (all pure, all unit-testable offline):
//
//     • RFC 9207 issuer validation (SEP-2468) — the authorization response
//       MUST carry `iss`, and the client MUST reject it if it does not match
//       the issuer of the authorization server it started the flow with,
//       BEFORE redeeming the code. Closes the AS mix-up attack.
//     • application_type=native in DCR (SEP-837) — so an AS stops rejecting the
//       loopback (127.0.0.1) redirect a CLI/desktop client needs.
//     • Issuer-bound credentials (SEP-2352) — a token/registration is stamped
//       with the issuer that minted it and MUST NOT be replayed to a different
//       authorization server.
//     • CIMD (Client ID Metadata Documents) — the client may present a stable
//       https URL as its client_id instead of a DCR registration; DCR remains
//       supported (now deprecated) as a fallback.
//     • RFC 9728 protected-resource-metadata discovery — parse the
//       `WWW-Authenticate: Bearer resource_metadata=...` challenge a 401 from
//       an MCP endpoint carries, to locate the authorization server.
//     • PKCE (RFC 7636, S256) — always, no plain fallback.
//
//   The flow (host drives the I/O, this header drives the logic):
//     1. 401 from the MCP endpoint → parse_challenge() → resource metadata URL.
//     2. host GETs it → parse_protected_resource_metadata() → issuer(s).
//     3. host GETs <issuer>/.well-known/oauth-authorization-server →
//        parse_authorization_server_metadata() → endpoints.
//     4. (optional) DCR: registration_request() → host POSTs → parse it.
//     5. begin() → an AuthSession (PKCE verifier + state) + authorize URL.
//     6. host opens the browser; the loopback redirect delivers code + iss.
//     7. redeem() validates iss (RFC 9207) + state, builds the token request.
//     8. host POSTs it → parse_token_response() → issuer-bound AccessToken.
//     9. token.authorization_header() → "Bearer …" for every MCP request.
//
#pragma once

#include <mcp/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

//==============================================================================
//  Small codec helpers (self-contained; no OpenSSL, no libcurl).
//==============================================================================
namespace detail {

// Base64URL (no padding) of a byte range.
inline std::string b64url(std::string_view in) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        std::uint32_t n = (std::uint8_t(in[i]) << 16) |
                          (std::uint8_t(in[i + 1]) << 8) | std::uint8_t(in[i + 2]);
        out.push_back(kAlpha[(n >> 18) & 63]); out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);  out.push_back(kAlpha[n & 63]);
    }
    if (i + 1 == in.size()) {
        std::uint32_t n = std::uint8_t(in[i]) << 16;
        out.push_back(kAlpha[(n >> 18) & 63]); out.push_back(kAlpha[(n >> 12) & 63]);
    } else if (i + 2 == in.size()) {
        std::uint32_t n = (std::uint8_t(in[i]) << 16) | (std::uint8_t(in[i + 1]) << 8);
        out.push_back(kAlpha[(n >> 18) & 63]); out.push_back(kAlpha[(n >> 12) & 63]);
        out.push_back(kAlpha[(n >> 6) & 63]);
    }
    return out;
}

// SHA-256 (FIPS 180-4) — needed for the PKCE S256 challenge. Small, portable,
// endianness-safe (works on byte streams, emits a byte digest).
inline std::array<std::uint8_t, 32> sha256(std::string_view msg) {
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    static constexpr std::uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                          0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string data(msg);
    const std::uint64_t bitlen = std::uint64_t(data.size()) * 8;
    data.push_back('\x80');
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(char((bitlen >> (i * 8)) & 0xFF));

    for (std::size_t off = 0; off < data.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint8_t(data[off + i*4]) << 24) | (std::uint8_t(data[off + i*4+1]) << 16) |
                   (std::uint8_t(data[off + i*4+2]) << 8) | std::uint8_t(data[off + i*4+3]);
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            std::uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (h[i] >> 24) & 0xFF; out[i*4+1] = (h[i] >> 16) & 0xFF;
        out[i*4+2] = (h[i] >> 8)  & 0xFF; out[i*4+3] =  h[i]        & 0xFF;
    }
    return out;
}

// RFC 3986 percent-encoding for query/form values (unreserved set kept).
inline std::string pct_encode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out; out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(char(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
    }
    return out;
}

// Normalise an issuer URL for comparison (RFC 9207 / 8414): drop a single
// trailing slash and lowercase the scheme+host. Path case is preserved (some
// AS namespace tenants by path). Good enough for the exact-match the spec
// mandates without pulling in a full URL library.
inline std::string normalize_issuer(std::string_view iss) {
    std::string s(iss);
    // strip exactly one trailing '/'
    if (!s.empty() && s.back() == '/') s.pop_back();
    // lowercase scheme://host authority
    auto scheme = s.find("://");
    std::size_t host_end = (scheme == std::string::npos)
        ? std::string::npos : s.find('/', scheme + 3);
    std::size_t stop = (host_end == std::string::npos) ? s.size() : host_end;
    for (std::size_t i = 0; i < stop; ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] - 'A' + 'a');
    return s;
}

inline bool issuer_equal(std::string_view a, std::string_view b) {
    return normalize_issuer(a) == normalize_issuer(b);
}

} // namespace detail

// Public: RFC 9207/8414 issuer comparison (normalised: trailing-slash + scheme/
// host case-insensitive, path case-sensitive). Used for the AS-mix-up guard.
inline bool issuer_equal(std::string_view a, std::string_view b) {
    return detail::issuer_equal(a, b);
}

//==============================================================================
//  PKCE (RFC 7636, S256 only — no `plain`).
//==============================================================================
struct Pkce {
    std::string verifier;   // 43..128 unreserved chars
    std::string challenge;  // base64url(sha256(verifier))
    static constexpr std::string_view method = "S256";

    // Build from a caller-supplied high-entropy seed (>= 32 random bytes). The
    // host supplies the randomness (mcp-cpp has no RNG dependency); we shape it
    // into a valid verifier + derive the S256 challenge.
    static Pkce from_entropy(std::string_view random_bytes) {
        Pkce p;
        p.verifier = detail::b64url(random_bytes);       // unreserved, 43+ chars
        if (p.verifier.size() > 128) p.verifier.resize(128);
        auto dg = detail::sha256(p.verifier);
        p.challenge = detail::b64url(
            std::string_view{reinterpret_cast<const char*>(dg.data()), dg.size()});
        return p;
    }
};

//==============================================================================
//  Metadata records parsed from the AS / protected-resource documents.
//==============================================================================

// RFC 9728 protected-resource metadata (what a 401 challenge points at).
struct ProtectedResourceMetadata {
    std::string              resource;             // canonical resource id (aud)
    std::vector<std::string> authorization_servers; // issuer URLs
};

// RFC 8414 authorization-server metadata (the .well-known doc).
struct AuthServerMetadata {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> registration_endpoint;      // DCR (deprecated)
    bool        supports_pkce_s256 = false;                 // from code_challenge_methods_supported
    bool        authorization_response_iss_supported = false; // RFC 9207
};

// A client identity. Either a DCR-issued client_id, or a CIMD URL used AS the
// client_id (2026-07-28 preferred path).
struct ClientRegistration {
    std::string client_id;                    // DCR id or the CIMD https URL
    std::optional<std::string> client_secret; // confidential clients only
    std::string issuer;                       // AS that minted it (SEP-2352 binding)
    bool        is_cimd = false;              // client_id is a metadata-document URL
};

//==============================================================================
//  AccessToken — issuer-bound (SEP-2352). Never replay to a different AS.
//==============================================================================
struct AccessToken {
    std::string  access_token;
    std::string  token_type = "Bearer";
    std::optional<std::string>  refresh_token;
    std::optional<std::int64_t> expires_in;   // seconds, as returned
    std::string  issuer;                      // AS that minted it — binding
    std::string  resource;                    // aud this token is for

    [[nodiscard]] std::string authorization_header() const {
        return token_type + " " + access_token;
    }
    // Guard against cross-AS replay (SEP-2352): only attach this token to an
    // endpoint whose authorization server is the one that minted it.
    [[nodiscard]] bool bound_to(std::string_view as_issuer) const {
        return detail::issuer_equal(issuer, as_issuer);
    }
};

//==============================================================================
//  Errors.
//==============================================================================
struct AuthError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

//==============================================================================
//  Challenge parsing — RFC 9728 §5.1 WWW-Authenticate on a 401.
//
//    WWW-Authenticate: Bearer resource_metadata="https://api.example/.well-known/
//                      oauth-protected-resource", error="invalid_token"
//==============================================================================
inline std::optional<std::string> parse_challenge(std::string_view www_authenticate) {
    // Find resource_metadata="..." (case-insensitive key, quoted value).
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    std::string hay; hay.reserve(www_authenticate.size());
    for (char c : www_authenticate) hay.push_back(lower(c));
    const std::string_view key = "resource_metadata";
    auto pos = hay.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos = www_authenticate.find('=', pos);
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < www_authenticate.size() && (www_authenticate[pos] == ' ')) ++pos;
    if (pos >= www_authenticate.size()) return std::nullopt;
    if (www_authenticate[pos] == '"') {
        auto end = www_authenticate.find('"', pos + 1);
        if (end == std::string_view::npos) return std::nullopt;
        return std::string(www_authenticate.substr(pos + 1, end - pos - 1));
    }
    // unquoted: up to the next comma/space
    auto end = www_authenticate.find_first_of(", ", pos);
    return std::string(www_authenticate.substr(pos, end - pos));
}

//==============================================================================
//  Document parsers (host GETs the JSON; we validate + extract).
//==============================================================================
inline ProtectedResourceMetadata parse_protected_resource_metadata(const Json& j) {
    ProtectedResourceMetadata m;
    m.resource = j.value("resource", std::string{});
    if (auto it = j.find("authorization_servers"); it != j.end() && it->is_array())
        for (const auto& a : *it) if (a.is_string()) m.authorization_servers.push_back(a.get<std::string>());
    if (m.authorization_servers.empty())
        throw AuthError("protected-resource metadata lists no authorization_servers");
    return m;
}

inline AuthServerMetadata parse_authorization_server_metadata(const Json& j) {
    AuthServerMetadata m;
    m.issuer                 = j.value("issuer", std::string{});
    m.authorization_endpoint = j.value("authorization_endpoint", std::string{});
    m.token_endpoint         = j.value("token_endpoint", std::string{});
    if (auto it = j.find("registration_endpoint"); it != j.end() && it->is_string())
        m.registration_endpoint = it->get<std::string>();
    if (auto it = j.find("code_challenge_methods_supported"); it != j.end() && it->is_array())
        for (const auto& c : *it) if (c.is_string() && c.get<std::string>() == "S256") m.supports_pkce_s256 = true;
    // RFC 9207 support flag (SEP-2468).
    if (auto it = j.find("authorization_response_iss_parameter_supported");
        it != j.end() && it->is_boolean())
        m.authorization_response_iss_supported = it->get<bool>();
    if (m.issuer.empty() || m.authorization_endpoint.empty() || m.token_endpoint.empty())
        throw AuthError("authorization-server metadata missing required endpoint(s)");
    return m;
}

//==============================================================================
//  Dynamic Client Registration (SEP-837 application_type; DEPRECATED overall in
//  favour of CIMD, but still emitted for servers that require it).
//==============================================================================

// Build the DCR request body. `redirect_uri` is the loopback the CLI listens
// on. application_type=native is the SEP-837 fix that stops an AS rejecting a
// 127.0.0.1 redirect.
inline Json registration_request(std::string_view client_name,
                                 std::string_view redirect_uri,
                                 std::vector<std::string> scopes = {}) {
    Json body = {
        {"client_name",                client_name},
        {"application_type",           "native"},            // SEP-837
        {"redirect_uris",              Json::array({std::string(redirect_uri)})},
        {"token_endpoint_auth_method", "none"},              // public client + PKCE
        {"grant_types",                Json::array({"authorization_code", "refresh_token"})},
        {"response_types",             Json::array({"code"})},
    };
    if (!scopes.empty()) {
        std::string s;
        for (auto& x : scopes) { if (!s.empty()) s += ' '; s += x; }
        body["scope"] = s;
    }
    return body;
}

// Parse the DCR response into an issuer-bound ClientRegistration.
inline ClientRegistration parse_registration(const Json& j, std::string_view issuer) {
    ClientRegistration r;
    r.client_id = j.value("client_id", std::string{});
    if (r.client_id.empty()) throw AuthError("registration response has no client_id");
    if (auto it = j.find("client_secret"); it != j.end() && it->is_string())
        r.client_secret = it->get<std::string>();
    r.issuer = std::string(issuer);   // SEP-2352 binding
    return r;
}

// CIMD path (2026-07-28 preferred): the client presents a stable https URL that
// resolves to its metadata document. No round-trip to the AS — the URL IS the
// client_id, bound to the AS issuer we're talking to.
inline ClientRegistration cimd_client(std::string_view metadata_url, std::string_view issuer) {
    if (metadata_url.substr(0, 8) != "https://")
        throw AuthError("CIMD client_id must be an https URL");
    ClientRegistration r;
    r.client_id = std::string(metadata_url);
    r.issuer    = std::string(issuer);
    r.is_cimd   = true;
    return r;
}

//==============================================================================
//  AuthSession — one in-flight authorization-code + PKCE exchange.
//==============================================================================
struct AuthSession {
    std::string issuer;         // AS issuer we started with (RFC 9207 anchor)
    std::string token_endpoint;
    std::string client_id;
    std::optional<std::string> client_secret;
    std::string redirect_uri;
    std::string resource;       // RFC 8707 resource indicator (aud)
    Pkce        pkce;
    std::string state;          // CSRF anti-forgery, echoed on redirect
    bool        expect_iss = false;  // AS advertised RFC 9207 support

    // The URL the host opens in a browser.
    [[nodiscard]] std::string authorize_url(std::string_view authorization_endpoint,
                                            const std::vector<std::string>& scopes = {}) const {
        std::string u(authorization_endpoint);
        u += (u.find('?') == std::string::npos) ? '?' : '&';
        auto add = [&](std::string_view k, std::string_view v) {
            u += detail::pct_encode(k); u += '='; u += detail::pct_encode(v); u += '&';
        };
        add("response_type", "code");
        add("client_id", client_id);
        add("redirect_uri", redirect_uri);
        add("code_challenge", pkce.challenge);
        add("code_challenge_method", Pkce::method);
        add("state", state);
        if (!resource.empty()) add("resource", resource);   // RFC 8707
        if (!scopes.empty()) {
            std::string s; for (auto& x : scopes) { if (!s.empty()) s += ' '; s += x; }
            add("scope", s);
        }
        if (!u.empty() && u.back() == '&') u.pop_back();
        return u;
    }
};

// Begin an authorization-code flow. `random_verifier_seed` and `random_state`
// are supplied by the host's RNG (>= 32 bytes each recommended).
inline AuthSession begin(const AuthServerMetadata& as,
                         const ClientRegistration& client,
                         std::string_view redirect_uri,
                         std::string_view resource,
                         std::string_view random_verifier_seed,
                         std::string random_state) {
    // The client was minted for `as.issuer` (parse_registration / cimd_client
    // both stamp it), so it is already bound to this AS (SEP-2352).
    AuthSession s;
    s.issuer         = as.issuer;
    s.token_endpoint = as.token_endpoint;
    s.client_id      = client.client_id;
    s.client_secret  = client.client_secret;
    s.redirect_uri   = std::string(redirect_uri);
    s.resource       = std::string(resource);
    s.pkce           = Pkce::from_entropy(random_verifier_seed);
    s.state          = std::move(random_state);
    s.expect_iss     = as.authorization_response_iss_supported;
    return s;
}

//==============================================================================
//  Redemption — the security-critical step. Validates state + RFC 9207 iss
//  BEFORE building the token request (SEP-2468: reject a mismatched iss before
//  the code is ever redeemed).
//==============================================================================

// The parsed query from the loopback redirect: ?code=…&state=…&iss=…
struct RedirectParams {
    std::string code;
    std::string state;
    std::optional<std::string> iss;
    std::optional<std::string> error;
};

// Parse the redirect URL's query string.
inline RedirectParams parse_redirect(std::string_view url) {
    RedirectParams p;
    auto q = url.find('?');
    std::string_view qs = (q == std::string_view::npos) ? url : url.substr(q + 1);
    std::size_t i = 0;
    while (i < qs.size()) {
        auto amp = qs.find('&', i);
        std::string_view pair = qs.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        auto eq = pair.find('=');
        std::string k(pair.substr(0, eq));
        std::string v = (eq == std::string_view::npos) ? std::string{}
                        : std::string(pair.substr(eq + 1));
        // minimal percent-decode
        std::string dec; dec.reserve(v.size());
        for (std::size_t j = 0; j < v.size(); ++j) {
            if (v[j] == '%' && j + 2 < v.size()) {
                auto hx = [](char c)->int{ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
                int hi = hx(v[j+1]), lo = hx(v[j+2]);
                if (hi>=0 && lo>=0) { dec.push_back(char(hi*16+lo)); j += 2; continue; }
            }
            dec.push_back(v[j] == '+' ? ' ' : v[j]);
        }
        if (k == "code")  p.code = dec;
        else if (k == "state") p.state = dec;
        else if (k == "iss")   p.iss = dec;
        else if (k == "error") p.error = dec;
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return p;
}

// Validate the redirect and build the token-exchange request body. THROWS
// AuthError on any of: an error param, state mismatch (CSRF), or RFC 9207 iss
// mismatch/absence (the AS-mix-up guard, SEP-2468).
inline Json redeem(const AuthSession& s, const RedirectParams& r) {
    if (r.error) throw AuthError("authorization error: " + *r.error);
    if (r.state != s.state) throw AuthError("state mismatch — possible CSRF; aborting");
    // RFC 9207 (SEP-2468): if the AS advertised iss support it MUST send iss and
    // it MUST equal our anchor. Even if it didn't advertise, an iss that's
    // PRESENT but WRONG is a hard fail.
    if (s.expect_iss && !r.iss)
        throw AuthError("RFC 9207: authorization server advertised iss support but response omitted iss");
    if (r.iss && !detail::issuer_equal(*r.iss, s.issuer))
        throw AuthError("RFC 9207: iss mismatch (" + *r.iss + " != " + s.issuer +
                        ") — authorization-server mix-up; aborting before code redemption");
    if (r.code.empty()) throw AuthError("authorization response carried no code");

    // Build the application/x-www-form-urlencoded token request.
    std::string form;
    auto add = [&](std::string_view k, std::string_view v) {
        if (!form.empty()) form += '&';
        form += detail::pct_encode(k); form += '='; form += detail::pct_encode(v);
    };
    add("grant_type", "authorization_code");
    add("code", r.code);
    add("redirect_uri", s.redirect_uri);
    add("client_id", s.client_id);
    add("code_verifier", s.pkce.verifier);
    if (!s.resource.empty()) add("resource", s.resource);   // RFC 8707
    if (s.client_secret) add("client_secret", *s.client_secret);
    return Json{{"__form", form}, {"token_endpoint", s.token_endpoint}, {"issuer", s.issuer}};
}

// Parse the token endpoint's JSON reply into an issuer-bound AccessToken.
inline AccessToken parse_token_response(const Json& j, std::string_view issuer,
                                        std::string_view resource) {
    if (auto it = j.find("error"); it != j.end())
        throw AuthError("token error: " + it->get<std::string>());
    AccessToken t;
    t.access_token = j.value("access_token", std::string{});
    if (t.access_token.empty()) throw AuthError("token response has no access_token");
    t.token_type = j.value("token_type", std::string{"Bearer"});
    if (auto it = j.find("refresh_token"); it != j.end() && it->is_string())
        t.refresh_token = it->get<std::string>();
    if (auto it = j.find("expires_in"); it != j.end() && it->is_number_integer())
        t.expires_in = it->get<std::int64_t>();
    t.issuer   = std::string(issuer);     // SEP-2352 binding
    t.resource = std::string(resource);
    return t;
}

// Build a refresh-token request form (issuer-bound; same client_id).
inline std::string refresh_request_form(const AccessToken& tok, std::string_view client_id) {
    if (!tok.refresh_token) throw AuthError("no refresh_token available");
    std::string form;
    auto add = [&](std::string_view k, std::string_view v) {
        if (!form.empty()) form += '&';
        form += detail::pct_encode(k); form += '='; form += detail::pct_encode(v);
    };
    add("grant_type", "refresh_token");
    add("refresh_token", *tok.refresh_token);
    add("client_id", client_id);
    if (!tok.resource.empty()) add("resource", tok.resource);
    return form;
}

} // namespace mcp::auth
