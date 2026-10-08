// SPDX-License-Identifier: Apache-2.0
//
// protocol_test.cpp — the structured -32042 error, the method types, typed
// dispatch over a wire, and the newer spec vocabulary. (The JSON-RPC
// envelope itself is jsonrpc-cpp's, tested there.)
//
#include "agtest.hpp"
#include "wire.hpp"

static int g_failures = 0;

#include <mcp/mcp.hpp>

#include <iostream>

using namespace mcp;


TEST_CASE("protocol") {
    // ── URLElicitationRequiredError data (-32042) ────────────────────────
    {
        UrlElicitationRequiredErrorData d;
        d.elicitations = {ElicitUrlParams{"Authorize", "el-9", "https://x/auth", Nothing, Json::object()}};
        Json data = to_json(d);
        CHECK(data["elicitations"][0]["mode"] == "url");
        CHECK(data["elicitations"][0]["elicitationId"] == "el-9");
        auto back = from_json<UrlElicitationRequiredErrorData>(data);
        CHECK(back.elicitations.size() == 1);
        CHECK(back.elicitations[0].url == "https://x/auth");

        RpcError e(kUrlElicitationRequired, "URL elicitation required", data);
        CHECK(to_json(e)["code"] == -32042);
        CHECK(to_json(e)["data"]["elicitations"][0]["elicitationId"] == "el-9");
    }

    // ── method types ─────────────────────────────────────────────────────
    {
        static_assert(to_server::CallTool::name   == "tools/call");
        static_assert(to_server::Initialize::name == "initialize");
        static_assert(to_client::Elicit::name     == "elicitation/create");
        static_assert(to_server::GetTask::name    == "tasks/get");
        static_assert(to_server::CancelTask::name == "tasks/cancel");
        static_assert(to_client::LoggingMessage::name == "notifications/message");
        static_assert(std::is_same_v<to_server::CallTool::result, CallToolResult>);
        static_assert(std::is_same_v<to_server::CallTool::params, CallToolParams>);
        static_assert(std::is_same_v<to_client::ListRoots::result, ListRootsResult>);
        static_assert(jsonrpc::IsNote<to_client::LoggingMessage>);
        static_assert(jsonrpc::IsMethod<to_server::CallTool>);
        CHECK(true);
    }

    // ── typed dispatch over a wire ───────────────────────────────────────
    {
        struct B {} b;
        Router<B> router;
        router.on<to_server::CallTool>([](B&, const CallToolParams& p) {
            CallToolResult r;
            r.content = {text("called " + p.name)};
            return r;
        });
        test::Wire w;
        w.answer = test::answer_with(router, b);
        auto res = w.call<to_server::CallTool>(CallToolParams{"echo", Json::object(), Nothing, Json::object()});
        REQUIRE(res.has_value());
        CHECK(std::get<TextContent>(res->content[0]).text == "called echo");

        bool logged = false;
        w.handlers.on_log = [&](const LoggingMessageParams& m) { logged = (m.level == LoggingLevel::Warning); };
        w.to_client.push_back(notify<to_client::LoggingMessage>(LoggingMessageParams{
            LoggingLevel::Warning, Json{{"msg", "hi"}}, Nothing, Json::object()}));
        w.pump();
        CHECK(logged);
    }

    // ── newly-added spec vocabulary types ────────────────────────────────
    {
        RelatedTaskMetadata rt{"task-42"};
        CHECK(to_json(rt)["taskId"] == "task-42");
        CHECK(from_json<RelatedTaskMetadata>(to_json(rt)).taskId == "task-42");

        BaseMetadata bm{"thing", std::string("Thing")};
        CHECK(to_json(bm)["name"] == "thing");
        CHECK(to_json(bm)["title"] == "Thing");

        Icons ic; ic.icons = List<Icon>{Icon{"https://x/i.png", Nothing, Nothing, IconTheme::Dark}};
        CHECK(to_json(ic)["icons"][0]["theme"] == "dark");

        // LegacyTitledEnum arm of PrimitiveSchema round-trips (and is
        // distinguished from a plain string / untitled enum by enumNames).
        LegacyTitledEnum le;
        le.values = {"a", "b"}; le.enumNames = List<std::string>{"Alpha", "Beta"};
        PrimitiveSchema ps{le};
        Json pj = to_json(ps);
        CHECK(pj["enum"][0] == "a");
        CHECK(pj["enumNames"][1] == "Beta");
        auto back = from_json<PrimitiveSchema>(pj);
        CHECK(std::holds_alternative<LegacyTitledEnum>(back));
    }
    CHECK(g_failures == 0);
}
