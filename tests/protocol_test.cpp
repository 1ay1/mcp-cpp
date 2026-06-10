// SPDX-License-Identifier: Apache-2.0
//
// protocol_test.cpp — exercise the JSON-RPC envelope algebra, the message-level
// sums, the structured -32042 error, and the compile-time method dictionary.
//
#include <mcp/mcp.hpp>

#include <iostream>

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
    // ── JsonRpcMessage round-trips for each envelope shape ───────────────
    {
        JsonRpcMessage req = JsonRpcMessage{JsonRpcRequest{
            id(1), "tools/call", Json{{"name", "x"}}}};
        Json j = to_json(req);
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"] == 1);
        CHECK(j["method"] == "tools/call");
        CHECK(j["params"]["name"] == "x");
        auto back = from_json<JsonRpcMessage>(j);
        CHECK(std::holds_alternative<JsonRpcRequest>(back));

        JsonRpcMessage note = JsonRpcMessage{JsonRpcNotification{
            "notifications/initialized", Json::object()}};
        Json nj = to_json(note);
        CHECK(nj["method"] == "notifications/initialized");
        CHECK(!nj.contains("id"));
        CHECK(std::holds_alternative<JsonRpcNotification>(from_json<JsonRpcMessage>(nj)));

        JsonRpcMessage ok = JsonRpcMessage{
            JsonRpcResponse{JsonRpcResult{id(2), Json{{"ok", true}}}}};
        Json oj = to_json(ok);
        CHECK(oj["result"]["ok"] == true);
        CHECK(std::holds_alternative<JsonRpcResponse>(from_json<JsonRpcMessage>(oj)));

        JsonRpcMessage err = JsonRpcMessage{
            JsonRpcResponse{JsonRpcError{id(3), Error{-32601, "Method not found", Nothing}}}};
        Json ej = to_json(err);
        CHECK(ej["error"]["code"] == -32601);
        CHECK(ej["error"]["message"] == "Method not found");
    }

    // ── string-id round-trips (RpcId = string | number) ──────────────────
    {
        JsonRpcRequest r{id("abc-1"), "ping", Json::object()};
        Json j = to_json(r);
        CHECK(j["id"] == "abc-1");
        CHECK(!j.contains("params"));   // empty params omitted
        CHECK(from_json<JsonRpcRequest>(j).id == id("abc-1"));
    }

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

        Error e{kUrlElicitationRequired, "URL elicitation required", Just<Json>(data)};
        CHECK(to_json(e)["code"] == -32042);
    }

    // ── Error optional data omitted ──────────────────────────────────────
    {
        Error e{-32602, "Invalid params", Nothing};
        Json j = to_json(e);
        CHECK(!j.contains("data"));
        CHECK(from_json<Error>(j).code == -32602);
    }

    // ── compile-time method dictionary ───────────────────────────────────
    {
        // The method literal is carried by the descriptor type itself.
        static_assert(method_v<dict::CallTool>   == "tools/call");
        static_assert(method_v<dict::Initialize> == "initialize");
        static_assert(method_v<dict::Elicit>     == "elicitation/create");
        static_assert(method_v<dict::GetTask>    == "tasks/get");
        static_assert(method_v<dict::CancelTask> == "tasks/cancel");
        static_assert(method_v<dict::LoggingMessage> == "notifications/message");
        // The result type is paired with the params type at compile time.
        static_assert(std::is_same_v<dict::CallTool::Result, CallToolResult>);
        static_assert(std::is_same_v<dict::CallTool::Params, CallToolParams>);
        static_assert(std::is_same_v<dict::ListRoots::Result, ListRootsResult>);
        CHECK(true);
    }

    // ── typed dispatch over a loopback engine ────────────────────────────
    {
        RpcEngine* a_eng = nullptr;
        RpcEngine* b_eng = nullptr;
        RpcEngine a([&](std::string_view f) { b_eng->feed_line(f); });
        RpcEngine b([&](std::string_view f) { a_eng->feed_line(f); });
        a_eng = &a; b_eng = &b;

        // b serves tools/call via the typed `handle<Desc>` helper.
        handle<dict::CallTool>(b, [](const CallToolParams& p) -> CallToolResult {
            CallToolResult r;
            r.content = {text("called " + p.name)};
            return r;
        });
        // a calls it via `call<Desc>` — result type deduced from the descriptor.
        auto fut = call<dict::CallTool>(a, CallToolParams{"echo", Json::object(), Nothing, Json::object()});
        CallToolResult res = fut.get();
        CHECK(std::get<TextContent>(res.content[0]).text == "called echo");

        // notification path: observe<Desc> / send<Desc>.
        bool logged = false;
        observe<dict::LoggingMessage>(a, [&](const LoggingMessageParams& m) {
            logged = (m.level == LoggingLevel::Warning);
        });
        send<dict::LoggingMessage>(b, LoggingMessageParams{
            LoggingLevel::Warning, Json{{"msg", "hi"}}, Nothing, Json::object()});
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

    if (g_failures == 0) {
        std::cout << "protocol_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "protocol_test: " << g_failures << " failure(s)\n";
    return 1;
}
