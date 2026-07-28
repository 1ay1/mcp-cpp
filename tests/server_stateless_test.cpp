// SPDX-License-Identifier: Apache-2.0
//
// server_stateless_test.cpp — the SERVER half of MCP 2026-07-28, exercised
// through the real mcp::Server + mcp::Client over a loopback:
//
//   1. RequestStateCodec: seal → open round-trips; tamper is rejected; a wrong
//      secret is rejected.
//   2. Server::server/discover advertises supportedVersions + serverInfo +
//      cache hint out of the box.
//   3. IncomingRequest reads the modern per-request _meta (protocolVersion,
//      clientInfo) with no initialize handshake.
//   4. A real Server tool emits input_required(...) with a signed requestState,
//      and the SDK Client's MRTR driver (call_tool_interactive) completes the
//      round — the server reconstructs its context purely from requestState.
//   5. Cacheable tools/list carries ttlMs / cacheScope.
//
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

using namespace mcp;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __LINE__ << "  " << #cond << "\n";       \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main() {
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
            Json s = {{"pad", std::string(len, 'x')}};
            CHECK(codec.open(codec.seal(s)).has_value());
        }

        // The MAC is endianness-stable by construction (fnv1a_u64 folds the
        // 64-bit inner state MSB-first, no object-representation reinterpret),
        // so a token sealed here verifies on any host of any endianness.
    }

    // ── loopback wiring: real Server ⟷ real Client ───────────────────────
    RpcEngine* client_engine = nullptr;
    Server*    server_ptr    = nullptr;

    Transport to_server = [&](std::string_view f) { server_ptr->engine().feed_line(f); };
    Transport to_client = [&](std::string_view f) { client_engine->feed_line(f); };

    Server server(to_client,
                  Implementation{"srv", "2.0", Nothing, Nothing, Nothing, Nothing});
    server_ptr = &server;
    server.set_instructions("stateless demo");
    server.set_state_secret("server-side-secret");
    server.set_discover_cache(CacheHint{3600000, "public"});

    // Capture what the server sees on the wire.
    Json last_meta = Json::object();

    // A real tool that runs an MRTR elicitation. Round 1 emits input_required
    // with a SIGNED requestState carrying the "pending" flag; round 2 verifies
    // that state + reads the fulfilled elicitation answer.
    Tool greet_spec;
    greet_spec.name = "greet";
    greet_spec.description = "greets you after asking your name";

    // We install a raw tools/call handler so we can drive MRTR by hand through
    // the server-side primitives (register_tool wraps a plain invoke that can't
    // emit input_required).
    server.engine().on_request(std::string(method::CallTool),
        [&](const RpcId&, const Json& params) -> Maybe<Json> {
            IncomingRequest req(params);
            last_meta = req.meta();

            // The client MUST declare a modern protocol version we serve.
            CHECK(req.version_supported());

            // Resume from prior state, if any.
            auto state = req.request_state(server.state_codec());
            if (!state) {
                // Round 1: ask the client to elicit a name.
                ElicitFormParams fp;
                fp.message = "Your name?";
                fp.properties.emplace_back("name",
                    PrimitiveSchema{StringSchema{std::string("Name"), Nothing,
                                                 Nothing, Nothing, Nothing, Nothing}});
                Json sealed = server.state_codec().seal(Json{{"pending", "name"}});
                Json requests = {
                    {"need_name", input_request(method::Elicit, to_json(ElicitParams{fp}))}};
                return Just(input_required(std::move(requests), sealed.get<std::string>()));
            }

            // Round 2: state verified. Read the fulfilled elicitation answer.
            CHECK((*state)["pending"] == "name");
            std::string name = "unknown";
            Json er = req.input_response("need_name");
            if (er.is_object() && er.contains("content") && er["content"].contains("name"))
                name = er["content"]["name"].get<std::string>();

            CallToolResult r;
            r.content.push_back(text(std::string("hello ") + name));
            return Just(to_json(r));
        });

    // Cacheable tools/list.
    server.engine().on_request(std::string(method::ListTools),
        [&](const RpcId&, const Json&) -> Maybe<Json> {
            ListToolsResult r;
            r.tools.push_back(greet_spec);
            r.ttlMs = 60000;
            r.cacheScope = "public";
            return Just(to_json(r));
        });

    // ── client with an elicitation handler for MRTR fulfilment ───────────
    ClientHandlers ch;
    bool elicited = false;
    ch.on_elicit = [&](const ElicitParams&) -> ElicitResult {
        elicited = true;
        ElicitResult r;
        r.action = ElicitAction::Accept;
        r.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Grace")}}};
        return r;
    };
    Client c(to_server, std::move(ch));
    client_engine = &c.engine();
    c.enable_modern_metadata(
        Implementation{"cli", "1.0", Nothing, Nothing, Nothing, Nothing},
        ClientCapabilities{});

    // ── 2. server/discover from the SDK Server ───────────────────────────
    {
        auto d = c.discover().get();
        CHECK(d.resultType == "complete");
        CHECK(d.supportedVersions.size() >= 2);
        CHECK(d.supportedVersions[0] == std::string(kProtocolVersion));
        CHECK(d.instructions.has_value() && *d.instructions == "stateless demo");
        CHECK(d.ttlMs.has_value() && *d.ttlMs == 3600000);
        CHECK(d.cacheScope.has_value() && *d.cacheScope == "public");
        // serverInfo travels under _meta.
        CHECK(d.meta.contains(std::string(meta_key::ServerInfo)));
        CHECK(d.meta[std::string(meta_key::ServerInfo)]["name"] == "srv");
    }

    // ── 3+4. MRTR through the real Server primitives ─────────────────────
    {
        CallToolResult r = c.call_tool_interactive("greet", Json::object());
        CHECK(elicited);
        CHECK(r.content.size() == 1);
        CHECK(std::holds_alternative<TextContent>(r.content[0]));
        CHECK(std::get<TextContent>(r.content[0]).text == "hello Grace");
        // Server saw modern per-request metadata.
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
        CHECK(last_meta[std::string(meta_key::ProtocolVersion)] == std::string(kProtocolVersion));
        CHECK(last_meta.contains(std::string(meta_key::ClientInfo)));
    }

    // ── 5. cacheable tools/list ──────────────────────────────────────────
    {
        auto lt = c.list_tools().get();
        CHECK(lt.tools.size() == 1);
        CHECK(lt.ttlMs.has_value() && *lt.ttlMs == 60000);
        CHECK(lt.cacheScope.has_value() && *lt.cacheScope == "public");
    }

    if (g_failures == 0) {
        std::cout << "server_stateless_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "server_stateless_test: " << g_failures << " failure(s)\n";
    return 1;
}
