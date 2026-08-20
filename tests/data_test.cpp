// SPDX-License-Identifier: Apache-2.0
//
// data_test.cpp — json_query (jq-lite) driven through make_provider().

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define mcp_getpid _getpid
#else
#  include <unistd.h>
#  define mcp_getpid getpid
#endif

using namespace mcp::tools;
namespace fs = std::filesystem;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}
static void wr(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}

TEST_CASE("json_query") {
    auto root = fs::temp_directory_path() / ("mcp_data_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    util::set_workspace_root(root);
    auto prev = fs::current_path();
    fs::current_path(root);

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    const std::string pkg = R"({
      "name": "demo",
      "version": "1.2.3",
      "scripts": { "build": "tsc", "test": "vitest", "lint": "eslint ." },
      "dependencies": { "react": "^18.0.0", "zod": "^3.0.0" },
      "services": [
        { "name": "api",  "port": 8080 },
        { "name": "web",  "port": 3000 },
        { "name": "cron", "port": 0 }
      ]
    })";
    wr(root / "package.json", pkg);
    auto pj = obj(); pj["path"] = (root / "package.json").string();

    auto q = [&](const std::string& query, bool raw=false) {
        auto a = pj; a["query"] = query; if (raw) a["raw"] = true;
        return call(*provider, "json_query", a);
    };

    // ── scalar member ────────────────────────────────────────────────────
    {
        auto r = q(".version");
        check(!r.is_error, ".version runs");
        check(has(r.text, "1.2.3"), ".version returns the value");
        check(read_effects(r).has(Effect::ReadFs), "json_query is ReadFs");
    }
    // raw string
    {
        auto r = q(".name", /*raw=*/true);
        check(r.text == "demo", "raw:true unquotes a string result");
    }
    // ── nested member ────────────────────────────────────────────────────
    {
        auto r = q(".dependencies.react", true);
        check(r.text == "^18.0.0", "nested .a.b path");
    }
    // ── keys pipeline ────────────────────────────────────────────────────
    {
        auto r = q(".scripts | keys");
        check(!r.is_error, "| keys runs");
        check(has(r.text, "build") && has(r.text, "test") && has(r.text, "lint"),
              "| keys lists the object's keys");
    }
    // ── length ───────────────────────────────────────────────────────────
    {
        auto r = q(".services | length");
        check(r.text == "3", "| length of an array");
    }
    // ── array index (incl negative) ──────────────────────────────────────
    {
        auto r = q(".services[0].name", true);
        check(r.text == "api", "positive index + member");
        auto r2 = q(".services[-1].name", true);
        check(r2.text == "cron", "negative index counts from the end");
    }
    // ── iterate: one result line per element ─────────────────────────────
    {
        auto r = q(".services[].name", true);
        check(!r.is_error, "[] iterate runs");
        check(has(r.text, "api") && has(r.text, "web") && has(r.text, "cron"),
              "iterate yields every element's field");
        // three lines
        check(std::count(r.text.begin(), r.text.end(), '\n') == 2,
              "one result per iterated element (3 lines)");
    }
    // ── slice ────────────────────────────────────────────────────────────
    {
        auto r = q(".services[0:2] | length");
        check(r.text == "2", "slice [0:2] then length");
    }
    // ── type / has ───────────────────────────────────────────────────────
    {
        check(q(".services | type").text == "\"array\"", "| type reports array");
        check(q(".version | type").text == "\"string\"", "| type reports string");
        check(q("has(\"scripts\")").text == "true", "has(k) true for present key");
        check(q("has(\"nope\")").text == "false", "has(k) false for absent key");
    }
    // ── identity ─────────────────────────────────────────────────────────
    {
        auto r = q(".");
        check(!r.is_error && has(r.text, "\"name\""), ". returns the whole doc");
    }
    // ── inline json (no file) ────────────────────────────────────────────
    {
        auto a = obj();
        a["json"] = R"({"a":{"b":[10,20,30]}})";
        a["query"] = ".a.b[1]";
        auto r = call(*provider, "json_query", a);
        check(!r.is_error && r.text == "20", "inline json + index path");
    }
    // ── error paths ──────────────────────────────────────────────────────
    {
        // bad JSON
        wr(root / "bad.json", "{ not json ");
        auto a = obj(); a["path"] = (root / "bad.json").string(); a["query"] = ".";
        check(call(*provider, "json_query", a).is_error, "invalid JSON is an error");
        // member on non-object
        check(q(".version.nope").is_error, ".key on a scalar is an error");
        // neither path nor json
        auto b = obj(); b["query"] = ".";
        check(call(*provider, "json_query", b).is_error, "missing path AND json is an error");
        // unknown verb
        check(q(".scripts | frobnicate").is_error, "unknown pipeline verb is an error");
    }

    fs::current_path(prev);
    fs::remove_all(root);
}
