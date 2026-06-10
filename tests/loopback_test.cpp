// SPDX-License-Identifier: Apache-2.0
//
// loopback_test.cpp — wire a Client and a Server back-to-back through an
// in-process transport pair and drive a full session: initialize, list_tools,
// call_tool, read_resource, get_prompt, plus a server→client sampling callback
// and a roots callback. No sockets, no threads in the transport — each side's
// sink feeds the other's engine directly.
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
    // Forward declarations so each sink can reference the other engine.
    RpcEngine* client_engine = nullptr;
    RpcEngine* server_engine = nullptr;

    // Transport pair: writing on one side feeds the other's feed_line.
    Transport to_server = [&](std::string_view frame) { server_engine->feed_line(frame); };
    Transport to_client = [&](std::string_view frame) { client_engine->feed_line(frame); };

    // ── Server ────────────────────────────────────────────────────────────
    Server server(to_client, Implementation{"demo-server", "1.0", Nothing, Nothing, Nothing, Nothing});
    server.set_instructions("A demo MCP server.");
    server_engine = &server.engine();

    // A tool.
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
    // A resource.
    {
        Resource r;
        r.uri = "mem:///greeting"; r.name = "greeting"; r.mimeType = "text/plain";
        server.register_resource(std::move(r), [](const std::string& uri) -> ReadResourceResult {
            ReadResourceResult out;
            out.contents = {ResourceContents{TextResourceContents{uri, "Hello, world!", std::string("text/plain"), Json::object()}}};
            return out;
        });
    }
    // A prompt.
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

    // ── Client ────────────────────────────────────────────────────────────
    ClientHandlers ch;
    bool sampling_called = false, roots_called = false;
    ch.on_create_message = [&](const CreateMessageParams& p) -> CreateMessageResult {
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
    ch.on_list_roots = [&]() -> ListRootsResult {
        roots_called = true;
        ListRootsResult r;
        r.roots = {Root{"file:///workspace", std::string("workspace"), Json::object()}};
        return r;
    };
    bool got_log = false;
    ch.on_log = [&](const LoggingMessageParams& p) { got_log = (p.level == LoggingLevel::Info); };

    Client client(to_server, std::move(ch));
    client_engine = &client.engine();

    // ── Drive the session ───────────────────────────────────────────────
    {
        auto r = client.initialize(
            Implementation{"demo-client", "1.0", Nothing, Nothing, Nothing, Nothing},
            ClientCapabilities{}).get();
        CHECK(r.protocolVersion == "2025-11-25");
        CHECK(r.serverInfo.name == "demo-server");
        CHECK(r.instructions.has_value() && *r.instructions == "A demo MCP server.");
        client.initialized();
    }
    {
        auto r = client.list_tools().get();
        CHECK(r.tools.size() == 1);
        CHECK(r.tools[0].name == "echo");
    }
    {
        auto r = client.call_tool("echo", Json{{"text", "ping"}}).get();
        CHECK(r.content.size() == 1);
        CHECK(std::holds_alternative<TextContent>(r.content[0]));
        CHECK(std::get<TextContent>(r.content[0]).text == "echo: ping");
    }
    {
        bool threw = false;
        try { client.call_tool("nope").get(); } catch (const RpcError& e) {
            threw = true; CHECK(e.code == errc::InvalidParams);
        }
        CHECK(threw);
    }
    {
        auto r = client.list_resources().get();
        CHECK(r.resources.size() == 1);
        CHECK(r.resources[0].uri == "mem:///greeting");
        auto rr = client.read_resource("mem:///greeting").get();
        CHECK(rr.contents.size() == 1);
        CHECK(std::holds_alternative<TextResourceContents>(rr.contents[0]));
        CHECK(std::get<TextResourceContents>(rr.contents[0]).text == "Hello, world!");
    }
    {
        auto r = client.list_prompts().get();
        CHECK(r.prompts.size() == 1);
        GetPromptParams gp;
        gp.name = "greet";
        gp.arguments = std::vector<std::pair<std::string,std::string>>{{"who", "Ada"}};
        auto gr = client.get_prompt(gp).get();
        CHECK(gr.messages.size() == 1);
        CHECK(std::get<TextContent>(gr.messages[0].content).text == "Say hello to Ada");
    }

    // ── Server → Client callbacks ────────────────────────────────────────
    {
        CreateMessageParams p;
        p.maxTokens = 256;
        p.messages = {SamplingMessage{Role::User, {SamplingContentBlock{TextContent{"the question", Nothing, Json::object()}}}, Json::object()}};
        auto r = server.create_message(p).get();
        CHECK(sampling_called);
        CHECK(r.model == "test-model");
        CHECK(std::get<TextContent>(r.content[0]).text == "completion of: the question");
    }
    {
        auto r = server.list_roots().get();
        CHECK(roots_called);
        CHECK(r.roots.size() == 1);
        CHECK(r.roots[0].uri == "file:///workspace");
    }
    {
        server.log(LoggingLevel::Info, Json{{"event", "ready"}});
        CHECK(got_log);
    }

    // ── Tasks (durable requests) ────────────────────────────────────────
    {
        ServerHandlers th;
        th.on_get_task = [](const TaskIdParams& p) -> GetTaskResult {
            Task t; t.taskId = p.taskId; t.status = TaskStatus::Completed;
            t.createdAt = "2025-01-01T00:00:00Z"; t.lastUpdatedAt = "2025-01-01T00:00:05Z";
            return t;
        };
        th.on_list_tasks = [](const PaginatedParams&) -> ListTasksResult {
            ListTasksResult r;
            Task t; t.taskId = "abc"; t.status = TaskStatus::Working;
            t.createdAt = "2025-01-01T00:00:00Z"; t.lastUpdatedAt = "2025-01-01T00:00:01Z";
            r.tasks = {t};
            return r;
        };
        th.on_cancel_task = [](const TaskIdParams& p) -> CancelTaskResult {
            Task t; t.taskId = p.taskId; t.status = TaskStatus::Cancelled;
            t.createdAt = "2025-01-01T00:00:00Z"; t.lastUpdatedAt = "2025-01-01T00:00:09Z";
            return t;
        };
        server.set_handlers(std::move(th));

        auto g = client.get_task("abc").get();
        CHECK(g.taskId == "abc");
        CHECK(g.status == TaskStatus::Completed);
        auto l = client.list_tasks().get();
        CHECK(l.tasks.size() == 1 && l.tasks[0].status == TaskStatus::Working);
        auto c = client.cancel_task("abc").get();
        CHECK(c.status == TaskStatus::Cancelled);
    }

    // ── Elicitation (server → client, form mode) ──────────────────────────
    {
        ClientHandlers eh;
        bool elicited = false;
        eh.on_elicit = [&](const ElicitParams& p) -> ElicitResult {
            elicited = std::holds_alternative<ElicitFormParams>(p);
            ElicitResult r; r.action = ElicitAction::Accept;
            r.content = std::vector<std::pair<std::string, ElicitValue>>{
                {"name", ElicitValue{std::string("Grace")}}};
            return r;
        };
        // Re-install the elicit handler on the client engine.
        client.engine().on<ElicitParams, ElicitResult>(
            std::string(method::Elicit), eh.on_elicit);

        ElicitFormParams fp;
        fp.message = "Your name?";
        fp.properties.emplace_back("name",
            PrimitiveSchema{StringSchema{std::string("Name"), Nothing, Nothing, Nothing, Nothing, Nothing}});
        fp.required = List<std::string>{"name"};
        auto r = server.elicit(ElicitParams{fp}).get();
        CHECK(elicited);
        CHECK(r.action == ElicitAction::Accept);
        CHECK(r.content.has_value());
        CHECK(std::get<std::string>((*r.content)[0].second) == "Grace");
    }

    if (g_failures == 0) {
        std::cout << "loopback_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "loopback_test: " << g_failures << " failure(s)\n";
    return 1;
}
