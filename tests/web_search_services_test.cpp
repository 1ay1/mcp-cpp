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

// The value ids one dial offers, in menu order ("" first).
std::vector<std::string> dial_ids(std::string_view service, std::string_view dial) {
    std::vector<std::string> out;
    for (const auto& d : svc(service).dials)
        if (d.id == dial) for (const auto& v : d.values) out.push_back(v.id);
    return out;
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
        CHECK(s.dials.size() <= kMaxWebSearchDials);
        CHECK(s.needs_key == !s.signup.empty());
        for (const auto& d : s.dials) {
            INFO(d.id);
            if (d.text != WebSearchText::None) {   // free text: no list, a bound
                CHECK(d.values.empty());
                CHECK(d.max_bytes > 0);
                continue;
            }
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

TEST_CASE("web search: dials offer each vendor's documented values") {
    using V = std::vector<std::string>;
    const V hourly_tbs{"", "qdr:h", "qdr:d", "qdr:w", "qdr:m", "qdr:y"};
    const V categories{"", "developer", "research", "pdf", "gov"};
    const V exa_types{"", "fast", "instant", "deep-lite", "deep", "deep-reasoning"};
    const V exa_ages{"", "hour", "day", "week", "month", "year"};
    // Firecrawl's four categories; github is no longer one.
    for (const char* id : {"firecrawl-free", "firecrawl-api"}) {
        INFO(id);
        CHECK(dial_ids(id, "category") == categories);
        CHECK(dial_ids(id, "freshness") == hourly_tbs);
        WebSearchSlot old{id, {}, {{"category", "github"}}};
        CHECK(validate_dials(svc(id), old).empty());
    }
    CHECK(dial_ids("exa-api", "type") == exa_types);
    CHECK(dial_ids("exa-api", "freshness") == exa_ages);
    CHECK(dial_ids("serper-api", "freshness") == hourly_tbs);
    // Brave's "no country" sits right under the default.
    const auto brave = dial_ids("brave-api", "country");
    REQUIRE(brave.size() >= 2);
    CHECK(brave[1] == "ALL");
    // DuckDuckGo: all 68 documented regions, us-en being the "" default.
    const auto kl = dial_ids("ddg-free", "region");
    CHECK(kl.size() == 68);
    CHECK(std::find(kl.begin(), kl.end(), "us-en") == kl.end());
    for (const char* r : {"wt-wt", "nl-nl", "xl-es", "ue-es", "tw-tzh", "vn-vi"})
        CHECK(std::find(kl.begin(), kl.end(), r) != kl.end());
    // No dial lists a value twice.
    for (const auto& s : web_search_services())
        for (const auto& d : s.dials) {
            V ids = dial_ids(s.id, d.id);
            std::sort(ids.begin(), ids.end());
            const std::string where = s.id + "." + d.id;
            CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), where);
        }
}

TEST_CASE("web search: the new dials pass validation and fill the rows") {
    auto dials = [](std::string_view id) { return svc(id).dials.size(); };
    CHECK(dials("tavily-free") == kMaxWebSearchDials);
    CHECK(dials("tavily-api") == kMaxWebSearchDials);
    CHECK(dials("firecrawl-free") == 6);
    CHECK(dials("firecrawl-api") == 6);
    CHECK(dials("brave-api") == 5);
    CHECK(dials("exa-api") == 5);
    CHECK(dials("perplexity-api") == 3);
    WebSearchSlot t{"tavily-free", {}, {{"exact_match", "true"}, {"language_filter", "only"},
                                       {"country", "JP"}, {"safe_search", "true"},
                                       {"depth", "ultra-fast"}}};
    CHECK(validate_dials(svc("tavily-free"), t).size() == 5);
    WebSearchSlot f{"firecrawl-api", {}, {{"sort", "date"}, {"safe", "true"}}};
    CHECK(validate_dials(svc("firecrawl-api"), f).size() == 2);
    WebSearchSlot e{"exa-api", {}, {{"moderation", "true"}, {"country", "GB"}}};
    CHECK(validate_dials(svc("exa-api"), e).size() == 2);
}

TEST_CASE("web search: text dials take checked free text") {
    const WebSearchDial* goggle = nullptr;
    for (const auto& d : svc("brave-api").dials) if (d.id == "goggles") goggle = &d;
    REQUIRE(goggle != nullptr);
    const std::string url = "https://raw.githubusercontent.com/brave/goggles-quickstart/"
                            "main/goggles/tech_blogs.goggle";
    CHECK(web_search_dial_value_ok(*goggle, url));
    CHECK(!web_search_dial_value_ok(*goggle, ""));
    CHECK(!web_search_dial_value_ok(*goggle, "https://"));
    CHECK(!web_search_dial_value_ok(*goggle, "http://example.com/a.goggle"));
    CHECK(!web_search_dial_value_ok(*goggle, "$boost=3,site=dev.to"));   // inline rules
    CHECK(!web_search_dial_value_ok(*goggle, "https://a.com/x y"));
    CHECK(!web_search_dial_value_ok(*goggle, "https://a.com/x\n$discard"));
    CHECK(!web_search_dial_value_ok(*goggle, "https://a.com/" + std::string(2048, 'x')));

    const WebSearchDial* where = nullptr;
    for (const auto& d : svc("firecrawl-free").dials) if (d.id == "location") where = &d;
    REQUIRE(where != nullptr);
    CHECK(web_search_dial_value_ok(*where, "San Francisco,California,United States"));
    CHECK(web_search_dial_value_ok(*where, "Z\xc3\xbcrich"));          // UTF-8 is fine
    CHECK(!web_search_dial_value_ok(*where, "Z\xfcrich"));             // Latin-1 is not
    CHECK(!web_search_dial_value_ok(*where, "line one\r\nline two"));
    CHECK(!web_search_dial_value_ok(*where, std::string(201, 'a')));

    // validate_dials applies the same rule, so a bad value never reaches a slot.
    WebSearchSlot ok{"brave-api", {}, {{"goggles", url}}};
    CHECK(validate_dials(svc("brave-api"), ok).size() == 1);
    WebSearchSlot bad{"brave-api", {}, {{"goggles", "javascript:alert(1)"}}};
    CHECK(validate_dials(svc("brave-api"), bad).empty());
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
    // At most 20 lifted out (Perplexity's limit); the rest stay in the text.
    std::string many = "q";
    for (int i = 0; i < 21; ++i) many += " -site:s" + std::to_string(i) + ".test";
    auto capped = split_site_exclusions(many);
    CHECK(capped.exclude.size() == 20);
    CHECK(capped.query == "q -site:s20.test");

    CHECK(has_native_exclude("tavily-free"));
    CHECK(has_native_exclude("exa-api"));
    CHECK(has_native_exclude("perplexity-api"));
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

TEST_CASE("web search: Tavily filters follow the reference's rules") {
    auto body = [](std::string_view id, const WebSearchQuery& q) {
        return json::parse(build_web_search_request(svc(id), "", q).body);
    };
    auto q = query(); q.language = "de";
    q.dials = {{"exact_match", "true"}, {"language_filter", "only"},
               {"country", "NZ"}, {"safe_search", "true"}, {"depth", "advanced"}};
    for (const char* id : {"tavily-free", "tavily-api"}) {
        INFO(id);
        auto b = body(id, q);
        CHECK(b["exact_match"] == true);
        CHECK(b["filter_by_language"] == true);
        CHECK(b["country"] == "new zealand");      // Tavily wants the name
        CHECK(b["safe_search"] == true);
        CHECK(b["search_depth"] == "advanced");    // key-less now offers depth too
    }
    // country is for the general topic only.
    q.dials = {{"country", "NZ"}, {"topic", "news"}};
    CHECK(!body("tavily-free", q).contains("country"));
    // safe_search is not supported with the fast depths.
    for (const char* depth : {"fast", "ultra-fast"}) {
        q.dials = {{"safe_search", "true"}, {"depth", depth}};
        CHECK(!body("tavily-free", q).contains("safe_search"), depth);
    }
    // filter_by_language without a language is a 400, so it is not sent.
    q.language.clear();
    q.dials = {{"language_filter", "only"}};
    CHECK(!body("tavily-free", q).contains("filter_by_language"));
    // Defaults send nothing new.
    q.dials = {};
    const auto plain = body("tavily-free", q);
    for (const char* k : {"exact_match", "filter_by_language", "country",
                          "safe_search", "search_depth"})
        CHECK(!plain.contains(k), k);
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
    CHECK(!b.contains("safe"));
    CHECK(header(build_web_search_request(svc("firecrawl-api"), "fc-1", q),
                 "authorization") == "Bearer fc-1");

    // Sorting by date joins the window in tbs; alone it is just sbd:1.
    q.dials = {{"freshness", "qdr:w"}, {"sort", "date"}, {"safe", "true"},
               {"location", "Germany"}};
    for (const char* id : {"firecrawl-free", "firecrawl-api"}) {
        INFO(id);
        auto sb = json::parse(build_web_search_request(svc(id), "k", q).body);
        CHECK(sb["tbs"] == "sbd:1,qdr:w");
        CHECK(sb["safe"] == true);
        CHECK(sb["location"] == "Germany");
    }
    q.dials = {{"sort", "date"}};
    CHECK(json::parse(build_web_search_request(svc("firecrawl-free"), "", q).body)
          ["tbs"] == "sbd:1");
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
    // Spell correction off is sent; on is Brave's default and sends nothing.
    q.dials = {{"spellcheck", "false"}, {"country", "ALL"}};
    const auto url = build_web_search_request(svc("brave-api"), "k", q).url;
    CHECK(url.find("&spellcheck=false") != std::string::npos);
    CHECK(url.find("&country=ALL") != std::string::npos);
    CHECK(url.find("goggles") == std::string::npos);
    // A Goggle URL is percent-encoded into its own parameter.
    q.dials = {{"goggles", "https://g.example/a b.goggle?x=1&y"}};
    CHECK(build_web_search_request(svc("brave-api"), "k", q).url.find(
          "&goggles=https%3A%2F%2Fg.example%2Fa%20b.goggle%3Fx%3D1%26y") != std::string::npos);
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
    q.dials = {{"freshness", "hour"}};
    CHECK(json::parse(build_web_search_request(svc("exa-api"), "k", q).body)
          ["startPublishedDate"] == "2025-10-09T07:53:20.000Z");
    q.dials = {{"type", "deep-reasoning"}};
    CHECK(json::parse(build_web_search_request(svc("exa-api"), "k", q).body)
          ["type"] == "deep-reasoning");
    q.dials = {{"moderation", "true"}, {"country", "NZ"}};
    auto em = json::parse(build_web_search_request(svc("exa-api"), "k", q).body);
    CHECK(em["moderation"] == true);
    CHECK(em["userLocation"] == "NZ");
    CHECK(!eb.contains("moderation"));
    CHECK(!eb.contains("userLocation"));

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
    CHECK(!pb.contains("search_type"));
    CHECK(!pb.contains("search_domain_filter"));
    q.dials = {{"search_type", "fast"}};
    CHECK(json::parse(build_web_search_request(svc("perplexity-api"), "k", q).body)
          ["search_type"] == "fast");
    // Excluded hosts become the deny form of its domain filter.
    q.dials = {};
    q.exclude = {"reddit.com", "pinterest.com"};
    const json deny = json::array({"-reddit.com", "-pinterest.com"});
    CHECK(json::parse(build_web_search_request(svc("perplexity-api"), "k", q).body)
          ["search_domain_filter"] == deny);
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

TEST_CASE("web search: Firecrawl paper records each get a link") {
    // The record shape is the documented research example.
    auto p = parse_json_hits("firecrawl-api",
        R"({"success":true,"data":{"research":[)"
        R"({"paperId":"2014215642691656232","primaryId":"arxiv:2105.05233",)"
        R"("ids":{"arxiv":["2105.05233"]},"title":"Diffusion Models Beat GANs on Image Synthesis",)"
        R"("abstract":"We show that diffusion models can achieve image sample quality.","score":0.016},)"
        R"({"paperId":"1","primaryId":"pmid:31452104","title":"P"},)"
        R"({"paperId":"2","primaryId":"pmcid:PMC6824467","title":"C"},)"
        R"({"paperId":"3","primaryId":"pmcid:6824467","title":"C2"},)"
        R"({"paperId":"4","primaryId":"doi:10.1038/s41586-021-03819-2","title":"D"},)"
        R"({"paperId":"5","primaryId":"arxiv:hep-th/9901001","title":"Old arXiv"},)"
        R"({"paperId":"6","primaryId":"doi:10.1000/a b","title":"Odd DOI"},)"
        R"({"paperId":"7","primaryId":"openalex:W2963403868","title":"Unknown kind"},)"
        R"({"paperId":"8","title":"No primary id"},)"
        R"({"title":"Nothing to link"})"
        R"(]}})", 20);
    REQUIRE(p.ok);
    REQUIRE(p.hits.size() == 9);   // the last record has no id at all
    CHECK(p.hits[0].url == "https://arxiv.org/abs/2105.05233");
    CHECK(p.hits[0].title == "Diffusion Models Beat GANs on Image Synthesis");
    CHECK(p.hits[0].snippet == "We show that diffusion models can achieve image sample quality.");
    CHECK(p.hits[1].url == "https://pubmed.ncbi.nlm.nih.gov/31452104/");
    CHECK(p.hits[2].url == "https://pmc.ncbi.nlm.nih.gov/articles/PMC6824467/");
    CHECK(p.hits[3].url == "https://pmc.ncbi.nlm.nih.gov/articles/PMC6824467/");
    CHECK(p.hits[4].url == "https://doi.org/10.1038/s41586-021-03819-2");
    CHECK(p.hits[5].url == "https://arxiv.org/abs/hep-th/9901001");
    CHECK(p.hits[6].url == "https://doi.org/10.1000/a%20b");
    // A kind of id Firecrawl does not document, or none: its own paper page.
    CHECK(p.hits[7].url == "https://api.firecrawl.dev/v2/search/research/papers/7");
    CHECK(p.hits[8].url == "https://api.firecrawl.dev/v2/search/research/papers/8");

    // Web results come first, and the count covers both lists.
    auto mixed = parse_json_hits("firecrawl-free",
        R"({"success":true,"data":{"web":[{"url":"https://a.test/","title":"A"}],)"
        R"("research":[{"paperId":"1","primaryId":"pmid:1","title":"P1"},)"
        R"({"paperId":"2","primaryId":"pmid:2","title":"P2"}]}})", 2);
    REQUIRE(mixed.ok);
    REQUIRE(mixed.hits.size() == 2);
    CHECK(mixed.hits[0].url == "https://a.test/");
    CHECK(mixed.hits[1].url == "https://pubmed.ncbi.nlm.nih.gov/1/");

    // Either list makes a readable reply; an empty one is "no results".
    auto empty = parse_json_hits("firecrawl-free", R"({"success":true,"data":{"research":[]}})", 5);
    CHECK(empty.ok);
    CHECK(empty.hits.empty());
    CHECK(!parse_json_hits("firecrawl-free", R"({"success":true,"data":{}})", 5).ok);
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

    auto http3 = std::make_shared<Scripted>();
    http3->rules.push_back({"api.perplexity.ai", reply(200, R"({"results":[]})")});
    WebSearchPlan p3; p3.slots = {WebSearchSlot{"perplexity-api", "k", {}}};
    run(http3, p3, "rust async -site:reddit.com");
    REQUIRE(http3->seen.size() == 1);
    auto pb = json::parse(http3->seen[0].body);
    CHECK(pb["query"] == "rust async");
    CHECK(pb["search_domain_filter"] == json::array({"-reddit.com"}));
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
