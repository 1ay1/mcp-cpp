// SPDX-License-Identifier: Apache-2.0
//
// web_search_services.hpp -- INTERNAL. How web_search talks to each service in the
// public catalogue (mcp/tools/web_search.hpp): the request to send, the reply to
// parse, and what a failed reply means.
//
// Kept apart from web.cpp so that adding a service is one table row plus one
// builder and one parser here, with no change to the fetch/extract code or to
// the failover loop.

#pragma once

#include <mcp/tools/host.hpp>
#include <mcp/tools/web_search.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::tools::detail {

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

// Why a service did not answer. The loop moves on to the next slot for every
// one of these; the distinction is for the person reading the report, who
// needs to know "your key was refused" from "the page layout changed".
enum class WebSearchFailure {
    Blocked,        // captcha / challenge / access refused for this network
    RateLimited,    // too many requests; retry later
    QuotaExhausted, // credits or plan limit used up
    BadKey,         // key missing, malformed, rejected, or lacking scope
    BadRequest,     // the service rejected what we sent
    Transient,      // transport error or 5xx
    Empty,          // answered, but with no results
    Unreadable,     // answered 2xx with something we cannot parse
};

[[nodiscard]] std::string describe_failure(WebSearchFailure f, int http_status);

// Percent-encode for a query string or form body (RFC 3986 unreserved kept).
[[nodiscard]] std::string url_escape(std::string_view s);

// Pull `-site:host` tokens out of a query. Services with a native domain
// filter take the list instead of the operator, which they would otherwise
// treat as plain words. A query that is nothing BUT exclusions is returned
// unchanged with an empty list, so we never send an empty query.
struct SiteSplit {
    std::string                query;
    std::vector<std::string>   exclude;   // lower-case hosts, at most 20
};
[[nodiscard]] SiteSplit split_site_exclusions(std::string_view query);

struct WebSearchQuery {
    std::string text;
    int         count = 10;          // already clamped to the service's max
    std::string language;            // "" or two lower-case letters
    // Hosts to leave out, for services with a native domain filter. Empty for
    // the others, which keep `-site:` in the query text.
    std::vector<std::string> exclude;
    // Validated dial choices: dial id -> value id, only non-default values.
    std::vector<std::pair<std::string, std::string>> dials;
    std::chrono::system_clock::time_point now{};   // from WebSearchPlan::now

    [[nodiscard]] std::string_view dial(std::string_view id) const noexcept;
};

// Keep only the dials `svc` actually offers, with a value that passes
// web_search_dial_value_ok, and drop the default. Anything else is discarded
// rather than forwarded.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
validate_dials(const WebSearchService& svc, const WebSearchSlot& slot);

// True when the service has a request field for excluded domains (and so
// reads them from WebSearchQuery::exclude rather than from `-site:` in the text).
[[nodiscard]] bool has_native_exclude(std::string_view id) noexcept;

// True when the service answers with an HTML page that web.cpp's parsers read.
[[nodiscard]] bool is_html_service(std::string_view id) noexcept;

// A key is a header value: refuse control characters so it cannot smuggle a
// second header. Returns the trimmed key, or "" when unusable.
[[nodiscard]] std::string clean_api_key(std::string_view key);

// The request for `svc`. Header names are lower-case. Never puts a key in the
// URL.
[[nodiscard]] HttpRequest build_web_search_request(const WebSearchService& svc,
                                               std::string_view api_key,
                                               const WebSearchQuery& q);

// Judge a non-2xx (or challenge) reply.
[[nodiscard]] WebSearchFailure classify_http(const WebSearchService& svc,
                                          int status, std::string_view body);

// Parse a JSON service's 2xx reply. `ok` is false when the body is not the
// shape the service documents.
struct JsonHits {
    bool                   ok = true;
    std::vector<SearchHit> hits;
};
[[nodiscard]] JsonHits parse_json_hits(std::string_view service_id,
                                       std::string_view body, int count);

} // namespace mcp::tools::detail
