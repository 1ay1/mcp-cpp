// SPDX-License-Identifier: Apache-2.0
//
// web_search_services.cpp -- the catalogue of web_search services, and for each
// one the request to send and the reply to read.
//
// Every request shape and every response path below was checked against the
// vendor's own documentation, and the key-less ones against a live reply. The
// evidence (and the things that are deliberately NOT here because their
// schema could not be verified) is written up in the agentty repository's
// research notes for this change; the one-line version sits next to each
// builder.

#include "web_search_services.hpp"

#include <mcp/tools/util/utf8.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;

namespace {

// ── Catalogue helpers ──────────────────────────────────────────────────

WebSearchDialValue V(std::string id, std::string label) {
    return WebSearchDialValue{std::move(id), std::move(label)};
}

WebSearchDial freshness_dial(std::string id, std::string help,
                          std::vector<WebSearchDialValue> vals) {
    std::vector<WebSearchDialValue> v;
    v.push_back(V("", "Any time"));
    for (auto& x : vals) v.push_back(std::move(x));
    return WebSearchDial{std::move(id), "Freshness", std::move(help), std::move(v)};
}

// Past day / week / month / year, with the vendor's own spellings.
std::vector<WebSearchDialValue> dwmy(std::string d, std::string w,
                                  std::string m, std::string y) {
    return {V(std::move(d), "Past day"), V(std::move(w), "Past week"),
            V(std::move(m), "Past month"), V(std::move(y), "Past year")};
}

// ISO 3166-1 alpha-2, upper case. The shared short list: enough to cover
// where most users are, without a 250-row dropdown nobody scrolls.
WebSearchDial country_dial(std::string help) {
    return WebSearchDial{"country", "Country", std::move(help), {
        V("", "Service default"),
        V("US", "United States"), V("GB", "United Kingdom"),
        V("NZ", "New Zealand"),   V("AU", "Australia"),
        V("CA", "Canada"),        V("DE", "Germany"),
        V("FR", "France"),        V("ES", "Spain"),
        V("IT", "Italy"),         V("NL", "Netherlands"),
        V("BR", "Brazil"),        V("IN", "India"),
        V("JP", "Japan"),
    }};
}

std::vector<WebSearchService> make_catalogue() {
    std::vector<WebSearchService> c;

    // ── Free: no account, card or key ──────────────────────────────────
    {
        WebSearchService s;
        s.id = "brave-free"; s.label = "Brave Free";
        s.note = "Scrapes Brave's public results page. Allows a short burst "
                 "(about 8-11 quick searches), then answers 429 for a while. "
                 "Brave's robots.txt disallows /search: best effort, unsanctioned.";
        s.max_count = 20;
        s.dials.push_back(freshness_dial("freshness",
            "Only pages published within this window (Brave's tf parameter).",
            dwmy("pd", "pw", "pm", "py")));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "ddg-free"; s.label = "DuckDuckGo Free";
        s.note = "Scrapes DuckDuckGo's HTML page. Many networks are served a "
                 "'select all squares' challenge instead of results; it works "
                 "for some users and never for others.";
        s.max_count = 30;
        s.dials.push_back(WebSearchDial{"region", "Region",
            "DuckDuckGo's kl parameter. Skews which country's results rank first.", {
            V("", "United States (default)"), V("wt-wt", "No region"),
            V("uk-en", "United Kingdom"), V("au-en", "Australia"),
            V("nz-en", "New Zealand"),   V("ca-en", "Canada"),
            V("in-en", "India"),         V("de-de", "Germany"),
            V("fr-fr", "France"),        V("es-es", "Spain"),
            V("it-it", "Italy"),         V("br-pt", "Brazil"),
            V("jp-jp", "Japan"),
        }});
        s.dials.push_back(WebSearchDial{"safe", "Safe search",
            "DuckDuckGo's kp parameter.", {
            V("", "Moderate (default)"), V("1", "Strict"), V("-2", "Off"),
        }});
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "tavily-free"; s.label = "Tavily Free";
        s.note = "Tavily's documented key-less mode. JSON, not a scrape. "
                 "The per-IP cap is not published; a 429 means it was reached.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(WebSearchDial{"topic", "Topic",
            "News and finance restrict results to those kinds of source.", {
            V("", "General"), V("news", "News"), V("finance", "Finance")}});
        s.dials.push_back(freshness_dial("time_range",
            "Only pages from this window (Tavily's time_range).",
            dwmy("day", "week", "month", "year")));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "firecrawl-free"; s.label = "Firecrawl Free";
        s.note = "Firecrawl's key-less search. Capped per IP per day by request "
                 "and credit count; the numbers are not published.";
        s.max_count = 100;
        s.dials.push_back(freshness_dial("freshness",
            "Only pages from this window (Firecrawl's tbs=qdr:).",
            dwmy("qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(WebSearchDial{"category", "Category",
            "Restrict to one kind of site.", {
            V("", "Any"), V("github", "GitHub"), V("research", "Research"),
            V("pdf", "PDF"), V("developer", "Developer docs")}});
        s.dials.push_back(country_dial("Firecrawl's country parameter."));
        c.push_back(std::move(s));
    }

    // ── API key: stored by the host, never in a URL ────────────────────
    {
        WebSearchService s;
        s.id = "brave-api"; s.label = "Brave API key"; s.needs_key = true;
        s.signup = "https://api-dashboard.search.brave.com";
        s.note = "Brave's official Web Search API. One request per second per plan.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(freshness_dial("freshness",
            "Only pages published within this window.",
            dwmy("pd", "pw", "pm", "py")));
        s.dials.push_back(WebSearchDial{"safesearch", "Safe search", "", {
            V("", "Moderate (default)"), V("off", "Off"), V("strict", "Strict")}});
        s.dials.push_back(country_dial("Where results come from."));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "tavily-api"; s.label = "Tavily API key"; s.needs_key = true;
        s.signup = "https://app.tavily.com";
        s.note = "Tavily's search API. 'Advanced' costs 2 credits a search.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(WebSearchDial{"depth", "Depth",
            "Basic is 1 credit; advanced is 2 and reads more of each page.", {
            V("", "Basic (default)"), V("advanced", "Advanced"),
            V("fast", "Fast"), V("ultra-fast", "Ultra fast")}});
        s.dials.push_back(WebSearchDial{"topic", "Topic", "", {
            V("", "General"), V("news", "News"), V("finance", "Finance")}});
        s.dials.push_back(freshness_dial("time_range",
            "Only pages from this window.", dwmy("day", "week", "month", "year")));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "firecrawl-api"; s.label = "Firecrawl API key"; s.needs_key = true;
        s.signup = "https://www.firecrawl.dev";
        s.note = "Firecrawl's search API. 2 credits a search.";
        s.max_count = 100;
        s.dials.push_back(freshness_dial("freshness",
            "Only pages from this window.", dwmy("qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(WebSearchDial{"category", "Category", "", {
            V("", "Any"), V("github", "GitHub"), V("research", "Research"),
            V("pdf", "PDF"), V("developer", "Developer docs")}});
        s.dials.push_back(country_dial("Firecrawl's country parameter."));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "exa-api"; s.label = "Exa API key"; s.needs_key = true;
        s.signup = "https://dashboard.exa.ai/api-keys";
        s.note = "Exa's neural search. Highlights are returned as the snippet.";
        s.max_count = 100;
        s.dials.push_back(WebSearchDial{"type", "Mode",
            "Fast and instant trade depth for speed.", {
            V("", "Auto (default)"), V("fast", "Fast"), V("instant", "Instant"),
            V("deep", "Deep")}});
        s.dials.push_back(WebSearchDial{"category", "Category",
            "Restrict to one kind of source.", {
            V("", "Any"), V("news", "News"), V("publication", "Publications"),
            V("personal site", "Personal sites")}});
        s.dials.push_back(freshness_dial("freshness",
            "Only pages published within this window (sent as startPublishedDate).",
            dwmy("day", "week", "month", "year")));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "serper-api"; s.label = "Serper API key"; s.needs_key = true;
        s.signup = "https://serper.dev";
        s.note = "Google results as JSON.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(country_dial("Google's gl parameter."));
        s.dials.push_back(freshness_dial("freshness",
            "Google's tbs=qdr: time filter.", dwmy("qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(WebSearchDial{"autocorrect", "Spell correction", "", {
            V("", "On (default)"), V("false", "Off")}});
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "perplexity-api"; s.label = "Perplexity API key"; s.needs_key = true;
        s.signup = "https://docs.perplexity.ai";
        s.note = "Perplexity's Search API (ranked results, not an answer). "
                 "'Never return' sites are left in the query as -site: here, "
                 "because its domain-filter syntax for excluding is not documented.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(WebSearchDial{"recency", "Freshness",
            "Only pages from this window (search_recency_filter).", {
            V("", "Any time"), V("hour", "Past hour"), V("day", "Past day"),
            V("week", "Past week"), V("month", "Past month"), V("year", "Past year")}});
        s.dials.push_back(country_dial("Perplexity's country parameter."));
        c.push_back(std::move(s));
    }
    return c;
}

// ── Small string helpers ───────────────────────────────────────────────

std::string lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    return contains(lower(std::string{hay}), lower(std::string{needle}));
}

// A header/body-safe single line, bounded.
std::string tidy(std::string s, std::size_t max_bytes) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == ' ' || u == '\t' || u == '\n' || u == '\r') { space = true; continue; }
        if (u < 0x20) continue;
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(ch);
    }
    if (out.size() > max_bytes) {
        out.resize(util::safe_utf8_cut(out, max_bytes));
        out += "...";
    }
    return out;
}

std::string str_at(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

std::string join_strings(const json& arr, std::string_view sep) {
    std::string out;
    if (!arr.is_array()) return out;
    for (const auto& x : arr) {
        if (!x.is_string()) continue;
        if (!out.empty()) out += sep;
        out += x.get<std::string>();
    }
    return out;
}

// ISO 8601 UTC, `days` before `now`.
std::string iso_days_ago(std::chrono::system_clock::time_point now, int days) {
    const std::time_t t = std::chrono::system_clock::to_time_t(
        now - std::chrono::hours(24 * days));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

// Brave's search_lang is not plain ISO 639-1 for a few languages.
// Source: the web/search parameter table. Anything not listed is not sent.
std::string brave_lang(const std::string& l) {
    if (l == "zh") return "zh-hans";
    if (l == "pt") return "pt-pt";
    if (l == "no") return "nb";
    static const char* ok[] = {
        "ar","eu","bn","bg","ca","hr","cs","da","nl","en","et","fi","fr","gl",
        "de","el","gu","he","hi","hu","is","it","ja","kn","ko","lv","lt","ms",
        "ml","mr","pl","pa","ro","ru","sr","sk","sl","es","sv","ta","te","th",
        "tr","uk","vi"};
    for (const char* x : ok) if (l == x) return l;
    return {};
}

HttpRequest json_post(std::string url, const json& body,
                      std::vector<std::pair<std::string, std::string>> extra) {
    HttpRequest r;
    r.method = "POST";
    r.url = std::move(url);
    r.headers = std::move(extra);
    r.headers.push_back({"content-type", "application/json"});
    r.headers.push_back({"accept", "application/json"});
    r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    return r;
}

constexpr const char* kBrowserUa =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

std::vector<std::pair<std::string, std::string>> browser_headers(
    const std::string& referer_host, bool post) {
    std::vector<std::pair<std::string, std::string>> h;
    h.push_back({"user-agent", kBrowserUa});
    h.push_back({"accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
    h.push_back({"accept-language", "en-US,en;q=0.9"});
    if (post) h.push_back({"content-type", "application/x-www-form-urlencoded"});
    h.push_back({"referer", "https://" + referer_host + "/"});
    return h;
}

int days_for(std::string_view v) {
    if (v == "day")   return 1;
    if (v == "week")  return 7;
    if (v == "month") return 30;
    if (v == "year")  return 365;
    return 0;
}

// ── Request builders ───────────────────────────────────────────────────
//
// Each cites where its shape comes from. [L] = confirmed against a live reply
// in the research notes; [D] = the vendor's published reference.

// Brave Free. [L] GET /search?q=&source=web ; tf=pd|pw|pm|py narrows by age.
HttpRequest req_brave_free(const WebSearchQuery& q) {
    HttpRequest r;
    r.method = "GET";
    r.url = "https://search.brave.com/search?q=" + url_escape(q.text)
          + "&source=web";
    if (auto f = q.dial("freshness"); !f.empty())
        r.url += "&tf=" + url_escape(f);
    r.headers = browser_headers("search.brave.com", false);
    return r;
}

// DuckDuckGo Free. [L] POST /html/ form q=&kl= ; kp=1 strict, -2 off. [D]
HttpRequest req_ddg_free(const WebSearchQuery& q) {
    HttpRequest r;
    r.method = "POST";
    r.url = "https://html.duckduckgo.com/html/";
    std::string_view region = q.dial("region");
    r.body = "q=" + url_escape(q.text) + "&kl="
           + (region.empty() ? std::string{"us-en"} : std::string{region});
    if (auto s = q.dial("safe"); !s.empty())
        r.body += "&kp=" + url_escape(s);
    r.headers = browser_headers("html.duckduckgo.com", true);
    return r;
}

// Tavily, keyless or keyed. [D] POST /search ; [L] keyless = the header
// X-Tavily-Access-Mode: keyless, no Authorization.
HttpRequest req_tavily(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"max_results", q.count},
              {"include_answer", false}, {"include_raw_content", false}};
    if (!q.exclude.empty()) b["exclude_domains"] = q.exclude;
    if (!q.language.empty()) b["language"] = q.language;
    if (auto v = q.dial("topic"); !v.empty())      b["topic"] = std::string{v};
    if (auto v = q.dial("time_range"); !v.empty()) b["time_range"] = std::string{v};
    if (auto v = q.dial("depth"); !v.empty())      b["search_depth"] = std::string{v};
    std::vector<std::pair<std::string, std::string>> h;
    if (key.empty()) h.push_back({"x-tavily-access-mode", "keyless"});
    else             h.push_back({"authorization", "Bearer " + std::string{key}});
    return json_post("https://api.tavily.com/search", b, std::move(h));
}

// Firecrawl, keyless or keyed. [D]+[L] POST /v2/search ; limit 1-100 ;
// tbs=qdr:d|w|m|y ; categories is a list of strings ; excludeDomains takes
// bare hostnames. Keyless = no Authorization header at all.
HttpRequest req_firecrawl(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"limit", q.count}};
    if (!q.exclude.empty()) b["excludeDomains"] = q.exclude;
    if (auto v = q.dial("freshness"); !v.empty()) b["tbs"] = std::string{v};
    if (auto v = q.dial("category"); !v.empty())  b["categories"] = json::array({std::string{v}});
    if (auto v = q.dial("country"); !v.empty())   b["country"] = std::string{v};
    std::vector<std::pair<std::string, std::string>> h;
    if (!key.empty()) h.push_back({"authorization", "Bearer " + std::string{key}});
    return json_post("https://api.firecrawl.dev/v2/search", b, std::move(h));
}

// Brave API. [D] GET /res/v1/web/search ; header X-Subscription-Token ;
// count 1-20 ; text_decorations=false drops the <strong> highlight markers.
HttpRequest req_brave_api(const WebSearchQuery& q, std::string_view key) {
    HttpRequest r;
    r.method = "GET";
    r.url = "https://api.search.brave.com/res/v1/web/search?q="
          + url_escape(q.text) + "&count=" + std::to_string(q.count)
          + "&text_decorations=false";
    if (auto l = brave_lang(q.language); !l.empty())
        r.url += "&search_lang=" + l;
    if (auto v = q.dial("country"); !v.empty())
        r.url += "&country=" + url_escape(v);
    if (auto v = q.dial("freshness"); !v.empty())
        r.url += "&freshness=" + url_escape(v);
    if (auto v = q.dial("safesearch"); !v.empty())
        r.url += "&safesearch=" + url_escape(v);
    r.headers.push_back({"accept", "application/json"});
    r.headers.push_back({"x-subscription-token", std::string{key}});
    return r;
}

// Exa. [D] POST /search ; x-api-key ; numResults 1-100 ; contents.highlights
// puts excerpts on each result ; startPublishedDate is an ISO 8601 instant.
HttpRequest req_exa(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"numResults", q.count},
              {"contents", {{"highlights", true}}}};
    if (!q.exclude.empty()) b["excludeDomains"] = q.exclude;
    if (auto v = q.dial("type"); !v.empty())     b["type"] = std::string{v};
    if (auto v = q.dial("category"); !v.empty()) b["category"] = std::string{v};
    if (int d = days_for(q.dial("freshness")); d > 0)
        b["startPublishedDate"] = iso_days_ago(q.now, d);
    return json_post("https://api.exa.ai/search", b,
                     {{"x-api-key", std::string{key}}});
}

// Serper. [D] POST /search ; X-API-KEY ; q, num, gl, hl, tbs, autocorrect.
HttpRequest req_serper(const WebSearchQuery& q, std::string_view key) {
    json b = {{"q", q.text}, {"num", q.count}};
    if (!q.language.empty()) b["hl"] = q.language;
    if (auto v = q.dial("country"); !v.empty())     b["gl"] = lower(std::string{v});
    if (auto v = q.dial("freshness"); !v.empty())   b["tbs"] = std::string{v};
    if (q.dial("autocorrect") == "false")          b["autocorrect"] = false;
    return json_post("https://google.serper.dev/search", b,
                     {{"x-api-key", std::string{key}}});
}

// Perplexity Search. [D] POST /search ; Bearer ; max_results 1-20 without a
// tier ; country is ISO alpha-2 ; search_language_filter is a list of ISO 639-1
// codes ; search_recency_filter is hour|day|week|month|year.
HttpRequest req_perplexity(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"max_results", q.count}};
    if (!q.language.empty()) b["search_language_filter"] = json::array({q.language});
    if (auto v = q.dial("country"); !v.empty())  b["country"] = std::string{v};
    if (auto v = q.dial("recency"); !v.empty())  b["search_recency_filter"] = std::string{v};
    return json_post("https://api.perplexity.ai/search", b,
                     {{"authorization", "Bearer " + std::string{key}}});
}

std::vector<SearchHit> collect(const json& arr, const char* url_key,
                               const char* title_key,
                               const std::vector<const char*>& snippet_keys,
                               int count) {
    std::vector<SearchHit> out;
    if (!arr.is_array()) return out;
    for (const auto& r : arr) {
        if (static_cast<int>(out.size()) >= count) break;
        if (!r.is_object()) continue;
        SearchHit h;
        h.url = tidy(str_at(r, url_key), 2048);
        h.title = tidy(str_at(r, title_key), 300);
        for (const char* k : snippet_keys) {
            auto it = r.find(k);
            if (it == r.end()) continue;
            std::string s = it->is_string() ? it->get<std::string>()
                          : it->is_array()  ? join_strings(*it, " ... ")
                                            : std::string{};
            if (!s.empty()) { h.snippet = tidy(std::move(s), 500); break; }
        }
        if (h.url.empty()) continue;
        if (h.title.empty()) h.title = h.url;
        out.push_back(std::move(h));
    }
    return out;
}

} // namespace

std::string_view WebSearchQuery::dial(std::string_view id) const noexcept {
    for (const auto& [k, v] : dials)
        if (k == id) return v;
    return {};
}

std::string url_escape(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

SiteSplit split_site_exclusions(std::string_view query) {
    SiteSplit out;
    std::string kept;
    std::size_t i = 0;
    auto valid_host = [](std::string_view h) {
        if (h.empty() || h.size() > 253 || h.find('.') == std::string_view::npos)
            return false;
        for (char c : h) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '.' || c == '-')) return false;
        }
        return h.front() != '.' && h.back() != '.' && h.front() != '-';
    };
    while (i < query.size()) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        const std::size_t b = i;
        while (i < query.size() && !std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        if (i == b) break;
        std::string_view tok = query.substr(b, i - b);
        if (tok.size() > 6 && contains_ci(tok.substr(0, 6), "-site:")) {
            std::string host = lower(std::string{tok.substr(6)});
            if (valid_host(host) && out.exclude.size() < 20) {
                if (std::find(out.exclude.begin(), out.exclude.end(), host)
                    == out.exclude.end())
                    out.exclude.push_back(std::move(host));
                continue;
            }
        }
        if (!kept.empty()) kept.push_back(' ');
        kept.append(tok);
    }
    if (kept.empty()) {            // nothing but exclusions: send it as it was
        out.exclude.clear();
        out.query = std::string{query};
    } else {
        out.query = std::move(kept);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
validate_dials(const WebSearchService& svc, const WebSearchSlot& slot) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& d : svc.dials) {
        for (const auto& [id, val] : slot.dials) {
            if (id != d.id || val.empty()) continue;
            const bool known = std::any_of(d.values.begin(), d.values.end(),
                [&](const WebSearchDialValue& v) { return v.id == val; });
            if (known) out.emplace_back(id, val);
            break;
        }
    }
    return out;
}

bool is_html_service(std::string_view id) noexcept {
    return id == "brave-free" || id == "ddg-free";
}

bool has_native_exclude(std::string_view id) noexcept {
    return id == "tavily-free" || id == "tavily-api"
        || id == "firecrawl-free" || id == "firecrawl-api"
        || id == "exa-api";
}

std::string clean_api_key(std::string_view key) {
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front())))
        key.remove_prefix(1);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
        key.remove_suffix(1);
    for (char c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return {};   // control char or inner space
    }
    return std::string{key};
}

HttpRequest build_web_search_request(const WebSearchService& svc,
                                std::string_view key, const WebSearchQuery& q_in) {
    WebSearchQuery q = q_in;
    q.text = util::to_valid_utf8(std::move(q.text));
    const std::string& id = svc.id;
    if (id == "brave-free")     return req_brave_free(q);
    if (id == "ddg-free")       return req_ddg_free(q);
    if (id == "tavily-free")    return req_tavily(q, {});
    if (id == "tavily-api")     return req_tavily(q, key);
    if (id == "firecrawl-free") return req_firecrawl(q, {});
    if (id == "firecrawl-api")  return req_firecrawl(q, key);
    if (id == "brave-api")      return req_brave_api(q, key);
    if (id == "exa-api")        return req_exa(q, key);
    if (id == "serper-api")     return req_serper(q, key);
    if (id == "perplexity-api") return req_perplexity(q, key);
    return {};   // unreachable for a catalogue id; empty url fails the send
}

WebSearchFailure classify_http(const WebSearchService& svc, int status,
                            std::string_view body) {
    // 2xx that is not results: a challenge page served with 200/202.
    if (status >= 200 && status < 300) {
        if (status == 202 || contains_ci(body, "captcha")
            || contains_ci(body, "select all squares")
            || contains_ci(body, "anomaly-modal"))
            return WebSearchFailure::Blocked;
        return WebSearchFailure::Unreadable;
    }
    // Brave's API answers a bad token with 422 + SUBSCRIPTION_TOKEN_INVALID. [L]
    if (status == 422 && contains(body, "SUBSCRIPTION_TOKEN_INVALID"))
        return WebSearchFailure::BadKey;
    // Tavily's own plan/PAYG limit codes. [D]
    if (status == 432 || status == 433) return WebSearchFailure::QuotaExhausted;
    if (status == 402) return WebSearchFailure::QuotaExhausted;
    if (status == 429) return WebSearchFailure::RateLimited;
    if (status == 401 || status == 403) {
        // A key-less service refusing us is access being withdrawn, not a
        // key problem the user can fix.
        return svc.needs_key ? WebSearchFailure::BadKey : WebSearchFailure::Blocked;
    }
    if (status == 408 || status >= 500) return WebSearchFailure::Transient;
    return WebSearchFailure::BadRequest;
}

std::string describe_failure(WebSearchFailure f, int http_status) {
    const std::string http = http_status > 0
        ? " (HTTP " + std::to_string(http_status) + ")" : std::string{};
    switch (f) {
        case WebSearchFailure::Blocked:
            return "blocked: a captcha or an access refusal for this network" + http;
        case WebSearchFailure::RateLimited:
            return "rate limited, try again later" + http;
        case WebSearchFailure::QuotaExhausted:
            return "quota or credits used up" + http;
        case WebSearchFailure::BadKey:
            return "API key missing, invalid or not permitted" + http;
        case WebSearchFailure::BadRequest:
            return "the service rejected the request" + http;
        case WebSearchFailure::Transient:
            return "service error, try again" + http;
        case WebSearchFailure::Empty:
            return "no results";
        case WebSearchFailure::Unreadable:
            return "reply not in the documented shape" + http;
    }
    return "failed";
}

JsonHits parse_json_hits(std::string_view service_id, std::string_view body,
                         int count) {
    JsonHits out;
    json j = json::parse(body.begin(), body.end(), nullptr, /*exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { out.ok = false; return out; }
    const json missing = nullptr;   // not an array, so need() flags it
    auto field = [&](const json& o, const char* k) -> const json& {
        if (!o.is_object()) return missing;
        auto it = o.find(k);
        return it == o.end() ? missing : *it;
    };
    // The container must exist and be an array, or the shape is not the one
    // documented. (An absent container with success:false is also unreadable.)
    auto need = [&](const json& arr) {
        if (!arr.is_array()) out.ok = false;
        return &arr;
    };

    if (service_id == "tavily-free" || service_id == "tavily-api") {
        out.hits = collect(*need(field(j, "results")), "url", "title",
                           {"content"}, count);
    } else if (service_id == "firecrawl-free" || service_id == "firecrawl-api") {
        out.hits = collect(*need(field(field(j, "data"), "web")), "url", "title",
                           {"description"}, count);
    } else if (service_id == "exa-api") {
        out.hits = collect(*need(field(j, "results")), "url", "title",
                           {"highlights", "summary", "text"}, count);
    } else if (service_id == "serper-api") {
        // Serper omits `organic` when Google found nothing.
        const json& o = field(j, "organic");
        out.hits = collect(o, "link", "title", {"snippet"}, count);
    } else if (service_id == "perplexity-api") {
        out.hits = collect(*need(field(j, "results")), "url", "title",
                           {"snippet"}, count);
    } else if (service_id == "brave-api") {
        // `web` is absent when there are no web results.
        out.hits = collect(field(field(j, "web"), "results"), "url", "title",
                           {"description"}, count);
    } else {
        out.ok = false;
    }
    return out;
}

} // namespace mcp::tools::detail

namespace mcp::tools {

const std::vector<WebSearchService>& web_search_services() {
    static const std::vector<WebSearchService> c = detail::make_catalogue();
    return c;
}

const WebSearchService* find_web_search_service(std::string_view id) {
    for (const auto& s : web_search_services())
        if (s.id == id) return &s;
    return nullptr;
}

WebSearchPlan default_web_search_plan() {
    WebSearchPlan p;
    p.slots = {WebSearchSlot{"brave-free", {}, {}},
               WebSearchSlot{"ddg-free", {}, {}},
               WebSearchSlot{"tavily-free", {}, {}}};
    return p;
}

} // namespace mcp::tools
