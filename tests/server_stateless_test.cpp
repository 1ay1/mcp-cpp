// SPDX-License-Identifier: Apache-2.0
//
// server_stateless_test.cpp — the SERVER half of MCP 2026-07-28, exercised
// through the real mcp::Server and a client on a loopback:
//
//   1. RequestStateCodec: seal → open round-trips; tamper is rejected; a wrong
//      secret is rejected.
//   2. Server::server/discover advertises supportedVersions + serverInfo +
//      cache hint out of the box.
//   3. IncomingRequest reads the modern per-request _meta (protocolVersion,
//      clientInfo) with no initialize handshake.
//   4. A real Server tool emits input_required(...) with a signed requestState,
//      and the client's MRTR loop (mrtr_retry) completes the
//      round — the server reconstructs its context purely from requestState.
//   5. Cacheable tools/list carries ttlMs / cacheScope.
//
#include "agtest.hpp"
#include "wire.hpp"

static int g_failures = 0;

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

using namespace mcp;


TEST_CASE("server_stateless") {
    // ── 1. RequestStateCodec integrity ───────────────────────────────────
    {
        RequestStateCodec codec("test-secret");
        Json state = {{"step", 2}, {"user", "Ada"}, {"n", 42}};
        std::string sealed = codec.seal(state);
        CHECK(!sealed.empty());
        CHECK(sealed.find('.') != std::string::npos);

        auto opened = codec.open(sealed);
        CHECK(opened.has_value());
        CHECK((*opened)["step"] == 2);
        CHECK((*opened)["user"] == "Ada");

        // Tamper the payload → verification fails.
        std::string tampered = sealed;
        tampered[0] = (tampered[0] == 'A' ? 'B' : 'A');
        CHECK(!codec.open(tampered).has_value());

        // A different secret cannot open it.
        RequestStateCodec other("other-secret");
        CHECK(!other.open(sealed).has_value());

        // Garbage is rejected, not crashed.
        CHECK(!codec.open("not-a-valid-token").has_value());
        CHECK(!codec.open("").has_value());
        CHECK(!codec.open(".").has_value());
        CHECK(!codec.open("AAAA.").has_value());

        // Base64URL round-trips EVERY byte value 0..255 (binary-safe) — the
        // sealed state may embed arbitrary UTF-8 / non-ASCII context. Drive it
        // through a JSON string field so the whole path is exercised.
        std::string all_bytes;
        for (int i = 1; i < 256; ++i) all_bytes.push_back(char(i));  // skip NUL (JSON strings)
        Json binstate = {{"blob", all_bytes}, {"len", (int)all_bytes.size()}};
        std::string sb = codec.seal(binstate);
        auto ob = codec.open(sb);
        CHECK(ob.has_value());
        CHECK((*ob)["blob"].get<std::string>() == all_bytes);

        // Length variety: payloads of every residue mod 3 (b64 tail cases).
        for (int len = 0; len <= 6; ++len) {
            Json s = {{"pad", std::string(static_cast<std::size_t>(len), 'x')}};
            CHECK(codec.open(codec.seal(s)).has_value());
        }

        // The MAC is endianness-stable by construction (fnv1a_u64 folds the
        // 64-bit inner state MSB-first, no object-representation reinterpret),
        // so a token sealed here verifies on any host of any endianness.
    }

    // ── loopback wiring: a real Server, a client on the test wire ───────
    Server server(Implementation{"srv", "2.0", Nothing, Nothing, Nothing, Nothing});
    server.set_instructions("stateless demo");
    server.set_state_secret("server-side-secret");
    server.set_discover_cache(CacheHint{3600000, "public"});

    // Capture what the server sees on the wire.
    Json last_meta = Json::object();

    Tool greet_spec;
    greet_spec.name = "greet";
    greet_spec.description = "greets you after asking your name";

    // A raw tools/call so MRTR is driven by hand through the server-side
    // primitives: round 1 emits input_required with a SIGNED requestState,
    // round 2 verifies that state and reads the fulfilled elicitation.
    server.on_raw(std::string(to_server::CallTool::name), [&](const Json& params) -> Json {
        IncomingRequest req(params);
        last_meta = req.meta();
        CHECK(req.version_supported());
        auto state = req.request_state(server.state_codec());
        if (!state) {
            ElicitFormParams fp;
            fp.message = "Your name?";
            fp.properties.emplace_back("name",
                PrimitiveSchema{StringSchema{std::string("Name"), Nothing,
                                             Nothing, Nothing, Nothing, Nothing}});
            Json sealed = server.state_codec().seal(Json{{"pending", "name"}});
            Json requests = {
                {"need_name", input_request(method::Elicit, to_json(ElicitParams{fp}))}};
            return input_required(std::move(requests), sealed.get<std::string>());
        }
        CHECK((*state)["pending"] == "name");
        std::string name = "unknown";
        Json er = req.input_response("need_name");
        if (er.is_object() && er.contains("content") && er["content"].contains("name"))
            name = er["content"]["name"].get<std::string>();
        CallToolResult r;
        r.content.push_back(text(std::string("hello ") + name));
        return to_json(r);
    });
    // Cacheable tools/list.
    server.on_raw(std::string(to_server::ListTools::name), [&](const Json&) -> Json {
        ListToolsResult r;
        r.tools.push_back(greet_spec);
        r.ttlMs = 60000;
        r.cacheScope = "public";
        return to_json(r);
    });

    test::Wire w;
    w.answer = [&server](const Call& c) { return server.handle(c); };
    bool elicited = false;
    w.handlers.on_elicit = [&](const ElicitParams&) -> ElicitResult {
        elicited = true;
        ElicitResult r;
        r.action = ElicitAction::Accept;
        r.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Grace")}}};
        return r;
    };
    w.client.set_request_meta(modern_request_meta(
        Implementation{"cli", "1.0", Nothing, Nothing, Nothing, Nothing},
        ClientCapabilities{}));

    // ── 2. server/discover from the SDK Server ───────────────────────────
    {
        auto d = w.call<to_server::Discover>(DiscoverParams{});
        REQUIRE(d.has_value());
        CHECK(d->resultType == "complete");
        CHECK(d->supportedVersions.size() >= 2);
        CHECK(d->supportedVersions[0] == std::string(kProtocolVersion));
        CHECK(d->instructions.has_value() && *d->instructions == "stateless demo");
        CHECK(d->ttlMs == 3600000);
        CHECK(d->cacheScope == "public");
        CHECK(d->meta.contains(std::string(meta_key::ServerInfo)));
        CHECK(d->meta[std::string(meta_key::ServerInfo)]["name"] == "srv");
    }

    // ── 3+4. MRTR through the real Server primitives ─────────────────────
    {
        auto raw = w.call_mrtr(to_server::CallTool::name,
                               to_json(CallToolParams{"greet", Json::object(), Nothing, Json::object()}));
        REQUIRE(raw.has_value());
        auto r = from_json<CallToolResult>(*raw);
        CHECK(elicited);
        CHECK(r.content.size() == 1);
        CHECK(std::get<TextContent>(r.content[0]).text == "hello Grace");
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
        CHECK(last_meta[std::string(meta_key::ProtocolVersion)] == std::string(kProtocolVersion));
        CHECK(last_meta.contains(std::string(meta_key::ClientInfo)));
    }

    // ── 5. cacheable tools/list ──────────────────────────────────────────
    {
        auto lt = w.call<to_server::ListTools>(ListToolsParams{});
        REQUIRE(lt.has_value());
        CHECK(lt->tools.size() == 1);
        CHECK(lt->ttlMs == 60000);
        CHECK(lt->cacheScope == "public");
    }

    // ── 6. capability enforcement (MissingRequiredClientCapability) ──────
    {
        // Client declared elicitation (ClientHandlers.on_elicit → capability).
        Json meta = Json::object();
        meta[std::string(meta_key::ClientCapabilities)] =
            to_json(ClientCapabilities{Nothing, Nothing,
                                       SamplingCapability{}, ElicitationCapability{}, Nothing});
        Json params = {{"name", "x"}, {"_meta", meta}};
        IncomingRequest req(params);
        // has elicitation + sampling → OK
        bool ok = true;
        try { require_capabilities(req, {ClientCap::Elicitation, ClientCap::Sampling}); }
        catch (...) { ok = false; }
        CHECK(ok);
        // requires roots → throws MissingRequiredClientCapability
        bool threw = false;
        try { require_capabilities(req, {ClientCap::Roots}); }
        catch (const RpcError& e) {
            threw = (e.code == errc::MissingRequiredClientCapability);
        }
        CHECK(threw);
        // No caps declared at all → don't over-reject.
        IncomingRequest bare(Json{{"name", "x"}});
        bool ok2 = true;
        try { require_capabilities(bare, {ClientCap::Roots}); } catch (...) { ok2 = false; }
        CHECK(ok2);
    }

    // ── 7. header-based routing validation (SEP-2243) ────────────────────
    {
        Json body = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                     {"params", {{"name", "read"}, {"arguments", Json::object()}}}};
        std::string err;
        // matching headers → OK
        CHECK(routing_headers_match(RoutingHeaders{"tools/call", "read"}, body, err));
        // absent headers → allowed (MAY be omitted)
        CHECK(routing_headers_match(RoutingHeaders{"", ""}, body, err));
        // wrong method header → rejected
        CHECK(!routing_headers_match(RoutingHeaders{"tools/list", ""}, body, err));
        CHECK(!err.empty());
        // wrong tool name → rejected (confused-deputy guard)
        err.clear();
        CHECK(!routing_headers_match(RoutingHeaders{"tools/call", "bash"}, body, err));
        CHECK(!err.empty());
        // routing_from_body extracts both
        auto rf = routing_from_body(body);
        CHECK(rf.method == "tools/call" && rf.name == "read");
    }
    CHECK(g_failures == 0);
}
