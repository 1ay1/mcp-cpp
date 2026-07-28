// SPDX-License-Identifier: Apache-2.0
//
// auth_test.cpp — MCP 2026-07-28 authorization (mcp/auth.hpp). Pure logic, no
// network: verifies the crypto primitives and every 2026-07-28 hardening rule.
//
#include <mcp/auth.hpp>

#include <iostream>
#include <string>

using namespace mcp;
using namespace mcp::auth;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __LINE__ << "  " << #cond << "\n";       \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static std::string hex(const std::array<std::uint8_t, 32>& d) {
    static constexpr char h[] = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (auto b : d) { s.push_back(h[b >> 4]); s.push_back(h[b & 0xF]); }
    return s;
}

int main() {
    // ── SHA-256 known-answer vectors (FIPS 180-4) ────────────────────────
    {
        CHECK(hex(detail::sha256("")) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(hex(detail::sha256("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(hex(detail::sha256(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }

    // ── base64url + pct-encode ───────────────────────────────────────────
    {
        CHECK(detail::b64url("") == "");
        CHECK(detail::b64url("f") == "Zg");
        CHECK(detail::b64url("fo") == "Zm8");
        CHECK(detail::b64url("foo") == "Zm9v");
        CHECK(detail::b64url("foob") == "Zm9vYg");
        // no padding, url-safe alphabet
        CHECK(detail::b64url(std::string("\xff\xff\xfe", 3)).find('+') == std::string::npos);
        CHECK(detail::pct_encode("a b/c?d") == "a%20b%2Fc%3Fd");
        CHECK(detail::pct_encode("Aa0-_.~") == "Aa0-_.~");
    }

    // ── issuer normalization / equality (RFC 9207 comparison) ────────────
    {
        CHECK(issuer_equal("https://as.example.com", "https://as.example.com/"));
        CHECK(issuer_equal("https://AS.Example.COM", "https://as.example.com"));
        CHECK(!issuer_equal("https://as.example.com", "https://evil.example.com"));
        // path case is preserved (tenant namespacing)
        CHECK(!issuer_equal("https://as.example.com/TenantA", "https://as.example.com/tenanta"));
    }

    // ── PKCE S256 ────────────────────────────────────────────────────────
    {
        Pkce p = Pkce::from_entropy("0123456789abcdef0123456789abcdef");  // 32 bytes
        CHECK(p.verifier.size() >= 43 && p.verifier.size() <= 128);
        CHECK(!p.challenge.empty() && p.challenge != p.verifier);
        CHECK(Pkce::method == "S256");
        // challenge == b64url(sha256(verifier))
        auto dg = detail::sha256(p.verifier);
        CHECK(p.challenge == detail::b64url(
            std::string_view{reinterpret_cast<const char*>(dg.data()), dg.size()}));
    }

    // ── WWW-Authenticate challenge parse (RFC 9728) ──────────────────────
    {
        auto u = parse_challenge(
            "Bearer resource_metadata=\"https://api.ex/.well-known/oauth-protected-resource\", "
            "error=\"invalid_token\"");
        CHECK(u.has_value() && *u == "https://api.ex/.well-known/oauth-protected-resource");
        // case-insensitive key
        auto u2 = parse_challenge("Bearer RESOURCE_METADATA=\"https://x/y\"");
        CHECK(u2.has_value() && *u2 == "https://x/y");
        CHECK(!parse_challenge("Bearer error=\"invalid_token\"").has_value());
    }

    // ── metadata parsers ─────────────────────────────────────────────────
    {
        Json prm = {{"resource", "https://api.ex/mcp"},
                    {"authorization_servers", {"https://as.ex"}}};
        auto m = parse_protected_resource_metadata(prm);
        CHECK(m.resource == "https://api.ex/mcp");
        CHECK(m.authorization_servers.size() == 1 && m.authorization_servers[0] == "https://as.ex");

        Json asm_ = {
            {"issuer", "https://as.ex"},
            {"authorization_endpoint", "https://as.ex/authorize"},
            {"token_endpoint", "https://as.ex/token"},
            {"registration_endpoint", "https://as.ex/register"},
            {"code_challenge_methods_supported", {"S256"}},
            {"authorization_response_iss_parameter_supported", true}};
        auto as = parse_authorization_server_metadata(asm_);
        CHECK(as.issuer == "https://as.ex");
        CHECK(as.token_endpoint == "https://as.ex/token");
        CHECK(as.registration_endpoint.has_value());
        CHECK(as.supports_pkce_s256);
        CHECK(as.authorization_response_iss_supported);

        // missing endpoint → throws
        bool threw = false;
        try { parse_authorization_server_metadata(Json{{"issuer", "x"}}); }
        catch (const AuthError&) { threw = true; }
        CHECK(threw);
    }

    // ── DCR request has SEP-837 application_type=native ───────────────────
    {
        Json body = registration_request("agentty", "http://127.0.0.1:8976/callback",
                                         {"mcp:tools"});
        CHECK(body["application_type"] == "native");        // SEP-837
        CHECK(body["token_endpoint_auth_method"] == "none"); // public + PKCE
        CHECK(body["redirect_uris"][0] == "http://127.0.0.1:8976/callback");
        CHECK(body["scope"] == "mcp:tools");

        auto reg = parse_registration(Json{{"client_id", "abc123"}}, "https://as.ex");
        CHECK(reg.client_id == "abc123");
        CHECK(reg.issuer == "https://as.ex");   // SEP-2352 binding
        CHECK(!reg.is_cimd);
    }

    // ── CIMD client (2026-07-28 preferred) ───────────────────────────────
    {
        auto c = cimd_client("https://agentty.dev/mcp-client.json", "https://as.ex");
        CHECK(c.is_cimd);
        CHECK(c.client_id == "https://agentty.dev/mcp-client.json");
        CHECK(c.issuer == "https://as.ex");
        bool threw = false;
        try { cimd_client("http://insecure/x", "https://as.ex"); }
        catch (const AuthError&) { threw = true; }
        CHECK(threw);   // must be https
    }

    // ── full flow: begin → authorize_url → redeem ────────────────────────
    AuthServerMetadata as;
    as.issuer = "https://as.ex";
    as.authorization_endpoint = "https://as.ex/authorize";
    as.token_endpoint = "https://as.ex/token";
    as.authorization_response_iss_supported = true;   // RFC 9207
    ClientRegistration client = cimd_client("https://agentty.dev/c.json", "https://as.ex");

    AuthSession sess = begin(as, client, "http://127.0.0.1:8976/callback",
                             "https://api.ex/mcp",
                             "0123456789abcdef0123456789abcdef", "state-xyz-123");
    {
        std::string url = sess.authorize_url(as.authorization_endpoint, {"mcp:tools"});
        CHECK(url.find("response_type=code") != std::string::npos);
        CHECK(url.find("code_challenge_method=S256") != std::string::npos);
        CHECK(url.find("state=state-xyz-123") != std::string::npos);
        CHECK(url.find("resource=https%3A%2F%2Fapi.ex%2Fmcp") != std::string::npos);
        CHECK(url.find(detail::pct_encode(sess.pkce.challenge)) != std::string::npos);
    }

    // Happy path: matching iss + state → token request.
    {
        RedirectParams r = parse_redirect(
            "http://127.0.0.1:8976/callback?code=AUTHCODE&state=state-xyz-123&iss=https%3A%2F%2Fas.ex");
        CHECK(r.code == "AUTHCODE");
        CHECK(r.state == "state-xyz-123");
        CHECK(r.iss.has_value() && *r.iss == "https://as.ex");
        Json req = redeem(sess, r);
        CHECK(req["token_endpoint"] == "https://as.ex/token");
        const std::string form = req["__form"];
        CHECK(form.find("grant_type=authorization_code") != std::string::npos);
        CHECK(form.find("code=AUTHCODE") != std::string::npos);
        CHECK(form.find("code_verifier=") != std::string::npos);
    }

    // SECURITY: RFC 9207 iss MISMATCH must be rejected BEFORE redemption.
    {
        RedirectParams r = parse_redirect(
            "http://127.0.0.1:8976/callback?code=STOLEN&state=state-xyz-123&iss=https%3A%2F%2Fevil.ex");
        bool threw = false;
        try { redeem(sess, r); } catch (const AuthError& e) {
            threw = std::string(e.what()).find("iss mismatch") != std::string::npos;
        }
        CHECK(threw);   // AS mix-up attack blocked (SEP-2468)
    }

    // SECURITY: advertised-iss-support but iss ABSENT must fail.
    {
        RedirectParams r = parse_redirect(
            "http://127.0.0.1:8976/callback?code=X&state=state-xyz-123");
        bool threw = false;
        try { redeem(sess, r); } catch (const AuthError&) { threw = true; }
        CHECK(threw);
    }

    // SECURITY: state mismatch (CSRF) must fail.
    {
        RedirectParams r = parse_redirect(
            "http://127.0.0.1:8976/callback?code=X&state=WRONG&iss=https%3A%2F%2Fas.ex");
        bool threw = false;
        try { redeem(sess, r); } catch (const AuthError& e) {
            threw = std::string(e.what()).find("state mismatch") != std::string::npos;
        }
        CHECK(threw);
    }

    // ── token parse (issuer-bound) + replay guard ────────────────────────
    {
        Json tok = {{"access_token", "ACCESS"}, {"token_type", "Bearer"},
                    {"refresh_token", "REFRESH"}, {"expires_in", 3600}};
        AccessToken t = parse_token_response(tok, "https://as.ex", "https://api.ex/mcp");
        CHECK(t.access_token == "ACCESS");
        CHECK(t.authorization_header() == "Bearer ACCESS");
        CHECK(t.refresh_token.has_value() && *t.refresh_token == "REFRESH");
        CHECK(t.expires_in.has_value() && *t.expires_in == 3600);
        CHECK(t.issuer == "https://as.ex");
        // SEP-2352: bound to its AS, not to another.
        CHECK(t.bound_to("https://as.ex/"));
        CHECK(!t.bound_to("https://other.ex"));

        std::string rf = refresh_request_form(t, client.client_id);
        CHECK(rf.find("grant_type=refresh_token") != std::string::npos);
        CHECK(rf.find("refresh_token=REFRESH") != std::string::npos);

        // token error → throws
        bool threw = false;
        try { parse_token_response(Json{{"error", "invalid_grant"}}, "https://as.ex", ""); }
        catch (const AuthError&) { threw = true; }
        CHECK(threw);
    }

    if (g_failures == 0) { std::cout << "auth_test: ALL PASS\n"; return 0; }
    std::cerr << "auth_test: " << g_failures << " failure(s)\n";
    return 1;
}
