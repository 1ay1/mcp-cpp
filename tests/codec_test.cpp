// SPDX-License-Identifier: Apache-2.0
//
// codec_test.cpp — round-trip every interesting corner of the type system.
//
//   The codec law we verify:  decode ∘ encode = id   for each type, and that
//   the wire JSON matches the schema's expected shape.
//
#include <mcp/mcp.hpp>

#include "agtest.hpp"

#include <filesystem>
#include <fstream>

static int g_failures = 0;
#include <iostream>
#include <string>

using namespace mcp;


template <class T>
static void roundtrip(const T& v, const char* label) {
    Json j = to_json(v);
    T back = from_json<T>(j);
    Json j2 = to_json(back);
    if (j != j2) {
        std::cerr << "ROUNDTRIP MISMATCH (" << label << ")\n  a=" << j.dump()
                  << "\n  b=" << j2.dump() << "\n";
        ++g_failures;
    }
}

TEST_CASE("codec") {
    // ── scalars / unions ────────────────────────────────────────────────
    {
        RequestId s{std::string{"req-7"}};
        RequestId n{std::int64_t{42}};
        CHECK(to_json(s) == Json("req-7"));
        CHECK(to_json(n) == Json(42));
        CHECK(from_json<RequestId>(Json("x")).index() == 0);
        CHECK(from_json<RequestId>(Json(99)).index() == 1);
    }

    // ── content blocks (tagged sum on "type") ───────────────────────────
    {
        ContentBlock t = text("hello");
        Json j = to_json(t);
        CHECK(j["type"] == "text");
        CHECK(j["text"] == "hello");
        roundtrip(t, "TextContent");

        ContentBlock img = image("ABCD", "image/png");
        CHECK(to_json(img)["type"] == "image");
        roundtrip(img, "ImageContent");

        EmbeddedResource er;
        er.resource = ResourceContents{TextResourceContents{"file:///a", "body", std::string("text/plain"), Json::object()}};
        ContentBlock e{er};
        CHECK(to_json(e)["type"] == "resource");
        CHECK(to_json(e)["resource"]["text"] == "body");
        roundtrip(e, "EmbeddedResource");

        ResourceLink rl;
        rl.uri = "file:///b"; rl.name = "b";
        ContentBlock l{rl};
        CHECK(to_json(l)["type"] == "resource_link");
        roundtrip(l, "ResourceLink");
    }

    // ── sampling content (tool_use / tool_result) ───────────────────────
    {
        SamplingContentBlock tu{ToolUseContent{"id1", "search", Json{{"q", "x"}}, Json::object()}};
        CHECK(to_json(tu)["type"] == "tool_use");
        CHECK(to_json(tu)["input"]["q"] == "x");
        roundtrip(tu, "ToolUseContent");

        SamplingContentBlock tr{ToolResultContent{"id1", {text("ok")}, Nothing, Nothing, Json::object()}};
        CHECK(to_json(tr)["type"] == "tool_result");
        roundtrip(tr, "ToolResultContent");
    }

    // ── Tool with schema, annotations, execution ────────────────────────
    {
        Tool tool;
        tool.name = "add";
        tool.description = "Add two ints";
        tool.inputSchema.properties = Json{{"a", {{"type", "integer"}}}, {"b", {{"type", "integer"}}}};
        tool.inputSchema.required = List<std::string>{"a", "b"};
        tool.annotations = ToolAnnotations{}; tool.annotations->readOnlyHint = true;
        tool.execution = ToolExecution{TaskSupport::Optional};
        Json j = to_json(tool);
        CHECK(j["inputSchema"]["type"] == "object");
        CHECK(j["inputSchema"]["required"][0] == "a");
        CHECK(j["annotations"]["readOnlyHint"] == true);
        CHECK(j["execution"]["taskSupport"] == "optional");
        roundtrip(tool, "Tool");
    }

    // ── capabilities ────────────────────────────────────────────────────
    {
        ServerCapabilities caps;
        caps.tools = ToolsCapability{true};
        caps.resources = ResourcesCapability{true, false};
        Json j = to_json(caps);
        CHECK(j["tools"]["listChanged"] == true);
        CHECK(j["resources"]["subscribe"] == true);
        CHECK(j["resources"]["listChanged"] == false);
        roundtrip(caps, "ServerCapabilities");

        ClientCapabilities cc;
        cc.sampling = SamplingCapability{};
        cc.elicitation = ElicitationCapability{}; cc.elicitation->form = Json::object();
        roundtrip(cc, "ClientCapabilities");
    }

    // ── initialize round-trip ───────────────────────────────────────────
    {
        InitializeParams p;
        p.clientInfo = Implementation{"host", "1.0", std::string("Host"), Nothing, Nothing, Nothing};
        p.capabilities.roots = RootsCapability{true};
        Json j = to_json(p);
        CHECK(j["protocolVersion"] == "2026-07-28");
        CHECK(j["clientInfo"]["name"] == "host");
        CHECK(j["capabilities"]["roots"]["listChanged"] == true);
        roundtrip(p, "InitializeParams");

        InitializeResult r;
        r.serverInfo = Implementation{"srv", "2.0", Nothing, Nothing, Nothing, Nothing};
        r.instructions = "be nice";
        roundtrip(r, "InitializeResult");
    }

    // ── elicitation: every primitive schema arm ─────────────────────────
    {
        ElicitFormParams p;
        p.message = "Tell me about yourself";
        p.properties.emplace_back("name", PrimitiveSchema{StringSchema{std::string("Name"), Nothing, Nothing, std::int64_t{50}, Nothing, Nothing}});
        p.properties.emplace_back("age",  PrimitiveSchema{NumberSchema{true, std::string("Age"), Nothing, double{0}, double{150}, Nothing}});
        p.properties.emplace_back("subscribe", PrimitiveSchema{BooleanSchema{std::string("Subscribe"), Nothing, bool{true}}});
        p.properties.emplace_back("color", PrimitiveSchema{UntitledSingleSelectEnum{{"red","green","blue"}, Nothing, Nothing, Nothing}});
        p.properties.emplace_back("plan", PrimitiveSchema{TitledSingleSelectEnum{{{"free","Free"},{"pro","Pro"}}, Nothing, Nothing, Nothing}});
        p.properties.emplace_back("tags", PrimitiveSchema{UntitledMultiSelectEnum{{"a","b","c"}, Nothing, Nothing, Nothing, Nothing, Nothing}});
        p.properties.emplace_back("perms", PrimitiveSchema{TitledMultiSelectEnum{{{"r","Read"},{"w","Write"}}, Nothing, Nothing, Nothing, Nothing, Nothing}});
        p.required = List<std::string>{"name"};
        ElicitParams ep{p};
        Json j = to_json(ep);
        CHECK(j["mode"] == "form");
        const Json& props = j["requestedSchema"]["properties"];
        CHECK(props["name"]["type"] == "string");
        CHECK(props["name"]["maxLength"] == 50);
        CHECK(props["age"]["type"] == "integer");
        CHECK(props["subscribe"]["type"] == "boolean");
        CHECK(props["color"]["enum"][0] == "red");
        CHECK(props["plan"]["oneOf"][0]["const"] == "free");
        CHECK(props["tags"]["items"]["enum"][1] == "b");
        CHECK(props["perms"]["items"]["anyOf"][0]["title"] == "Read");
        roundtrip(ep, "ElicitFormParams");

        ElicitParams url{ElicitUrlParams{"Authorize", "el-1", "https://x/auth", Nothing, Json::object()}};
        CHECK(to_json(url)["mode"] == "url");
        roundtrip(url, "ElicitUrlParams");

        ElicitResult res;
        res.action = ElicitAction::Accept;
        res.content = std::vector<std::pair<std::string, ElicitValue>>{
            {"name", ElicitValue{std::string("Ada")}},
            {"age",  ElicitValue{double{36}}},
            {"subscribe", ElicitValue{true}},
            {"tags", ElicitValue{List<std::string>{"a","c"}}}};
        Json rj = to_json(res);
        CHECK(rj["action"] == "accept");
        CHECK(rj["content"]["name"] == "Ada");
        CHECK(rj["content"]["subscribe"] == true);
        CHECK(rj["content"]["tags"][1] == "c");
        roundtrip(res, "ElicitResult");
    }

    // ── sampling createMessage ──────────────────────────────────────────
    {
        CreateMessageParams p;
        p.maxTokens = 1024;
        p.messages = {SamplingMessage{Role::User, {SamplingContentBlock{TextContent{"hi", Nothing, Json::object()}}}, Json::object()}};
        p.modelPreferences = ModelPreferences{}; p.modelPreferences->intelligencePriority = 0.9;
        p.toolChoice = ToolChoice{ToolChoiceMode::Auto};
        Json j = to_json(p);
        CHECK(j["maxTokens"] == 1024);
        CHECK(j["messages"][0]["role"] == "user");
        CHECK(j["messages"][0]["content"][0]["text"] == "hi");
        CHECK(j["toolChoice"]["mode"] == "auto");
        roundtrip(p, "CreateMessageParams");

        CreateMessageResult r;
        r.role = Role::Assistant; r.model = "claude";
        r.content = {SamplingContentBlock{TextContent{"hello back", Nothing, Json::object()}}};
        r.stopReason = "endTurn";
        Json rj = to_json(r);
        CHECK(rj["content"]["type"] == "text");   // single block, not array
        CHECK(rj["stopReason"] == "endTurn");
        roundtrip(r, "CreateMessageResult");
    }

    // ── tasks ────────────────────────────────────────────────────────────
    {
        Task t;
        t.taskId = "t1"; t.status = TaskStatus::Working;
        t.createdAt = "2025-01-01T00:00:00Z"; t.lastUpdatedAt = "2025-01-01T00:00:01Z";
        t.ttl = 60000; t.pollInterval = 500;
        Json j = to_json(t);
        CHECK(j["status"] == "working");
        CHECK(j["taskId"] == "t1");
        roundtrip(t, "Task");
    }

    // ── completion ───────────────────────────────────────────────────────
    {
        CompleteParams p;
        p.ref = CompletionReference{PromptReference{"code_review", Nothing}};
        p.argument = CompleteArgument{"language", "py"};
        Json j = to_json(p);
        CHECK(j["ref"]["type"] == "ref/prompt");
        CHECK(j["ref"]["name"] == "code_review");
        CHECK(j["argument"]["value"] == "py");
        roundtrip(p, "CompleteParams");

        CompleteParams p2;
        p2.ref = CompletionReference{ResourceTemplateReference{"file:///{path}"}};
        p2.argument = CompleteArgument{"path", "/etc"};
        CHECK(to_json(p2)["ref"]["type"] == "ref/resource");
        roundtrip(p2, "CompleteParams resource ref");
    }

    // ── notifications ────────────────────────────────────────────────────
    {
        ProgressParams pp;
        pp.progressToken = ProgressToken{std::string("tok")};
        pp.progress = 0.5; pp.total = 1.0; pp.message = "halfway";
        CHECK(to_json(pp)["progressToken"] == "tok");
        CHECK(to_json(pp)["progress"] == 0.5);
        roundtrip(pp, "ProgressParams");

        CancelledParams cp;
        cp.requestId = RequestId{std::int64_t{7}};
        cp.reason = "user aborted";
        CHECK(to_json(cp)["requestId"] == 7);
        roundtrip(cp, "CancelledParams");
    }
    CHECK(g_failures == 0);
}

// ── the codec cache outlives static destruction ─────────────────────────
//
// codec<T>() hands out a reference to a process-lifetime cache. The obvious
// spelling of that cache is a Meyers singleton:
//
//     static const Codec<T> instance = build_codec<T>();
//
// which is registered with atexit and destroyed when main returns. That is
// wrong here, because the threads that call codec<T>() are not all joined by
// then. A host that times out a slow server's handshake detaches the connect
// worker and moves on; that worker later calls codec<InitializeParams>() to
// encode its request while exit handlers are already running, and reads
// std::functions that have been freed.
//
// The resulting SIGSEGV lands inside std::function::operator() with no
// application frame nearby, which reads as a corrupted stack rather than a
// destruction-order bug. It reproduced about 1 run in 10 in the host's test
// binary and went unexplained for a long time.
//
// So the cache is deliberately leaked (one small immutable object per
// codec'd type) and this test pins that, because the Meyers spelling is
// strictly more idiomatic and will otherwise be "cleaned up" by someone
// reading the line in isolation.

namespace {

// Reproduces the exact crash scenario, deterministically.
//
// Static destructors and atexit handlers run in reverse order of
// registration, interleaved. This object is constructed EARLY (before any
// codec is built), so it is destroyed LATE — after the codec cache's own
// destructor would have run, had the cache been a by-value static.
//
// Its destructor then does what the orphaned MCP connect worker does: call
// codec<InitializeParams>() and encode with it. With the leaked cache that
// is fine. With a Meyers singleton it is a read of freed std::functions,
// which is the 1-in-10 SIGSEGV this pins.
//
// The check has to report from inside a destructor, after doctest has
// finished, so it writes to stderr and forces a non-zero exit rather than
// using CHECK.
struct LateCodecUser {
    bool armed = false;

    ~LateCodecUser() {
        if (!armed) return;
        mcp::InitializeParams p;
        p.clientInfo = mcp::Implementation{"late", "1.0", Nothing, Nothing,
                                           Nothing, Nothing};
        // If the cache was destroyed, this call reads freed memory. On a
        // good day it crashes here; under ASAN it is a clean use-after-free
        // report. Either way the run fails, which is the point.
        const Json j = mcp::codec<mcp::InitializeParams>().encode(p);
        if (j["clientInfo"]["name"] != "late") {
            std::cerr << "FATAL: codec<InitializeParams>() returned garbage "
                         "during static destruction \u2014 the codec cache was "
                         "torn down while it was still reachable.\n";
            std::_Exit(70);
        }
    }
};

// Constructed during static init, i.e. before main and before the first
// codec<T>() call anywhere. Destroyed correspondingly late.
LateCodecUser g_late_user;

}  // namespace

TEST_CASE("codec cache is not destroyed at exit") {
    // Build one, so the cache for this type definitely exists.
    const auto& a = mcp::codec<mcp::InitializeParams>();
    const auto& b = mcp::codec<mcp::InitializeParams>();

    // Same object every call — it really is a cache, not a rebuild.
    CHECK(&a == &b);

    // It works now; the interesting question is whether it still works
    // after main returns.
    mcp::InitializeParams p;
    p.clientInfo = mcp::Implementation{"host", "1.0", Nothing, Nothing,
                                       Nothing, Nothing};
    CHECK(a.encode(p)["clientInfo"]["name"] == "host");

    // Arm the late user. It runs long after this test case, during static
    // destruction, and fails the process if the cache has gone away.
    //
    // Note this arm is a BEST-EFFORT probe, not the real guard. Reading a
    // destroyed std::function is undefined behaviour, and undefined
    // behaviour is free to look like success: in a small binary the freed
    // storage is usually still intact and the encode returns the right
    // answer. It only turned into a reliable SIGSEGV in the host's much
    // larger test binary, where ~30 live threads and a concurrent allocator
    // meant the memory was genuinely reused. So this arm catches the bug
    // under ASAN and on a bad day, and the structural check below catches
    // it every time.
    g_late_user.armed = true;
}

TEST_CASE("codec cache uses non-destructible storage") {
    // THE ACTUAL GUARD.
    //
    // The property we need is "this static is never destroyed", which no
    // runtime assertion can observe: by the time it would be false, the
    // observer is reading freed memory and the answer is meaningless.
    // What IS decidable is the storage class in the source, so check that
    // directly — the same reason attribution_discipline_test scans sources
    // instead of trying to provoke a corrupted tool call.
    //
    // A function-local static of POINTER type registers no destructor.
    // A by-value `static const Codec<T>` registers one with atexit, and a
    // thread still running at exit can then read freed std::functions.
    const std::filesystem::path header =
        std::filesystem::path(__FILE__).parent_path() / ".." / "include"
                                       / "mcp" / "codec.hpp";
    std::ifstream in(header);
    REQUIRE(in.good());
    const std::string src((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    // Find the accessor's body and look at the static it declares.
    const auto at = src.find("inline const Codec<T>& codec()");
    REQUIRE(at != std::string::npos);
    const auto body = src.substr(at, 240);

    const bool pointer_storage =
        body.find("static const Codec<T>* const") != std::string::npos
        || body.find("static const Codec<T>*") != std::string::npos;

    const bool by_value_storage =
        body.find("static const Codec<T> instance") != std::string::npos
        || body.find("static Codec<T> instance") != std::string::npos;

    CHECK(pointer_storage);
    CHECK(!by_value_storage);

    if (by_value_storage || !pointer_storage) {
        std::cerr <<
            "codec<T>() must hold its cache in non-destructible storage.\n"
            "A by-value function-local static is destroyed at exit, but the\n"
            "threads that read codecs are not all joined by then: a detached\n"
            "MCP connect worker calls codec<InitializeParams>() while exit\n"
            "handlers run and reads freed std::functions. That crashed the\n"
            "host's test binary about 1 run in 10, with a backtrace inside\n"
            "std::function::operator() that looks like a corrupted stack.\n"
            "Keep the leaked pointer: one small immutable object per type.\n";
    }
}
