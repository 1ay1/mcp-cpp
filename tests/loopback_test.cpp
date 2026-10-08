// SPDX-License-Identifier: Apache-2.0
//
// loopback_test.cpp — a client and a server, two engines, no threads.
//
// Frames are carried by hand from one side's Effects.send into the other's
// Received, so the whole session runs synchronously: initialize, tools,
// resources, prompts, the server's callbacks (sampling, roots, elicitation)
// and a log notification.

#include <mcp/mcp.hpp>

#include "agtest.hpp"
#include "wire.hpp"

#include <string>

using namespace mcp;


TEST_CASE("loopback") {
    // ── Server ────────────────────────────────────────────────────────────
    Server server(Implementation{"demo-server", "1.0", Nothing, Nothing, Nothing, Nothing});
    server.set_instructions("A demo MCP server.");
    {
        Tool t;
        t.name = "echo";
        t.description = "Echo back the text argument";
        t.inputSchema.properties = Json{{"text", {{"type", "string"}}}};
        t.inputSchema.required = List<std::string>{"text"};
        server.register_tool(std::move(t), [](const Json& args) -> CallToolResult {
            CallToolResult r;
            r.content = {text("echo: " + args.value("text", std::string{}))};
            return r;
        });
    }
    {
        Resource r;
        r.uri = "mem:///greeting"; r.name = "greeting"; r.mimeType = "text/plain";
        server.register_resource(std::move(r), [](const std::string& uri) -> ReadResourceResult {
            ReadResourceResult out;
            out.contents = {ResourceContents{TextResourceContents{uri, "Hello, world!", std::string("text/plain"), Json::object()}}};
            return out;
        });
    }
    {
        Prompt p;
        p.name = "greet"; p.description = "Greet a person";
        p.arguments = List<PromptArgument>{PromptArgument{"who", Nothing, std::string("Person to greet"), true}};
        server.register_prompt(std::move(p),
            [](const std::vector<std::pair<std::string,std::string>>& args) -> GetPromptResult {
                std::string who = "stranger";
                for (auto& [k, v] : args) if (k == "who") who = v;
                GetPromptResult r;
                r.messages = {PromptMessage{Role::User, text("Say hello to " + who)}};
                return r;
            });
    }

    test::Wire w;
    w.answer         = [&server](const Call& c) { return server.handle(c); };
    w.on_server_note = [&server](const Notification& n) { server.handle(n); };

    // ── Client's callbacks ────────────────────────────────────────────────
    bool sampling_called = false, roots_called = false, got_log = false, elicited = false;
    w.handlers.on_create_message = [&](const CreateMessageParams& p) -> CreateMessageResult {
        sampling_called = true;
        CreateMessageResult r;
        r.role = Role::Assistant; r.model = "test-model";
        std::string seen;
        if (!p.messages.empty() && std::holds_alternative<TextContent>(p.messages[0].content[0]))
            seen = std::get<TextContent>(p.messages[0].content[0]).text;
        r.content = {SamplingContentBlock{TextContent{"completion of: " + seen, Nothing, Json::object()}}};
        r.stopReason = "endTurn";
        return r;
    };
    w.handlers.on_list_roots = [&]() -> ListRootsResult {
        roots_called = true;
        ListRootsResult r;
        r.roots = {Root{"file:///workspace", std::string("workspace"), Json::object()}};
        return r;
    };
    w.handlers.on_log = [&](const LoggingMessageParams& p) { got_log = (p.level == LoggingLevel::Info); };
    w.handlers.on_elicit = [&](const ElicitParams& p) -> ElicitResult {
        elicited = std::holds_alternative<ElicitFormParams>(p);
        ElicitResult r; r.action = ElicitAction::Accept;
        r.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Grace")}}};
        return r;
    };

    // ── Drive the session ───────────────────────────────────────────────
    {
        InitializeParams ip;
        ip.protocolVersion = std::string(kProtocolVersion);
        ip.clientInfo = Implementation{"demo-client", "1.0", Nothing, Nothing, Nothing, Nothing};
        auto r = w.call<to_server::Initialize>(ip);
        REQUIRE(r.has_value());
        CHECK(r->protocolVersion == "2026-07-28");
        CHECK(r->serverInfo.name == "demo-server");
        CHECK(r->instructions.has_value() && *r->instructions == "A demo MCP server.");
        w.to_server.push_back(notify<to_server::Initialized>(Unit{}));
        w.pump();
    }
    {
        auto r = w.call<to_server::ListTools>(ListToolsParams{});
        REQUIRE(r.has_value());
        CHECK(r->tools.size() == 1);
        CHECK(r->tools[0].name == "echo");
    }
    {
        auto r = w.call<to_server::CallTool>(CallToolParams{"echo", Json{{"text", "ping"}}, Nothing, Json::object()});
        REQUIRE(r.has_value());
        CHECK(r->content.size() == 1);
        CHECK(std::get<TextContent>(r->content[0]).text == "echo: ping");
    }
    {
        auto r = w.call<to_server::CallTool>(CallToolParams{"nope", Json::object(), Nothing, Json::object()});
        CHECK(!r.has_value());
        if (!r) CHECK(r.error().code == errc::InvalidParams);
    }
    {
        auto r = w.call<to_server::ListResources>(ListResourcesParams{});
        REQUIRE(r.has_value());
        CHECK(r->resources.size() == 1);
        CHECK(r->resources[0].uri == "mem:///greeting");
        auto rr = w.call<to_server::ReadResource>(ReadResourceParams{"mem:///greeting", Json::object()});
        REQUIRE(rr.has_value());
        CHECK(std::get<TextResourceContents>(rr->contents[0]).text == "Hello, world!");
    }
    {
        auto r = w.call<to_server::ListPrompts>(ListPromptsParams{});
        REQUIRE(r.has_value());
        CHECK(r->prompts.size() == 1);
        GetPromptParams gp;
        gp.name = "greet";
        gp.arguments = std::vector<std::pair<std::string,std::string>>{{"who", "Ada"}};
        auto gr = w.call<to_server::GetPrompt>(gp);
        REQUIRE(gr.has_value());
        CHECK(std::get<TextContent>(gr->messages[0].content).text == "Say hello to Ada");
    }

    // ── Server → client ──────────────────────────────────────────────────
    {
        CreateMessageParams p;
        p.maxTokens = 256;
        p.messages = {SamplingMessage{Role::User, {SamplingContentBlock{TextContent{"the question", Nothing, Json::object()}}}, Json::object()}};
        auto r = w.server_call<to_client::CreateMessage>(p);
        REQUIRE(r.has_value());
        CHECK(sampling_called);
        CHECK(r->model == "test-model");
        CHECK(std::get<TextContent>(r->content[0]).text == "completion of: the question");
    }
    {
        auto r = w.server_call<to_client::ListRoots>(Unit{});
        REQUIRE(r.has_value());
        CHECK(roots_called);
        CHECK(r->roots.size() == 1);
        CHECK(r->roots[0].uri == "file:///workspace");
    }
    {
        LoggingMessageParams lp;
        lp.level = LoggingLevel::Info;
        lp.data  = Json{{"event", "ready"}};
        w.to_client.push_back(notify<to_client::LoggingMessage>(lp));
        w.pump();
        CHECK(got_log);
    }
    {
        ElicitFormParams fp;
        fp.message = "Your name?";
        fp.properties.emplace_back("name",
            PrimitiveSchema{StringSchema{std::string("Name"), Nothing, Nothing, Nothing, Nothing, Nothing}});
        fp.required = List<std::string>{"name"};
        auto r = w.server_call<to_client::Elicit>(ElicitParams{fp});
        REQUIRE(r.has_value());
        CHECK(elicited);
        CHECK(r->action == ElicitAction::Accept);
        CHECK(std::get<std::string>((*r->content)[0].second) == "Grace");
    }

    // ── MRTR: one retry turn, built purely ───────────────────────────────
    {
        Json sent = {{"name", "x"}, {"arguments", Json::object()}};
        Json got = {{"resultType", "input_required"},
                    {"inputRequests", {{"k", {{"method", "roots/list"}, {"params", Json::object()}}}}},
                    {"requestState", "opaque"}};
        auto retry = mrtr_retry(sent, got, w.handlers.mrtr());
        REQUIRE(retry.has_value());
        CHECK((*retry)["_meta"][std::string(meta_key::RequestState)] == "opaque");
        CHECK((*retry)["_meta"][std::string(meta_key::InputResponses)].contains("k"));
        CHECK(!mrtr_retry(sent, Json{{"content", Json::array()}}, w.handlers.mrtr()).has_value());
    }

    CHECK(w.client.pending() == 0);
    CHECK(w.server.pending() == 0);
}
