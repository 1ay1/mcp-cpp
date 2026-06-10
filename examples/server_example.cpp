// SPDX-License-Identifier: Apache-2.0
//
// server_example.cpp — a minimal but complete MCP server over stdio.
//
//   Run it directly and pipe JSON-RPC frames in, or point any MCP client at
//   it as a stdio server command. Exposes:
//
//     • tool      "add"        — adds two integers (structured output)
//     • tool      "now"        — returns the current time (a task-capable tool)
//     • resource  "file:///motd"
//     • prompt    "summarize"
//
#include <mcp/mcp.hpp>

#include <chrono>
#include <ctime>
#include <iostream>

using namespace mcp;

int main() {
    StdioTransport transport(std::cin, std::cout);
    Server server(transport.sink(),
                  Implementation{"mcp-cpp-example", std::string(kLibraryVersion),
                                 std::string("Example Server"), Nothing, Nothing, Nothing});
    server.set_capabilities(ServerCapabilities{
        .logging = Json::object(),
        .tools   = ToolsCapability{true},
    });
    server.set_instructions("A reference MCP server built with mcp-cpp.");

    // ── tool: add ────────────────────────────────────────────────────────
    {
        Tool t;
        t.name = "add";
        t.title = "Add";
        t.description = "Add two integers and return their sum.";
        t.inputSchema.properties = Json{
            {"a", {{"type", "integer"}, {"description", "first addend"}}},
            {"b", {{"type", "integer"}, {"description", "second addend"}}}};
        t.inputSchema.required = List<std::string>{"a", "b"};
        t.outputSchema = JsonSchema{};
        t.outputSchema->properties = Json{{"sum", {{"type", "integer"}}}};
        t.annotations = ToolAnnotations{}; t.annotations->readOnlyHint = true;
        server.register_tool(std::move(t), [](const Json& args) -> CallToolResult {
            const long a = args.value("a", 0L);
            const long b = args.value("b", 0L);
            CallToolResult r;
            r.content = {text("The sum is " + std::to_string(a + b))};
            r.structuredContent = Json{{"sum", a + b}};
            return r;
        });
    }

    // ── tool: now ────────────────────────────────────────────────────────
    {
        Tool t;
        t.name = "now";
        t.description = "Return the current UTC time as an ISO-8601 string.";
        t.inputSchema.properties = Json::object();
        t.execution = ToolExecution{TaskSupport::Optional};
        server.register_tool(std::move(t), [](const Json&) -> CallToolResult {
            std::time_t now = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
            CallToolResult r;
            r.content = {text(buf)};
            return r;
        });
    }

    // ── resource: motd ───────────────────────────────────────────────────
    {
        Resource r;
        r.uri = "file:///motd"; r.name = "motd";
        r.title = "Message of the Day"; r.mimeType = "text/plain";
        server.register_resource(std::move(r), [](const std::string& uri) -> ReadResourceResult {
            ReadResourceResult out;
            out.contents = {ResourceContents{TextResourceContents{
                uri, "Welcome to mcp-cpp — the type-theoretic MCP SDK.",
                std::string("text/plain"), Json::object()}}};
            return out;
        });
    }

    // ── prompt: summarize ────────────────────────────────────────────────
    {
        Prompt p;
        p.name = "summarize";
        p.description = "Produce a prompt asking the model to summarize some text.";
        p.arguments = List<PromptArgument>{
            PromptArgument{"text", Nothing, std::string("The text to summarize"), true}};
        server.register_prompt(std::move(p),
            [](const std::vector<std::pair<std::string,std::string>>& args) -> GetPromptResult {
                std::string body;
                for (auto& [k, v] : args) if (k == "text") body = v;
                GetPromptResult r;
                r.description = "Summarization prompt";
                r.messages = {PromptMessage{Role::User,
                    text("Please summarize the following text concisely:\n\n" + body)}};
                return r;
            });
    }

    transport.start(server.engine());
    transport.join();   // run until stdin closes
    return 0;
}
