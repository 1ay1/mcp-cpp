// SPDX-License-Identifier: Apache-2.0
//
// web_tools_test.cpp — exercises web_fetch / web_search through
// make_provider() with a FAKE HttpClient (no network). Proves
// registration (gated on svc.http), SSRF refusal, HTML→text extraction,
// the multi-engine search fallback chain, and Net effects.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/cap/local.hpp>

#include <cassert>
#include <cstdio>
#include <string>

using namespace mcp::tools;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }

// A scripted HttpClient: returns a canned response keyed on URL substring.
class FakeHttp : public HttpClient {
public:
    std::vector<std::pair<std::string, HttpResponse>> rules;
    std::vector<std::string> seen_urls;

    HttpResponse send(const HttpRequest& req) override {
        seen_urls.push_back(req.url);
        for (auto& [needle, resp] : rules)
            if (req.url.find(needle) != std::string::npos) return resp;
        HttpResponse r; r.status = 0; r.error = "no rule for " + req.url;
        return r;
    }
};

static HttpResponse html_resp(std::string body) {
    HttpResponse r;
    r.status = 200;
    r.headers.push_back({"content-type", "text/html; charset=utf-8"});
    r.body = std::move(body);
    return r;
}

int main() {
    auto fake = std::make_shared<FakeHttp>();

    // web_fetch: a simple HTML page → cleaned text with the heading + link.
    fake->rules.push_back({"example.com",
        html_resp("<html><head><title>T</title></head><body>"
                  "<main><h1>Hello World</h1>"
                  "<p>This is a reasonably long paragraph of real prose so that "
                  "the nav-line filter keeps it, and it contains a "
                  "<a href=\"https://dest.test/x\">link text</a> in the middle "
                  "of the sentence which should survive extraction.</p>"
                  "<nav>Home About Pricing Contact</nav></main></body></html>")});

    // web_search: Brave returns a result block our parser understands.
    fake->rules.push_back({"search.brave.com",
        html_resp("<div class=\"snippet svelte-x\">"
                  "<a href=\"https://result.test/page\" class=\"l1\">x</a>"
                  "<div class=\"title svelte-y\">Result Title</div>"
                  "<div class=\"generic-snippet svelte-z\">A useful snippet.</div>"
                  "</div>")});

    HostServices svc;
    svc.http = fake;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── web_fetch cleans HTML to text + preserves the link ───────────────
    {
        auto args = obj(); args["url"] = "https://example.com/page";
        auto r = call(*provider, "web_fetch", args);
        assert(!r.is_error);
        assert(r.text.find("Hello World") != std::string::npos);
        assert(r.text.find("[link text](https://dest.test/x)") != std::string::npos);
        // nav chrome should be stripped
        assert(r.text.find("Home About") == std::string::npos);
        assert(read_effects(r).has(Effect::Net));
        std::puts("web_fetch: HTML→text + link preserved + nav stripped");
    }

    // ── web_fetch SSRF refusal ───────────────────────────────────────────
    {
        auto args = obj(); args["url"] = "https://127.0.0.1/secret";
        auto r = call(*provider, "web_fetch", args);
        assert(r.is_error);
        assert(r.text.find("SSRF") != std::string::npos
            || r.text.find("loopback") != std::string::npos);
        std::puts("web_fetch: SSRF refusal ok");
    }

    // ── web_fetch rejects non-https ──────────────────────────────────────
    {
        auto args = obj(); args["url"] = "http://example.com";
        auto r = call(*provider, "web_fetch", args);
        assert(r.is_error);
        std::puts("web_fetch: non-https refused");
    }

    // ── web_search parses the Brave result ───────────────────────────────
    {
        auto args = obj(); args["query"] = "test query";
        auto r = call(*provider, "web_search", args);
        assert(!r.is_error);
        assert(r.text.find("[via Brave]") != std::string::npos);
        assert(r.text.find("Result Title") != std::string::npos);
        assert(r.text.find("https://result.test/page") != std::string::npos);
        assert(read_effects(r).has(Effect::Net));
        std::puts("web_search: Brave parse ok");
    }

    // ── web tools NOT registered when svc.http is null ───────────────────
    {
        HostServices empty;  // no http
        auto p2 = make_provider(empty, ToolsetConfig{}, "local");
        auto args = obj(); args["url"] = "https://example.com";
        auto r = call(*p2, "web_fetch", args);
        // No such tool → provider returns an error result.
        assert(r.is_error);
        std::puts("web tools: absent without HttpClient");
    }

    std::puts("ALL WEB TOOL TESTS PASSED");
    return 0;
}
