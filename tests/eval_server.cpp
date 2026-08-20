// SPDX-License-Identifier: Apache-2.0
//
// eval_server.cpp — a minimal stdio MCP server that exposes the FULL
// batteries-included toolset (make_provider), for the Tier-1 eval harness
// (evals/run.py). Speaks just enough of the JSON-RPC surface the harness
// uses: `initialize`, `tools/list`, `tools/call`. Self-contained so
// mcp-cpp's evals don't depend on the agentty superproject binary.
//
// Usage (driven by evals/run.py):
//   eval_server -w <workspace>   < requests.jsonl   > responses.jsonl

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <string_view>

using json = nlohmann::json;
using namespace mcp::tools;

int main(int argc, char** argv) {
    // -w <dir> sets the workspace boundary the tools resolve paths against.
    std::string workspace = ".";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "-w") workspace = argv[i + 1];
    util::set_workspace_root(workspace);

    HostServices svc;                        // no http/git backends needed
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    auto reply = [](const json& id, json result) {
        json r{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
        std::cout << r.dump() << "\n" << std::flush;
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try { req = json::parse(line); } catch (...) { continue; }
        const std::string method = req.value("method", "");
        const json id = req.contains("id") ? req["id"] : json(nullptr);

        if (method == "initialize") {
            reply(id, json{{"protocolVersion", "2024-11-05"},
                           {"capabilities", {{"tools", json::object()}}},
                           {"serverInfo", {{"name", "eval_server"}, {"version", "1"}}}});
        } else if (method == "tools/list") {
            json arr = json::array();
            for (const auto& t : provider->list()) {
                json tj = json::object();
                tj["name"] = t.name;
                tj["description"] = t.description.value_or("");
                arr.push_back(std::move(tj));
            }
            reply(id, json{{"tools", std::move(arr)}});
        } else if (method == "tools/call") {
            const auto& p = req["params"];
            mcp::cap::Request cr{p.value("name", ""),
                                 p.value("arguments", json::object())};
            auto res = provider->execute(cr);
            reply(id, json{{"content", json::array({
                              json{{"type", "text"}, {"text", res.text}}})},
                           {"isError", res.is_error}});
        }
        // unknown methods: ignore (the harness only sends the three above)
    }
    return 0;
}
