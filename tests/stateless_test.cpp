// SPDX-License-Identifier: Apache-2.0
//
// stateless_test.cpp — MCP 2026-07-28 stateless-core features:
//   1. per-request _meta injection (protocolVersion / clientInfo /
//      clientCapabilities on EVERY request) via enable_modern_metadata()
//   2. server/discover
//   3. MRTR (Multi Round-Trip Requests): a tool that needs input_required
//      elicitation, fulfilled transparently by call_tool_interactive().
//
#include "agtest.hpp"
#include "wire.hpp"

static int g_failures = 0;

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

using namespace mcp;


TEST_CASE("stateless") {
    // A hand-rolled server, so the test can inspect the incoming _meta and
    // craft input_required results.
    Json last_meta = Json::object();
    std::string last_method;

    struct Srv {} srv;
    Router<Srv> router;
    router.on_raw(std::string(to_server::Discover::name), [&](Srv&, const Json& params) -> Json {
        last_method = "server/discover";
        last_meta = params.value("_meta", Json::object());
        return Json{
            {"resultType", "complete"},
            {"supportedVersions", {"2026-07-28", "2025-11-25"}},
            {"capabilities", {{"tools", Json::object()}}},
            {"instructions", "demo stateless server"},
            {"ttlMs", 3600000},
            {"cacheScope", "public"},
            {"_meta", {{std::string(meta_key::ServerInfo),
                        {{"name", "demo-server"}, {"version", "9.9"}}}}},
        };
    });
    // tools/call: MRTR. The first call returns input_required asking for an
    // elicitation; the retry (carrying inputResponses) completes.
    router.on_raw(std::string(to_server::CallTool::name), [&](Srv&, const Json& params) -> Json {
        last_method = "tools/call";
        last_meta = params.value("_meta", Json::object());
        const Json meta = params.value("_meta", Json::object());
        const std::string irk = std::string(meta_key::InputResponses);
        if (!meta.contains(irk)) {
            ElicitFormParams fp;
            fp.message = "Your name?";
            fp.properties.emplace_back("name",
                PrimitiveSchema{StringSchema{std::string("Name"), Nothing,
                                             Nothing, Nothing, Nothing, Nothing}});
            return Json{
                {"resultType", "input_required"},
                {"requestState", "opaque-state-42"},
                {"inputRequests", {
                    {"need_name", {
                        {"method", std::string(method::Elicit)},
                        {"params", to_json(ElicitParams{fp})}}}}},
            };
        }
        const Json& responses = meta[irk];
        std::string name = "unknown";
        if (responses.contains("need_name")) {
            const Json& er = responses["need_name"];
            if (er.contains("content") && er["content"].contains("name"))
                name = er["content"]["name"].get<std::string>();
        }
        bool state_ok = meta.contains(std::string(meta_key::RequestState))
            && meta[std::string(meta_key::RequestState)] == "opaque-state-42";
        return Json{
            {"content", Json::array({{{"type", "text"},
                {"text", std::string("hello ") + name +
                         (state_ok ? " [state-ok]" : " [state-BAD]")}}})}};
    });
    router.on_raw(std::string(to_server::Ping::name), [&](Srv&, const Json& params) -> Json {
        last_meta = params.value("_meta", Json::object());
        return Json::object();
    });

    test::Wire w;
    w.answer = test::answer_with(router, srv);
    bool elicited = false;
    w.handlers.on_elicit = [&](const ElicitParams&) -> ElicitResult {
        elicited = true;
        ElicitResult r;
        r.action = ElicitAction::Accept;
        r.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Ada")}}};
        return r;
    };

    // Modern metadata on every request; no initialize handshake.
    w.client.set_request_meta(modern_request_meta(
        Implementation{"demo-client", "1.0", Nothing, Nothing, Nothing, Nothing},
        ClientCapabilities{}));

    // 1. server/discover carries the modern _meta.
    {
        auto d = w.call<to_server::Discover>(DiscoverParams{});
        REQUIRE(d.has_value());
        CHECK(d->resultType == "complete");
        CHECK(d->supportedVersions.size() == 2);
        CHECK(d->supportedVersions[0] == "2026-07-28");
        CHECK(d->instructions.has_value() && *d->instructions == "demo stateless server");
        CHECK(d->ttlMs == 3600000);
        CHECK(d->cacheScope == "public");
        CHECK(last_method == "server/discover");
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
        CHECK(last_meta[std::string(meta_key::ProtocolVersion)] == "2026-07-28");
        CHECK(last_meta.contains(std::string(meta_key::ClientInfo)));
        CHECK(last_meta[std::string(meta_key::ClientInfo)]["name"] == "demo-client");
        CHECK(last_meta.contains(std::string(meta_key::ClientCapabilities)));
    }

    // 2. MRTR drives the input_required round.
    {
        auto r = w.call_mrtr(to_server::CallTool::name,
                             to_json(CallToolParams{"greet", Json::object(), Nothing, Json::object()}));
        REQUIRE(r.has_value());
        auto res = from_json<CallToolResult>(*r);
        CHECK(elicited);
        CHECK(res.content.size() == 1);
        CHECK(std::get<TextContent>(res.content[0]).text == "hello Ada [state-ok]");
        // The final (retry) request still carried the protocol metadata.
        CHECK(last_meta.contains(std::string(meta_key::ProtocolVersion)));
    }

    // 3. Clearing the request meta stops sending it.
    {
        w.client.set_request_meta(Json::object());
        auto p = w.call<to_server::Ping>(Unit{});
        CHECK(p.has_value());
        CHECK(!last_meta.contains(std::string(meta_key::ProtocolVersion)));
    }
    CHECK(g_failures == 0);
}
