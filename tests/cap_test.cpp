// SPDX-License-Identifier: Apache-2.0
//
// cap_test.cpp — the capability layer (cap/). Exercises LocalProvider, the
// Registry's namespacing + dispatch routing, content<->Result helpers, and
// the multi-provider fan-in. No subprocess / network: StdioServerProvider's
// live path needs a real MCP server, so we cover the in-process surface that
// every backed provider shares.
//
#include <mcp/cap/cap.hpp>

#include <iostream>
#include <memory>
#include <string>

using namespace mcp;
using namespace mcp::cap;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << "\n";                                      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

std::shared_ptr<LocalProvider> make_calc() {
    auto p = std::make_shared<LocalProvider>("calc");
    p->add("add", "add two ints",
           Json{{"type","object"},
                {"properties",{{"a",{{"type","integer"}}},{"b",{{"type","integer"}}}}}},
           [](const Json& a) {
               long s = a.value("a", 0L) + a.value("b", 0L);
               return Result::ok("sum=" + std::to_string(s));
           });
    p->add("boom", "always errors", Json{{"type","object"}},
           [](const Json&) -> Result { throw std::runtime_error("kaboom"); });
    return p;
}

// (a) LocalProvider lists tools and dispatches handlers; thrown handler →
//     Result::error, not a crash.
void test_local_provider() {
    auto calc = make_calc();
    CHECK(calc->origin() == "calc");
    CHECK(calc->list().size() == 2);

    auto r = calc->execute(Request{"add", Json{{"a",17},{"b",25}}});
    CHECK(!r.is_error);
    CHECK(r.text == "sum=42");

    auto miss = calc->execute(Request{"nope", Json::object()});
    CHECK(miss.is_error);

    auto thrown = calc->execute(Request{"boom", Json::object()});
    CHECK(thrown.is_error);                 // exception caught → error result
    CHECK(thrown.text.find("kaboom") != std::string::npos);
}

// (b) Single-provider Registry: bare names, dispatch routes to the provider.
void test_registry_single() {
    Registry reg;
    reg.add(make_calc());
    CHECK(reg.provider_count() == 1);

    auto tools = reg.tools();
    CHECK(tools.size() == 2);
    // Unambiguous → bare name (no "calc__" prefix).
    bool saw_bare_add = false;
    for (const auto& t : tools) if (t.name == "add") saw_bare_add = true;
    CHECK(saw_bare_add);

    auto r = reg.dispatch("add", Json{{"a",1},{"b",2}});
    CHECK(!r.is_error);
    CHECK(r.text == "sum=3");

    CHECK(reg.dispatch("ghost").is_error);  // unknown tool
}

// (c) Collision across providers → automatic "<origin>__<name>" namespacing,
//     and dispatch resolves the namespaced form to the right provider.
void test_registry_collision_namespacing() {
    auto a = std::make_shared<LocalProvider>("alpha");
    a->add("ping", "alpha ping", Json{{"type","object"}},
           [](const Json&) { return Result::ok("from-alpha"); });
    auto b = std::make_shared<LocalProvider>("beta");
    b->add("ping", "beta ping", Json{{"type","object"}},
           [](const Json&) { return Result::ok("from-beta"); });

    Registry reg;
    reg.add(a);
    reg.add(b);

    auto tools = reg.tools();
    CHECK(tools.size() == 2);
    bool saw_alpha = false, saw_beta = false;
    for (const auto& t : tools) {
        if (t.name == "alpha__ping") saw_alpha = true;
        if (t.name == "beta__ping")  saw_beta  = true;
    }
    CHECK(saw_alpha);
    CHECK(saw_beta);

    CHECK(reg.dispatch("alpha__ping").text == "from-alpha");
    CHECK(reg.dispatch("beta__ping").text  == "from-beta");
    // The bare colliding name is NOT routable (ambiguous).
    CHECK(reg.dispatch("ping").is_error);
}

// (d) always_namespace forces the prefix even without a collision.
void test_registry_always_namespace() {
    Registry reg(/*always_namespace=*/true);
    reg.add(make_calc());
    auto tools = reg.tools();
    bool saw_ns = false;
    for (const auto& t : tools) if (t.name == "calc__add") saw_ns = true;
    CHECK(saw_ns);
    CHECK(reg.dispatch("calc__add", Json{{"a",4},{"b",4}}).text == "sum=8");
}

// (e) content <-> Result helpers round-trip text + error + structured.
void test_content_helpers() {
    Result r = Result::ok("hello world");
    r.structured = Json{{"k", 1}};
    CallToolResult ct = call_result_from(r);
    CHECK(ct.content.size() == 1);
    CHECK(ct.structuredContent.has_value());

    Result back = result_from_call(ct);
    CHECK(!back.is_error);
    CHECK(back.text.find("hello world") != std::string::npos);
    CHECK(back.structured == r.structured);

    // isError survives the round-trip.
    CallToolResult err = call_result_from(Result::error("nope"));
    CHECK(err.isError.has_value() && *err.isError);
    CHECK(result_from_call(err).is_error);
}

} // namespace

int main() {
    test_local_provider();
    test_registry_single();
    test_registry_collision_namespacing();
    test_registry_always_namespace();
    test_content_helpers();

    if (g_failures == 0) { std::cout << "cap_test: all checks passed\n"; return 0; }
    std::cerr << "cap_test: " << g_failures << " failure(s)\n";
    return 1;
}
