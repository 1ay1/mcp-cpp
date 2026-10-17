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

// The same, led by Past hour, for the services that filter down to the hour.
std::vector<WebSearchDialValue> hdwmy(std::string h, std::string d, std::string w,
                                   std::string m, std::string y) {
    auto v = dwmy(std::move(d), std::move(w), std::move(m), std::move(y));
    v.insert(v.begin(), V(std::move(h), "Past hour"));
    return v;
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

// Firecrawl's documented categories, the same for both tiers. Only one is
// sent, which also keeps `gov` alone as Firecrawl requires.
WebSearchDial firecrawl_category_dial() {
    return WebSearchDial{"category", "Category",
        "Restrict to one kind of source. Developer: code repositories and "
        "documentation sites. Government: laws, regulations and court opinions.", {
        V("", "Any"), V("developer", "Developer"), V("research", "Research"),
        V("pdf", "PDF"), V("gov", "Government")}};
}

// Firecrawl folds date sorting into tbs: sbd:1 alone, or joined to the
// freshness window as "sbd:1,qdr:w" (its documented combined form).
WebSearchDial firecrawl_sort_dial() {
    return WebSearchDial{"sort", "Sort",
        "Newest first sorts by date, inside the Freshness window if one is set.", {
        V("", "Relevance (default)"), V("date", "Newest first")}};
}

// Firecrawl's location is free text; its reference pairs it with country.
WebSearchDial firecrawl_location_dial() {
    WebSearchDial d{"location", "Location",
        "A place to search from, e.g. San Francisco,California,United States. "
        "Works best with Country set too.", {}};
    d.text = WebSearchText::Line;
    d.max_bytes = 200;
    return d;
}

WebSearchDial on_off_dial(std::string id, std::string label, std::string help) {
    return WebSearchDial{std::move(id), std::move(label), std::move(help), {
        V("", "Off (default)"), V("true", "On")}};
}

WebSearchDial tavily_depth_dial(std::string help) {
    return WebSearchDial{"depth", "Depth", std::move(help), {
        V("", "Basic (default)"), V("advanced", "Advanced"),
        V("fast", "Fast"), V("ultra-fast", "Ultra fast")}};
}

// The dials both Tavily tiers share after topic, freshness and depth. The
// key-less mode documents "all parameters", so the free tier gets them too.
void add_tavily_filters(WebSearchService& s) {
    s.dials.push_back(on_off_dial("exact_match", "Exact phrases",
        "Only results that contain the query's \"quoted phrases\" as written, "
        "with no synonyms."));
    s.dials.push_back(WebSearchDial{"language_filter", "Language match",
        "Uses the web search language. Boost ranks it first; only drops every "
        "other language. Not sent when no language is set.", {
        V("", "Boost (default)"), V("only", "Only this language")}});
    s.dials.push_back(country_dial("Boosts results from this country. Sent only "
        "with the General topic, the only one Tavily allows it with."));
    s.dials.push_back(on_off_dial("safe_search", "Safe search",
        "Filters adult content. The fast and ultra-fast depths do not support "
        "it, so it is not sent with them."));
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
            // Every region on DuckDuckGo's URL-parameters help page, in its
            // order. us-en is the default, so it is the "" row, not a second one.
            V("", "United States (default)"), V("wt-wt", "No region"),
            V("xa-ar", "Arabia"),            V("xa-en", "Arabia (en)"),
            V("ar-es", "Argentina"),         V("au-en", "Australia"),
            V("at-de", "Austria"),           V("be-fr", "Belgium (fr)"),
            V("be-nl", "Belgium (nl)"),      V("br-pt", "Brazil"),
            V("bg-bg", "Bulgaria"),          V("ca-en", "Canada"),
            V("ca-fr", "Canada (fr)"),       V("ct-ca", "Catalan"),
            V("cl-es", "Chile"),             V("cn-zh", "China"),
            V("co-es", "Colombia"),          V("hr-hr", "Croatia"),
            V("cz-cs", "Czech Republic"),    V("dk-da", "Denmark"),
            V("ee-et", "Estonia"),           V("fi-fi", "Finland"),
            V("fr-fr", "France"),            V("de-de", "Germany"),
            V("gr-el", "Greece"),            V("hk-tzh", "Hong Kong"),
            V("hu-hu", "Hungary"),           V("in-en", "India"),
            V("id-id", "Indonesia"),         V("id-en", "Indonesia (en)"),
            V("ie-en", "Ireland"),           V("il-he", "Israel"),
            V("it-it", "Italy"),             V("jp-jp", "Japan"),
            V("kr-kr", "Korea"),             V("lv-lv", "Latvia"),
            V("lt-lt", "Lithuania"),         V("xl-es", "Latin America"),
            V("my-ms", "Malaysia"),          V("my-en", "Malaysia (en)"),
            V("mx-es", "Mexico"),            V("nl-nl", "Netherlands"),
            V("nz-en", "New Zealand"),       V("no-no", "Norway"),
            V("pe-es", "Peru"),              V("ph-en", "Philippines"),
            V("ph-tl", "Philippines (tl)"),  V("pl-pl", "Poland"),
            V("pt-pt", "Portugal"),          V("ro-ro", "Romania"),
            V("ru-ru", "Russia"),            V("sg-en", "Singapore"),
            V("sk-sk", "Slovak Republic"),   V("sl-sl", "Slovenia"),
            V("za-en", "South Africa"),      V("es-es", "Spain"),
            V("se-sv", "Sweden"),            V("ch-de", "Switzerland (de)"),
            V("ch-fr", "Switzerland (fr)"),  V("ch-it", "Switzerland (it)"),
            V("tw-tzh", "Taiwan"),           V("th-th", "Thailand"),
            V("tr-tr", "Turkey"),            V("ua-uk", "Ukraine"),
            V("uk-en", "United Kingdom"),    V("ue-es", "United States (es)"),
            V("ve-es", "Venezuela"),         V("vn-vi", "Vietnam"),
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
        s.dials.push_back(tavily_depth_dial(
            "Advanced reads more of each page and spends 2 of the key-less "
            "budget's credits; fast and ultra-fast trade relevance for speed."));
        add_tavily_filters(s);
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
            hdwmy("qdr:h", "qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(firecrawl_category_dial());
        s.dials.push_back(country_dial("Firecrawl's country parameter."));
        s.dials.push_back(on_off_dial("safe", "Safe search",
            "Filters explicit content (Firecrawl's safe)."));
        s.dials.push_back(firecrawl_sort_dial());
        s.dials.push_back(firecrawl_location_dial());
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
        // Brave's default is US; ALL is its documented "no country" value.
        auto country = country_dial("Where results come from.");
        country.values.insert(country.values.begin() + 1, V("ALL", "All regions"));
        s.dials.push_back(std::move(country));
        s.dials.push_back(WebSearchDial{"spellcheck", "Spell correction",
            "When on, Brave searches its corrected query instead of the one "
            "sent, which can change identifiers and error strings.", {
            V("", "On (default)"), V("false", "Off")}});
        // One hosted Goggle; Brave advises hosted files over inline rules.
        WebSearchDial goggle{"goggles", "Goggle",
            "Re-ranks results by a Goggle's rules: the https URL of a Goggle "
            "file on GitHub, GitLab or a Gist, registered at "
            "search.brave.com/goggles/create.", {}};
        goggle.text = WebSearchText::HttpsUrl;
        goggle.max_bytes = 2048;
        s.dials.push_back(std::move(goggle));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "tavily-api"; s.label = "Tavily API key"; s.needs_key = true;
        s.signup = "https://app.tavily.com";
        s.note = "Tavily's search API. 'Advanced' costs 2 credits a search.";
        s.max_count = 20; s.language = true;
        s.dials.push_back(tavily_depth_dial(
            "Basic is 1 credit; advanced is 2 and reads more of each page."));
        s.dials.push_back(WebSearchDial{"topic", "Topic", "", {
            V("", "General"), V("news", "News"), V("finance", "Finance")}});
        s.dials.push_back(freshness_dial("time_range",
            "Only pages from this window.", dwmy("day", "week", "month", "year")));
        add_tavily_filters(s);
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "firecrawl-api"; s.label = "Firecrawl API key"; s.needs_key = true;
        s.signup = "https://www.firecrawl.dev";
        s.note = "Firecrawl's search API. 2 credits a search.";
        s.max_count = 100;
        s.dials.push_back(freshness_dial("freshness",
            "Only pages from this window.",
            hdwmy("qdr:h", "qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(firecrawl_category_dial());
        s.dials.push_back(country_dial("Firecrawl's country parameter."));
        s.dials.push_back(on_off_dial("safe", "Safe search",
            "Filters explicit content (Firecrawl's safe)."));
        s.dials.push_back(firecrawl_sort_dial());
        s.dials.push_back(firecrawl_location_dial());
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "exa-api"; s.label = "Exa API key"; s.needs_key = true;
        s.signup = "https://dashboard.exa.ai/api-keys";
        s.note = "Exa's neural search. Highlights are returned as the snippet.";
        s.max_count = 100;
        s.dials.push_back(WebSearchDial{"type", "Mode",
            "Fast and instant trade depth for speed. Deep lite, deep and deep "
            "reasoning research and synthesise: about 4, 4-15 and 12-40 seconds, "
            "so deep reasoning can outlast a host's request timeout.", {
            V("", "Auto (default)"), V("fast", "Fast"), V("instant", "Instant"),
            V("deep-lite", "Deep lite"), V("deep", "Deep"),
            V("deep-reasoning", "Deep reasoning")}});
        s.dials.push_back(WebSearchDial{"category", "Category",
            "Restrict to one kind of source.", {
            V("", "Any"), V("news", "News"), V("publication", "Publications"),
            V("personal site", "Personal sites")}});
        s.dials.push_back(freshness_dial("freshness",
            "Only pages published within this window (sent as startPublishedDate).",
            hdwmy("hour", "day", "week", "month", "year")));
        s.dials.push_back(on_off_dial("moderation", "Safe search",
            "Filters unsafe content (Exa's moderation)."));
        s.dials.push_back(country_dial(
            "The searcher's country (Exa's userLocation); localises results."));
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
            "Google's tbs=qdr: time filter.",
            hdwmy("qdr:h", "qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        s.dials.push_back(WebSearchDial{"autocorrect", "Spell correction", "", {
            V("", "On (default)"), V("false", "Off")}});
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "serply-api"; s.label = "Serply API key"; s.needs_key = true;
        s.signup = "https://serply.io";
        s.note = "Google results as JSON.";
        s.max_count = 10;
        // Without gl Serply serves from whichever proxy region is free, so the
        // results can change country between calls. req_serply sends gl=us
        // for the default, so United States takes the first row.
        auto country = country_dial("Google's gl parameter.");
        country.values.erase(country.values.begin() + 1);
        country.values[0].label = "United States (default)";
        s.dials.push_back(std::move(country));
        s.dials.push_back(freshness_dial("freshness",
            "Google's tbs=qdr: time filter.",
            hdwmy("qdr:h", "qdr:d", "qdr:w", "qdr:m", "qdr:y")));
        c.push_back(std::move(s));
    }
    {
        WebSearchService s;
        s.id = "perplexity-api"; s.label = "Perplexity API key"; s.needs_key = true;
        s.signup = "https://docs.perplexity.ai";
        s.note = "Perplexity's Search API (ranked results, not an answer).";
        s.max_count = 20; s.language = true;
        s.dials.push_back(WebSearchDial{"recency", "Freshness",
            "Only pages from this window (search_recency_filter).", {
            V("", "Any time"), V("hour", "Past hour"), V("day", "Past day"),
            V("week", "Past week"), V("month", "Past month"), V("year", "Past year")}});
        s.dials.push_back(country_dial("Perplexity's country parameter."));
        s.dials.push_back(WebSearchDial{"search_type", "Mode",
            "Fast answers sooner with a lighter web search.", {
            V("", "Web (default)"), V("fast", "Fast")}});
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

// Plain text from the markdown some services put in a snippet (Firecrawl's
// `description` is page markdown: "## [Why Bother?](https://...#why)\nIn the
// first article...", seen live). The scrapers always produced plain text,
// and a link's URL inside a snippet is noise next to the result's own URL.
// Deliberately small: heading markers at a line start, [text](url) -> text,
// and emphasis markers. Anything else passes through unchanged.
std::string flatten_markdown(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool line_start = true;
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (line_start) {
            std::size_t j = i;
            while (j < s.size() && s[j] == '#') ++j;
            if (j > i && j < s.size() && s[j] == ' ') { i = j + 1; line_start = false; continue; }
        }
        line_start = (c == '\n');
        if (c == '[') {
            const auto close = s.find(']', i + 1);
            if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
                const auto paren = s.find(')', close + 2);
                if (paren != std::string_view::npos) {
                    out.append(s.substr(i + 1, close - i - 1));
                    i = paren + 1;
                    continue;
                }
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) { i += 2; continue; }
        out.push_back(c);
        ++i;
    }
    return out;
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

// ISO 8601 UTC, `hours` before `now`.
std::string iso_hours_ago(std::chrono::system_clock::time_point now, int hours) {
    const std::time_t t = std::chrono::system_clock::to_time_t(
        now - std::chrono::hours(hours));
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

int hours_for(std::string_view v) {
    if (v == "hour")  return 1;
    if (v == "day")   return 24;
    if (v == "week")  return 24 * 7;
    if (v == "month") return 24 * 30;
    if (v == "year")  return 24 * 365;
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
           + url_escape(region.empty() ? std::string_view{"us-en"} : region);
    if (auto s = q.dial("safe"); !s.empty())
        r.body += "&kp=" + url_escape(s);
    r.headers = browser_headers("html.duckduckgo.com", true);
    return r;
}

// Tavily's country takes a lower-case English name from its own list, not
// an ISO code. Covers country_dial's codes; anything else is not sent.
std::string tavily_country(std::string_view code) {
    static constexpr std::pair<std::string_view, std::string_view> names[] = {
        {"US", "united states"}, {"GB", "united kingdom"}, {"NZ", "new zealand"},
        {"AU", "australia"},     {"CA", "canada"},         {"DE", "germany"},
        {"FR", "france"},        {"ES", "spain"},          {"IT", "italy"},
        {"NL", "netherlands"},   {"BR", "brazil"},         {"IN", "india"},
        {"JP", "japan"}};
    for (const auto& [c, n] : names)
        if (c == code) return std::string{n};
    return {};
}

// Tavily, keyless or keyed. [D] POST /search ; [L] keyless = the header
// X-Tavily-Access-Mode: keyless, no Authorization. Three dials depend on
// others, per the reference: country only with topic general, safe_search
// not with the fast depths, filter_by_language only with a language (else
// a 400).
HttpRequest req_tavily(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"max_results", q.count},
              {"include_answer", false}, {"include_raw_content", false}};
    if (!q.exclude.empty()) b["exclude_domains"] = q.exclude;
    if (!q.language.empty()) b["language"] = q.language;
    const std::string_view topic = q.dial("topic");
    const std::string_view depth = q.dial("depth");
    if (!topic.empty())                            b["topic"] = std::string{topic};
    if (auto v = q.dial("time_range"); !v.empty()) b["time_range"] = std::string{v};
    if (!depth.empty())                            b["search_depth"] = std::string{depth};
    if (q.dial("exact_match") == "true")           b["exact_match"] = true;
    if (q.dial("language_filter") == "only" && !q.language.empty())
        b["filter_by_language"] = true;
    if (auto c = tavily_country(q.dial("country")); !c.empty() && topic.empty())
        b["country"] = c;
    if (q.dial("safe_search") == "true" && depth != "fast" && depth != "ultra-fast")
        b["safe_search"] = true;
    std::vector<std::pair<std::string, std::string>> h;
    if (key.empty()) h.push_back({"x-tavily-access-mode", "keyless"});
    else             h.push_back({"authorization", "Bearer " + std::string{key}});
    return json_post("https://api.tavily.com/search", b, std::move(h));
}

// Firecrawl, keyless or keyed. [D]+[L] POST /v2/search ; limit 1-100 ;
// tbs=qdr:h|d|w|m|y, sbd:1 sorts by date and joins a window with a comma ;
// categories is a list of strings ; excludeDomains takes bare hostnames ;
// safe is a boolean. Keyless = no Authorization header at all.
HttpRequest req_firecrawl(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"limit", q.count}};
    if (!q.exclude.empty()) b["excludeDomains"] = q.exclude;
    std::string tbs{q.dial("freshness")};
    if (q.dial("sort") == "date") tbs = tbs.empty() ? "sbd:1" : "sbd:1," + tbs;
    if (!tbs.empty()) b["tbs"] = std::move(tbs);
    if (auto v = q.dial("category"); !v.empty())  b["categories"] = json::array({std::string{v}});
    if (auto v = q.dial("country"); !v.empty())   b["country"] = std::string{v};
    if (q.dial("safe") == "true")                 b["safe"] = true;
    if (auto v = q.dial("location"); !v.empty())  b["location"] = std::string{v};
    std::vector<std::pair<std::string, std::string>> h;
    if (!key.empty()) h.push_back({"authorization", "Bearer " + std::string{key}});
    return json_post("https://api.firecrawl.dev/v2/search", b, std::move(h));
}

// Brave API. [D] GET /res/v1/web/search ; header X-Subscription-Token ;
// count 1-20 ; text_decorations=false drops the <strong> highlight markers ;
// spellcheck=false searches the query as sent.
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
    if (q.dial("spellcheck") == "false")
        r.url += "&spellcheck=false";
    if (auto v = q.dial("goggles"); !v.empty())
        r.url += "&goggles=" + url_escape(v);
    r.headers.push_back({"accept", "application/json"});
    r.headers.push_back({"x-subscription-token", std::string{key}});
    return r;
}

// Exa. [D] POST /search ; x-api-key ; numResults 1-100 ; contents.highlights
// puts excerpts on each result ; startPublishedDate is an ISO 8601 instant ;
// userLocation takes the bare ISO alpha-2 string form ; moderation is boolean.
HttpRequest req_exa(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"numResults", q.count},
              {"contents", {{"highlights", true}}}};
    if (!q.exclude.empty()) b["excludeDomains"] = q.exclude;
    if (auto v = q.dial("type"); !v.empty())     b["type"] = std::string{v};
    if (auto v = q.dial("category"); !v.empty()) b["category"] = std::string{v};
    if (int h = hours_for(q.dial("freshness")); h > 0
        && q.now != std::chrono::system_clock::time_point{})   // host gave the time
        b["startPublishedDate"] = iso_hours_ago(q.now, h);
    if (q.dial("moderation") == "true")             b["moderation"] = true;
    if (auto v = q.dial("country"); !v.empty())     b["userLocation"] = std::string{v};
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

// Serply. [L] GET /v1/search/?q=&num= ; X-Api-Key ; num stops at 10 ; gl and
// tbs pass through to Google ; hl does not change the results, so no language.
// gl is always sent, us by default: without it the proxy region is random.
// Cloudflare in front of it refuses some default client user-agents, so the
// request names itself.
HttpRequest req_serply(const WebSearchQuery& q, std::string_view key) {
    HttpRequest r;
    r.method = "GET";
    r.url = "https://api.serply.io/v1/search/?q=" + url_escape(q.text)
          + "&num=" + std::to_string(q.count);
    auto country = q.dial("country");
    r.url += "&gl=" + (country.empty() ? std::string{"us"}
                                       : url_escape(lower(std::string{country})));
    if (auto v = q.dial("freshness"); !v.empty())
        r.url += "&tbs=" + url_escape(v);
    r.headers.push_back({"accept", "application/json"});
    r.headers.push_back({"user-agent", "mcp-cpp"});
    r.headers.push_back({"x-api-key", std::string{key}});
    return r;
}

// Perplexity Search. [D] POST /search ; Bearer ; max_results 1-20 without a
// tier ; country is ISO alpha-2 ; search_language_filter is a list of ISO 639-1
// codes ; search_recency_filter is hour|day|week|month|year ; search_type is
// web|fast|people (people is not offered: it searches for people, not pages) ;
// search_domain_filter takes at most 20 hosts, a "-" in front excluding one,
// and is either all allow or all deny (only deny is sent).
HttpRequest req_perplexity(const WebSearchQuery& q, std::string_view key) {
    json b = {{"query", q.text}, {"max_results", q.count}};
    if (!q.language.empty()) b["search_language_filter"] = json::array({q.language});
    if (auto v = q.dial("country"); !v.empty())  b["country"] = std::string{v};
    if (auto v = q.dial("recency"); !v.empty())  b["search_recency_filter"] = std::string{v};
    if (auto v = q.dial("search_type"); !v.empty()) b["search_type"] = std::string{v};
    if (!q.exclude.empty()) {
        json deny = json::array();
        for (const auto& host : q.exclude) deny.push_back("-" + host);
        b["search_domain_filter"] = std::move(deny);
    }
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
            if (!s.empty()) { h.snippet = tidy(flatten_markdown(s), 500); break; }
        }
        if (h.url.empty()) continue;
        if (h.title.empty()) h.title = h.url;
        out.push_back(std::move(h));
    }
    return out;
}

// Percent-encode an id for a URL path, keeping '/' (a DOI and an old-style
// arXiv id such as hep-th/9901001 both carry one).
std::string path_escape(std::string_view s) {
    std::string out;
    for (std::size_t from = 0;;) {
        const auto slash = s.find('/', from);
        out += url_escape(s.substr(from, slash == std::string_view::npos
                                             ? std::string_view::npos : slash - from));
        if (slash == std::string_view::npos) return out;
        out += '/';
        from = slash + 1;
    }
}

// A Firecrawl paper record carries ids, not a link. The four primaryId forms
// Firecrawl documents become the paper's own page; anything else falls back
// to Firecrawl's paper endpoint by the record's canonical paperId, which
// answers for every record. "" when the record has neither.
std::string paper_url(const json& r) {
    const std::string primary = str_at(r, "primaryId");
    if (const auto colon = primary.find(':');
        colon != std::string::npos && colon + 1 < primary.size()) {
        const std::string ns = lower(primary.substr(0, colon));
        std::string id = primary.substr(colon + 1);
        if (ns == "arxiv") return "https://arxiv.org/abs/" + path_escape(id);
        if (ns == "pmid")  return "https://pubmed.ncbi.nlm.nih.gov/" + url_escape(id) + "/";
        if (ns == "pmcid") {
            // PMC writes its ids as PMC1234567; whether Firecrawl keeps the
            // prefix is not documented, so both spellings are accepted.
            if (lower(id.substr(0, 3)) != "pmc") id = "PMC" + id;
            return "https://pmc.ncbi.nlm.nih.gov/articles/" + url_escape(id) + "/";
        }
        if (ns == "doi")   return "https://doi.org/" + path_escape(id);
    }
    if (const std::string paper = str_at(r, "paperId"); !paper.empty())
        return "https://api.firecrawl.dev/v2/search/research/papers/" + url_escape(paper);
    return {};
}

// Firecrawl's data.research: paper records (title, abstract, ids).
std::vector<SearchHit> collect_papers(const json& arr, int count) {
    std::vector<SearchHit> out;
    if (!arr.is_array()) return out;
    for (const auto& r : arr) {
        if (static_cast<int>(out.size()) >= count) break;
        if (!r.is_object()) continue;
        SearchHit h;
        h.url = paper_url(r);
        if (h.url.empty() || h.url.size() > 2048) continue;   // never a cut-off link
        h.title = tidy(str_at(r, "title"), 300);
        if (h.title.empty()) h.title = h.url;
        h.snippet = tidy(str_at(r, "abstract"), 500);
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
            if (id != d.id) continue;
            if (web_search_dial_value_ok(d, val)) out.emplace_back(id, val);
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
        || id == "exa-api" || id == "perplexity-api";
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
    if (id == "serply-api")     return req_serply(q, key);
    if (id == "perplexity-api") return req_perplexity(q, key);
    return {};   // unreachable for a catalogue id; empty url fails the send
}

WebSearchFailure classify_http(const WebSearchService& svc, int status,
                            std::string_view body) {
    // 2xx that is not results: a challenge page served with 200/202.
    //
    // Markers, not the bare word "captcha": every NORMAL Brave results page
    // carries "Switch to traditional captcha" in its inline i18n bundle (25 of
    // 25 saved good pages). The classifier only sees a 2xx page when the
    // parser found nothing on it, so the bare word turned "the layout changed"
    // or "no results for this query" into a false "blocked". These are the
    // strings only the challenge pages carry:
    //   Brave       page:"/captcha" (its router state) and "flagged as
    //               being suspicious" (the visible text)
    //   DuckDuckGo  "anomaly-modal" / "Select all squares" / "bots use
    //               DuckDuckGo too", on a 202
    if (status >= 200 && status < 300) {
        if (status == 202
            || contains(body, "page:\"/captcha\"")
            || contains_ci(body, "flagged as being suspicious")
            || contains_ci(body, "select all squares")
            || contains(body, "anomaly-modal")
            || contains_ci(body, "bots use duckduckgo too"))
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
        // Web results, then paper records. From 2026-11-16 the research
        // category answers in data.research and no longer in data.web, so
        // either container makes a readable reply.
        const json& data = field(j, "data");
        const json& web = field(data, "web");
        const json& papers = field(data, "research");
        if (!web.is_array() && !papers.is_array()) out.ok = false;
        out.hits = collect(web, "url", "title", {"description"}, count);
        for (auto& h : collect_papers(papers, count - static_cast<int>(out.hits.size())))
            out.hits.push_back(std::move(h));
    } else if (service_id == "exa-api") {
        out.hits = collect(*need(field(j, "results")), "url", "title",
                           {"highlights", "summary", "text"}, count);
    } else if (service_id == "serper-api") {
        // Serper omits `organic` when Google found nothing.
        const json& o = field(j, "organic");
        out.hits = collect(o, "link", "title", {"snippet"}, count);
    } else if (service_id == "serply-api") {
        // Now and then a row's `link` is an opaque token, not a URL. [L]
        // Such rows are dropped before counting, so they never become hits.
        json rows = json::array();
        for (const auto& r : *need(field(j, "results")))
            if (r.is_object() && str_at(r, "link").rfind("http", 0) == 0)
                rows.push_back(r);
        out.hits = collect(rows, "link", "title", {"description"}, count);
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

bool web_search_dial_value_ok(const WebSearchDial& d, std::string_view value) noexcept {
    if (value.empty()) return false;
    if (d.text == WebSearchText::None)
        return std::any_of(d.values.begin(), d.values.end(),
            [&](const WebSearchDialValue& v) { return v.id == value; });
    if (value.size() > d.max_bytes || !util::is_valid_utf8(value)) return false;
    // No control characters in either kind: the value lands in a URL or a
    // JSON string, and a newline could start a second Goggles rule.
    const bool url = d.text == WebSearchText::HttpsUrl;
    for (char ch : value) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7f || (url && u == ' ')) return false;
    }
    if (url) {
        constexpr std::string_view kScheme = "https://";
        if (value.size() <= kScheme.size() || !value.starts_with(kScheme)) return false;
    }
    return true;
}

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
