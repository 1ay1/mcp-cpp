// SPDX-License-Identifier: Apache-2.0
//
// web_search_services_test.cpp -- the web_search service catalogue: request shapes,
// reply parsing, failure classification and the failover loop, all against a
// scripted HttpClient (no network).
//
// The JSON bodies below are trimmed copies of replies recorded from the real
// services (or, for the keyed ones, of the vendor's published example), so a
// parser that drifts from the documented shape fails here rather than in a
// user's session.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/web_search.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/cap/local.hpp>

#include "../src/tools/web_search_services.hpp"
#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

using namespace mcp::tools;
using namespace mcp::tools::detail;
using nlohmann::json;

namespace {

const WebSearchService& svc(std::string_view id) {
    const WebSearchService* s = find_web_search_service(id);
    REQUIRE(s != nullptr);
    return *s;
}

WebSearchQuery query(std::string text = "rust async") {
    WebSearchQuery q;
    q.text = std::move(text);
    q.count = 5;
    return q;
}

std::string header(const HttpRequest& r, std::string_view name) {
    for (const auto& [k, v] : r.headers) if (k == name) return v;
    return {};
}

class Scripted : public HttpClient {
public:
    // First rule whose needle is in the URL wins.
    std::vector<std::pair<std::string, HttpResponse>> rules;
    std::vector<HttpRequest> seen;
    HttpResponse send(const HttpRequest& r) override {
        seen.push_back(r);
        for (auto& [needle, resp] : rules)
            if (r.url.find(needle) != std::string::npos) return resp;
        HttpResponse x; x.error = "no rule"; return x;
    }
};

HttpResponse reply(int status, std::string body) {
    HttpResponse r; r.status = status; r.body = std::move(body); return r;
}

struct FixedPlan : WebSearchPlanSource {
    WebSearchPlan p;
    explicit FixedPlan(WebSearchPlan x) : p(std::move(x)) {}
    WebSearchPlan plan() const override { return p; }
};

mcp::cap::Result run(const std::shared_ptr<Scripted>& http,
                     const WebSearchPlan& plan, std::string q = "rust async") {
    HostServices hs;
    hs.http = http;
    hs.web_search_plan = std::make_shared<FixedPlan>(plan);
    auto provider = make_provider(hs, ToolsetConfig{}, "local");
    mcp::Json args = mcp::Json::object();
    args["query"] = std::move(q);
    return provider->execute(mcp::cap::Request{"web_search", std::move(args)});
}

const char* kTavily =
    R"({"query":"q","follow_up_questions":null,"answer":null,"images":[],)"
    R"("results":[{"url":"https://tokio.rs/tokio/tutorial","title":"Tutorial | Tokio",)"
    R"("content":"Tokio is an asynchronous runtime.","score":0.9,"raw_content":null}],)"
    R"("response_time":0.4,"request_id":"x"})";

} // namespace

TEST_CASE("web search: search catalogue is well formed") {
    const auto& all = web_search_services();
    CHECK(all.size() == 10);
    for (const auto& s : all) {
        INFO(s.id);
        CHECK(!s.id.empty());
        CHECK(!s.label.empty());
        CHECK(s.max_count >= 1);
        CHECK(s.dials.size() <= 3);               // the pane has three dial rows
        CHECK(s.needs_key == !s.signup.empty());
        for (const auto& d : s.dials) {
            REQUIRE(!d.values.empty());
            CHECK(d.values.front().id.empty());   // [0] is "send nothing"
        }
        CHECK(find_web_search_service(s.id) == &s);
    }
    // Free and keyed variants are named apart, as the menu promises.
    CHECK(svc("brave-free").label == "Brave Free");
    CHECK(svc("brave-api").label == "Brave API key");
    CHECK(!svc("tavily-free").needs_key);
    CHECK(svc("tavily-api").needs_key);
    // Nothing is shipped whose schema could not be verified.
    CHECK(find_web_search_service("kagi-api") == nullptr);
    CHECK(find_web_search_service("mojeek-api") == nullptr);
    CHECK(find_web_search_service("linkup-api") == nullptr);
    CHECK(find_web_search_service("startpage") == nullptr);
}

TEST_CASE("web search: default plan is Brave, DuckDuckGo, Tavily -- all key-less") {
    const auto p = default_web_search_plan();
    REQUIRE(p.slots.size() == 3);
    CHECK(p.slots[0].service == "brave-free");
    CHECK(p.slots[1].service == "ddg-free");
    CHECK(p.slots[2].service == "tavily-free");
    for (const auto& s : p.slots) CHECK(!svc(s.service).needs_key);
}

TEST_CASE("web search: dials: only listed values reach a request") {
    WebSearchSlot slot{"ddg-free", {}, {{"region", "nz-en"}, {"safe", "1; DROP"},
                                      {"bogus", "x"}}};
    auto v = validate_dials(svc("ddg-free"), slot);
    REQUIRE(v.size() == 1);
    CHECK(v[0].first == "region");
    CHECK(v[0].second == "nz-en");
    // The default value is "send nothing", so it is dropped too.
    WebSearchSlot d{"ddg-free", {}, {{"region", ""}}};
    CHECK(validate_dials(svc("ddg-free"), d).empty());
}

TEST_CASE("web search: keys are checked before they become a header") {
    CHECK(clean_api_key("  tvly-abc123 \n") == "tvly-abc123");
    CHECK(clean_api_key("abc\r\nx-evil: 1").empty());
    CHECK(clean_api_key("two words").empty());
    CHECK(clean_api_key("").empty());
}

TEST_CASE("web search: -site: becomes a native filter only where the service has one") {
    auto s = split_site_exclusions("rust async -site:Reddit.com -site:pinterest.com");
    CHECK(s.query == "rust async");
    REQUIRE(s.exclude.size() == 2);
    CHECK(s.exclude[0] == "reddit.com");
    // Not a host: stays in the text.
    auto bad = split_site_exclusions("a -site:nonsense");
    CHECK(bad.query == "a -site:nonsense");
    CHECK(bad.exclude.empty());
    // Nothing but exclusions: sent as written, never an empty query.
    auto only = split_site_exclusions("-site:reddit.com");
    CHECK(only.query == "-site:reddit.com");
    CHECK(only.exclude.empty());

    CHECK(has_native_exclude("tavily-free"));
    CHECK(has_native_exclude("exa-api"));
    CHECK(!has_native_exclude("brave-free"));
    CHECK(!has_native_exclude("serper-api"));
}

TEST_CASE("web search: Brave Free request") {
    auto q = query("a b"); q.dials = {{"freshness", "pw"}};
    auto r = build_web_search_request(svc("brave-free"), "", q);
    CHECK(r.method == "GET");
    CHECK(r.url == "https://search.brave.com/search?q=a%20b&source=web&tf=pw");
}

TEST_CASE("web search: DuckDuckGo Free request") {
    auto q = query(); q.dials = {{"region", "nz-en"}, {"safe", "1"}};
    auto r = build_web_search_request(svc("ddg-free"), "", q);
    CHECK(r.method == "POST");
    CHECK(r.url == "https://html.duckduckgo.com/html/");
    CHECK(r.body == "q=rust%20async&kl=nz-en&kp=1");
    CHECK(header(r, "content-type") == "application/x-www-form-urlencoded");
    // No region chosen: the old behaviour (us-en) is kept.
    CHECK(build_web_search_request(svc("ddg-free"), "", query()).body
          == "q=rust%20async&kl=us-en");
}

TEST_CASE("web search: Tavily request: keyless header vs bearer key, exclusions and dials") {
    auto q = query(); q.exclude = {"reddit.com"}; q.language = "en";
    q.dials = {{"topic", "news"}, {"time_range", "week"}};
    auto free = build_web_search_request(svc("tavily-free"), "", q);
    CHECK(free.url == "https://api.tavily.com/search");
    CHECK(header(free, "x-tavily-access-mode") == "keyless");
    CHECK(header(free, "authorization").empty());
    auto b = json::parse(free.body);
    CHECK(b["query"] == "rust async");
    CHECK(b["max_results"] == 5);
    CHECK(b["exclude_domains"] == json::array({"reddit.com"}));
    CHECK(b["language"] == "en");
    CHECK(b["topic"] == "news");
    CHECK(b["time_range"] == "week");
    CHECK(b["include_answer"] == false);

    auto keyed = build_web_search_request(svc("tavily-api"), "tvly-k", q);
    CHECK(header(keyed, "authorization") == "Bearer tvly-k");
    CHECK(header(keyed, "x-tavily-access-mode").empty());
    CHECK(keyed.url.find("tvly-k") == std::string::npos);
}

TEST_CASE("web search: Firecrawl request") {
    auto q = query(); q.exclude = {"github.com"};
    q.dials = {{"freshness", "qdr:m"}, {"category", "research"}, {"country", "NZ"}};
    auto r = build_web_search_request(svc("firecrawl-free"), "", q);
    CHECK(r.url == "https://api.firecrawl.dev/v2/search");
    CHECK(header(r, "authorization").empty());
    auto b = json::parse(r.body);
    CHECK(b["limit"] == 5);
    CHECK(b["excludeDomains"] == json::array({"github.com"}));
    CHECK(b["tbs"] == "qdr:m");
    CHECK(b["categories"] == json::array({"research"}));
    CHECK(b["country"] == "NZ");
    CHECK(header(build_web_search_request(svc("firecrawl-api"), "fc-1", q),
                 "authorization") == "Bearer fc-1");
}

TEST_CASE("web search: Brave API request keeps the key out of the URL") {
    auto q = query(); q.language = "de"; q.count = 7;
    q.dials = {{"country", "DE"}, {"freshness", "pm"}, {"safesearch", "strict"}};
    auto r = build_web_search_request(svc("brave-api"), "BSA-secret", q);
    CHECK(r.method == "GET");
    CHECK(r.url == "https://api.search.brave.com/res/v1/web/search?q=rust%20async"
                   "&count=7&text_decorations=false&search_lang=de&country=DE"
                   "&freshness=pm&safesearch=strict");
    CHECK(header(r, "x-subscription-token") == "BSA-secret");
    CHECK(r.url.find("BSA-secret") == std::string::npos);
    // A language Brave's table does not list is simply not sent.
    q.language = "xx";
    CHECK(build_web_search_request(svc("brave-api"), "k", q).url.find("search_lang")
          == std::string::npos);
    q.language = "zh";
    CHECK(build_web_search_request(svc("brave-api"), "k", q).url
          .find("search_lang=zh-hans") != std::string::npos);
}

TEST_CASE("web search: Exa, Serper and Perplexity requests") {
    auto q = query(); q.language = "fr";
    q.now = std::chrono::system_clock::from_time_t(1'760'000'000);  // 2025-10-09
    q.dials = {{"type", "fast"}, {"category", "news"}, {"freshness", "week"}};
    auto e = build_web_search_request(svc("exa-api"), "exa-k", q);
    CHECK(e.url == "https://api.exa.ai/search");
    CHECK(header(e, "x-api-key") == "exa-k");
    auto eb = json::parse(e.body);
    CHECK(eb["numResults"] == 5);
    CHECK(eb["contents"]["highlights"] == true);
    CHECK(eb["type"] == "fast");
    CHECK(eb["category"] == "news");
    CHECK(eb["startPublishedDate"] == "2025-10-02T08:53:20.000Z");

    q.dials = {{"country", "NZ"}, {"freshness", "qdr:d"}, {"autocorrect", "false"}};
    auto s = build_web_search_request(svc("serper-api"), "sp-k", q);
    CHECK(s.url == "https://google.serper.dev/search");
    CHECK(header(s, "x-api-key") == "sp-k");
    auto sb = json::parse(s.body);
    CHECK(sb["q"] == "rust async");
    CHECK(sb["num"] == 5);
    CHECK(sb["gl"] == "nz");
    CHECK(sb["hl"] == "fr");
    CHECK(sb["tbs"] == "qdr:d");
    CHECK(sb["autocorrect"] == false);

    q.dials = {{"country", "NZ"}, {"recency", "month"}};
    auto p = build_web_search_request(svc("perplexity-api"), "pp-k", q);
    CHECK(p.url == "https://api.perplexity.ai/search");
    CHECK(header(p, "authorization") == "Bearer pp-k");
    auto pb = json::parse(p.body);
    CHECK(pb["max_results"] == 5);
    CHECK(pb["country"] == "NZ");
    CHECK(pb["search_recency_filter"] == "month");
    CHECK(pb["search_language_filter"] == json::array({"fr"}));
}

TEST_CASE("web search: reply parsers") {
    auto t = parse_json_hits("tavily-free", kTavily, 10);
    REQUIRE(t.ok);
    REQUIRE(t.hits.size() == 1);
    CHECK(t.hits[0].url == "https://tokio.rs/tokio/tutorial");
    CHECK(t.hits[0].title == "Tutorial | Tokio");
    CHECK(t.hits[0].snippet == "Tokio is an asynchronous runtime.");

    auto f = parse_json_hits("firecrawl-free",
        R"({"success":true,"data":{"web":[{"url":"https://a.test/","title":"A",)"
        R"("description":"d","position":1},{"url":"https://b.test/","title":"B"}]},)"
        R"("creditsUsed":2})", 1);
    REQUIRE(f.ok);
    CHECK(f.hits.size() == 1);                       // clamped to count
    CHECK(f.hits[0].snippet == "d");

    auto e = parse_json_hits("exa-api",
        R"({"results":[{"title":"T","url":"https://e.test/","highlights":["one","two"]}]})", 5);
    REQUIRE(e.hits.size() == 1);
    CHECK(e.hits[0].snippet == "one ... two");

    auto s = parse_json_hits("serper-api",
        R"({"organic":[{"title":"G","link":"https://g.test/","snippet":"s","position":1}]})", 5);
    REQUIRE(s.hits.size() == 1);
    CHECK(s.hits[0].url == "https://g.test/");
    // Serper leaves `organic` out when there is nothing: that is "no results".
    auto none = parse_json_hits("serper-api", R"({"searchParameters":{}})", 5);
    CHECK(none.ok);
    CHECK(none.hits.empty());

    auto p = parse_json_hits("perplexity-api",
        R"({"results":[{"title":"P","url":"https://p.test/","snippet":"ps","date":"2025-01-01"}],"id":"x"})", 5);
    REQUIRE(p.hits.size() == 1);
    CHECK(p.hits[0].snippet == "ps");

    auto b = parse_json_hits("brave-api",
        R"({"type":"search","web":{"results":[{"title":"B","url":"https://b.test/","description":"bd"}]}})", 5);
    REQUIRE(b.hits.size() == 1);
    CHECK(b.hits[0].snippet == "bd");

    // Firecrawl's description is page markdown (recorded live). The snippet
    // is plain text, the way the scrapers always produced it.
    auto md = parse_json_hits("firecrawl-free",
        R"({"success":true,"data":{"web":[{"url":"https://corrode.dev/blog/async/",)"
        R"("title":"T","description":"## [Why Bother?](https://corrode.dev/blog/async/#why-bother)\nIn the **first** article"}]}})", 5);
    REQUIRE(md.hits.size() == 1);
    CHECK(md.hits[0].snippet == "Why Bother? In the first article");

    // Wrong shape is "unreadable", not "no results".
    CHECK(!parse_json_hits("tavily-free", "<html>nope</html>", 5).ok);
    CHECK(!parse_json_hits("tavily-free", R"({"detail":{"error":"x"}})", 5).ok);
    CHECK(!parse_json_hits("firecrawl-free", R"({"success":false,"error":"x"})", 5).ok);
}

TEST_CASE("web search: failures are told apart") {
    const auto& brave = svc("brave-free");
    const auto& tavily = svc("tavily-api");
    CHECK(classify_http(brave, 202, "select all squares containing a duck")
          == WebSearchFailure::Blocked);
    CHECK(classify_http(brave, 429, "") == WebSearchFailure::RateLimited);
    CHECK(classify_http(tavily, 401, "") == WebSearchFailure::BadKey);
    CHECK(classify_http(tavily, 432, "") == WebSearchFailure::QuotaExhausted);
    CHECK(classify_http(svc("exa-api"), 402, "") == WebSearchFailure::QuotaExhausted);
    // Brave's API answers a rejected token with 422, recorded live.
    CHECK(classify_http(svc("brave-api"), 422,
          R"({"error":{"code":"SUBSCRIPTION_TOKEN_INVALID"}})") == WebSearchFailure::BadKey);
    CHECK(classify_http(svc("brave-api"), 422, "{}") == WebSearchFailure::BadRequest);
    // A key-less service refusing us is access withdrawn, not a bad key.
    CHECK(classify_http(svc("tavily-free"), 401, "") == WebSearchFailure::Blocked);
    CHECK(classify_http(brave, 503, "") == WebSearchFailure::Transient);
}

TEST_CASE("web search: a normal results page is not mistaken for a captcha") {
    // Every real Brave results page carries this string in its inline i18n
    // bundle. Matching the bare word "captcha" turned a page the parser found
    // nothing on (a layout change, a query with no hits) into "blocked".
    const std::string ok_page =
        R"(<title>rust async - Brave Search</title><script>)"
        R"({"Switch to traditional captcha":"Switch to traditional CAPTCHA"})"
        R"(</script><div class="snippet svelte-x">...</div>)";
    CHECK(classify_http(svc("brave-free"), 200, ok_page) == WebSearchFailure::Unreadable);

    // The two challenge pages, by the markers only they carry.
    const std::string brave_challenge =
        R"(Your request has been flagged as being suspicious and Brave Search )"
        R"(decided to schedule a captcha for you.<script>{page:"/captcha"}</script>)";
    CHECK(classify_http(svc("brave-free"), 200, brave_challenge) == WebSearchFailure::Blocked);
    const std::string ddg_challenge =
        R"(<div class="anomaly-modal__title">Unfortunately, bots use DuckDuckGo too.</div>)";
    CHECK(classify_http(svc("ddg-free"), 200, ddg_challenge) == WebSearchFailure::Blocked);
}

TEST_CASE("web search: fails over, and says why") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"search.brave.com", reply(429, "slow down")});
    http->rules.push_back({"html.duckduckgo.com",
        reply(202, "<p>Unfortunately, bots use DuckDuckGo too. select all squares</p>")});
    http->rules.push_back({"api.tavily.com", reply(200, kTavily)});

    auto r = run(http, default_web_search_plan());
    CHECK(!r.is_error);
    CHECK(r.text.find("[via Tavily Free]") != std::string::npos);
    CHECK(r.text.find("https://tokio.rs/tokio/tutorial") != std::string::npos);
    REQUIRE(http->seen.size() == 3);
    CHECK(http->seen[0].url.find("search.brave.com") != std::string::npos);
    CHECK(http->seen[2].url == "https://api.tavily.com/search");

    // Every slot failing: the report names each service and the reason.
    http->rules.clear();
    http->rules.push_back({"search.brave.com", reply(429, "")});
    http->rules.push_back({"html.duckduckgo.com", reply(202, "select all squares")});
    http->rules.push_back({"api.tavily.com", reply(429, "")});
    auto all = run(http, default_web_search_plan());
    // (Unchanged contract: "no results" is a normal reply, not a tool error.)
    CHECK(all.text.find("search returned no results") != std::string::npos);
    CHECK(all.text.find("Brave Free") != std::string::npos);
    CHECK(all.text.find("rate limited") != std::string::npos);
    CHECK(all.text.find("DuckDuckGo Free") != std::string::npos);
    CHECK(all.text.find("blocked") != std::string::npos);
    // The old, misleading wording for a challenge page is gone.
    CHECK(all.text.find("layout may have changed") == std::string::npos);
}

TEST_CASE("web search: a keyed slot without a key is skipped, not sent") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"api.tavily.com", reply(200, kTavily)});
    WebSearchPlan plan;
    plan.slots = {WebSearchSlot{"exa-api", "", {}},          // no key
                  WebSearchSlot{"tavily-free", "", {}}};
    auto r = run(http, plan);
    CHECK(!r.is_error);
    CHECK(r.text.find("[via Tavily Free]") != std::string::npos);
    REQUIRE(http->seen.size() == 1);                       // Exa was never called
}

TEST_CASE("web search: a keyed answer, the key sent only in its header") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"google.serper.dev",
        reply(200, R"({"organic":[{"title":"G","link":"https://g.test/","snippet":"s"}]})")});
    WebSearchPlan plan;
    plan.slots = {WebSearchSlot{"serper-api", "sp-secret", {{"country", "NZ"}}}};
    plan.language = "en";
    auto r = run(http, plan);
    CHECK(!r.is_error);
    CHECK(r.text.find("[via Serper API key]") != std::string::npos);
    REQUIRE(http->seen.size() == 1);
    CHECK(header(http->seen[0], "x-api-key") == "sp-secret");
    CHECK(http->seen[0].url.find("sp-secret") == std::string::npos);
    CHECK(json::parse(http->seen[0].body)["gl"] == "nz");
}

TEST_CASE("web search: -site: reaches a native filter, and stays in the text elsewhere") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"api.tavily.com", reply(200, kTavily)});
    WebSearchPlan plan; plan.slots = {WebSearchSlot{"tavily-free", "", {}}};
    run(http, plan, "rust async -site:reddit.com");
    REQUIRE(http->seen.size() == 1);
    auto b = json::parse(http->seen[0].body);
    CHECK(b["query"] == "rust async");
    CHECK(b["exclude_domains"] == json::array({"reddit.com"}));

    auto http2 = std::make_shared<Scripted>();
    http2->rules.push_back({"google.serper.dev", reply(200, R"({"organic":[]})")});
    WebSearchPlan p2; p2.slots = {WebSearchSlot{"serper-api", "k", {}}};
    run(http2, p2, "rust async -site:reddit.com");
    REQUIRE(http2->seen.size() == 1);
    CHECK(json::parse(http2->seen[0].body)["q"] == "rust async -site:reddit.com");
}

TEST_CASE("web search: at most three slots are tried") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"api.tavily.com", reply(500, "")});
    WebSearchPlan plan;
    for (int i = 0; i < 5; ++i) plan.slots.push_back(WebSearchSlot{"tavily-free", "", {}});
    run(http, plan);
    CHECK(http->seen.size() == 3);
}

TEST_CASE("web search: no plan source means the key-less default") {
    auto http = std::make_shared<Scripted>();
    http->rules.push_back({"api.tavily.com", reply(200, kTavily)});
    http->rules.push_back({"search.brave.com", reply(429, "")});
    http->rules.push_back({"html.duckduckgo.com", reply(202, "select all squares")});
    HostServices hs; hs.http = http;               // web_search_plan left null
    auto provider = make_provider(hs, ToolsetConfig{}, "local");
    mcp::Json args = mcp::Json::object(); args["query"] = "x";
    auto r = provider->execute(mcp::cap::Request{"web_search", std::move(args)});
    CHECK(!r.is_error);
    CHECK(r.text.find("[via Tavily Free]") != std::string::npos);
}
