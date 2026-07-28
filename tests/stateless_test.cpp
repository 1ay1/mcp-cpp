// SPDX-License-Identifier: Apache-2.0
//
// stateless_test.cpp — MCP 2026-07-28 stateless-core features:
//   1. per-request _meta injection (protocolVersion / clientInfo /
//      clientCapabilities on EVERY request) via enable_modern_metadata()
//   2. server/discover
//   3. MRTR (Multi Round-Trip Requests): a tool that needs input_required
//      elicitation, fulfilled transparently by call_tool_interactive().
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
    // Loopback: two raw engines that cross-feed. The "server" is hand-rolled
    // so we can inspect incoming _meta and craft input_required results the
    // SDK Server doesn't emit yet.
    RpcEngine* client_engine = nullptr;
    RpcEngine* server_engine = nullptr;

    Transport to_server = [&](std::string_view f) { server_engine->feed_line(f); };
    Transport to_client = [&](std::string_view f) { client_engine->feed_line(f); };

    RpcEngine server(to_client);
    server_engine = &server;

    // Capture the last _meta the server saw, per method.
    Json last_meta = Json::object();
    std::string last_method;

    // ── server/discover handler ──────────────────────────────────────────
    server.on_request(std::string(method::Discover),
        [&](const RpcId&, const Json& params) -> Maybe<Json> {
            last_method = "server/discover";
            last_meta = params.value("_meta", Json::object());
            Json result = {
                {"resultType", "complete"},
                {"supportedVersions", {"2026-07-28", "2025-11-25"}},
                {"capabilities", {{"tools", Json::object()}}},
                {"instructions", "demo stateless server"},
                {"ttlMs", 3600000},
                {"cacheScope", "public"},
                {"_meta", {{std::string(meta_key::ServerInfo),
                            {{"name", "demo-server"}, {"version", "9.9"}}}}},
            };
            return Just(result);
        });

    // ── tools/call handler: MRTR — first call returns input_required asking
    //    for an elicitation; the retry (carrying inputResponses) completes. ──
    server.on_request(std::string(method::CallTool),
        [&](const RpcId&, const Json& params) -> Maybe<Json> {
            last_method = "tools/call";
            last_meta = params.value("_meta", Json::object());
            const Json meta = params.value("_meta", Json::object());
            const std::string irk = std::string(meta_key::InputResponses);
            if (!meta.contains(irk)) {
                // Round 1: ask for a name via a (spec-valid) form elicitation.
                ElicitFormParams fp;
                fp.message = "Your name?";
                fp.properties.emplace_back("name",
                    PrimitiveSchema{StringSchema{std::string("Name"), Nothing,
                                                 Nothing, Nothing, Nothing, Nothing}});
                Json ir = {
                    {"resultType", "input_required"},
                    {"requestState", "opaque-state-42"},
                    {"inputRequests", {
                        {"need_name", {
                            {"method", std::string(method::Elicit)},
                            {"params", to_json(ElicitParams{fp})}}}}},
                };
                return Just(ir);
            }
            // Round 2: the client fulfilled it — echo what we got back.
            const Json& responses = meta[irk];
            std::string name = "unknown";
            if (responses.contains("need_name")) {
                const Json& er = responses["need_name"];
                if (er.contains("content") && er["content"].contains("name"))
                    name = er["content"]["name"].get<std::string>();
            }
            // Also assert the server got the echoed requestState.
            bool state_ok = meta.contains(std::string(meta_key::RequestState))
                && meta[std::string(meta_key::RequestState)] == "opaque-state-42";
            Json result = {
                {"content", Json::array({{{"type", "text"},
                    {"text", std::string("hello ") + name +
                             (state_ok ? " [state-ok]" : " [state-BAD]")}}})}};
            return Just(result);
        });

    // ── Client with an elicitation handler (used by MRTR fulfilment) ──────
    ClientHandlers ch;
    bool elicited = false;
    ch.on_elicit = [&](const ElicitParams&) -> ElicitResult {
        elicited = true;
        ElicitResult r;
        r.action = ElicitAction::Accept;
        r.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Ada")}}};
        return r;
    };
    // The client that drives the session (its engine feeds `server`).
    Client c(to_server, std::move(ch));
    client_engine = &c.engine();

    // Modern metadata: attach protocolVersion + clientInfo + clientCapabilities
    // to every request. NO initialize handshake.
    c.enable_modern_metadata(
        Implementation{"demo-client", "1.0", Nothing, Nothing, Nothing, Nothing},
        ClientCapabilities{});

    // 1. server/discover carries the modern _meta.
    {
        auto d = c.discover().get();
        CHECK(d.resultType == "complete");
        CHECK(d.supportedVersions.size() == 2);
        CHECK(d.supportedVersions[0] == "2026-07-28");
        CHECK(d.instructions.has_value() && *d.instructions == "demo stateless server");
        CHECK(d.ttlMs.has_value() && *d.ttlMs == 3600000);
        CHECK(d.cacheScope.has_value() && *d.cacheScope == "public");
        // The server saw our per-request protocol metadata.
        CHECK(last_method == "server/discover");
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
        CHECK(last_meta[std::string(meta_key::ProtocolVersion)] == "2026-07-28");
        CHECK(last_meta.contains(std::string(meta_key::ClientInfo)));
        CHECK(last_meta[std::string(meta_key::ClientInfo)]["name"] == "demo-client");
        CHECK(last_meta.contains(std::string(meta_key::ClientCapabilities)));
    }

    // 2. MRTR: call_tool_interactive drives the input_required round.
    {
        CallToolResult r = c.call_tool_interactive("greet", Json::object());
        CHECK(elicited);
        CHECK(r.content.size() == 1);
        CHECK(std::holds_alternative<TextContent>(r.content[0]));
        const std::string& txt = std::get<TextContent>(r.content[0]).text;
        CHECK(txt == "hello Ada [state-ok]");
        // The final (retry) request STILL carried the modern protocol metadata.
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
    }

    // 3. disable_modern_metadata clears the per-request _meta.
    {
        c.disable_modern_metadata();
        // A plain (non-interactive) call now carries no injected protocol meta.
        server.on_request(std::string(method::Ping),
            [&](const RpcId&, const Json& params) -> Maybe<Json> {
                last_meta = params.value("_meta", Json::object());
                return Just(Json::object());
            });
        c.ping().get();
        CHECK(!last_meta.contains(std::string(meta_key::ProtocolVersion)));
    }

    if (g_failures == 0) {
        std::cout << "stateless_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "stateless_test: " << g_failures << " failure(s)\n";
    return 1;
}
