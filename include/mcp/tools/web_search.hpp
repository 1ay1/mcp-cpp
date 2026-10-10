// SPDX-License-Identifier: Apache-2.0
//
// web_search.hpp -- the menu of services web_search can use, and the plan a host
// hands the tool to say which of them to try, in what order, with what key.
//
// web_search used to be a hard-coded chain of three HTML scrapers. Scrapers
// are the one thing in this library that breaks without anyone touching the
// code: a captcha, a rate limit or a layout change on someone else's page. A
// search *API* does not. So the tool now takes a plan: up to three slots,
// tried in order, first to answer wins, each slot naming one service from the
// catalogue below.
//
//   * "Free" services need no account, card or key. They are best-effort: a
//     scrape of a public page, or a vendor's key-less tier. They can be
//     blocked, rate limited or withdrawn without notice.
//   * "API key" services are the vendor's documented, paid-or-metered
//     interface. The host owns the key; this library only puts it in the
//     request (in a header, never in a URL, so a logged URL is not a leak).
//
// This header is the whole contract. The library owns the *what* (which
// services exist, what each one's request looks like, what a failure means);
// the host owns the *which* (the user's choice) and the *secret* (where a key
// is stored). Nothing here touches the disk, the environment or a keystore.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::tools {

// One selectable value of a dial. `id` is what a host stores; "" is the
// service's own default, which sends nothing at all.
struct WebSearchDialValue {
    std::string id;
    std::string label;
};

// One extra knob a service honours (freshness, region, ...). A service
// declares at most three. `values[0]` is always the default (id "").
struct WebSearchDial {
    std::string id;
    std::string label;
    std::string help;
    std::vector<WebSearchDialValue> values;
};

struct WebSearchService {
    std::string id;        // stable, stored by hosts: "brave-free", "exa-api"
    std::string label;     // shown to people and in the tool's "[via ...]" line
    bool        needs_key = false;
    std::string signup;    // where to get a key / read the terms (keyed only)
    std::string note;      // one honest line about limits and caveats
    int         max_count = 20;  // most results one request can ask for
    bool        language = false;  // honours WebSearchPlan::language
    std::vector<WebSearchDial> dials;  // 0..3
};

// Every service, in menu order: the Free ones first, then the API-key ones.
[[nodiscard]] const std::vector<WebSearchService>& web_search_services();

// nullptr when `id` is not in the catalogue.
[[nodiscard]] const WebSearchService* find_web_search_service(std::string_view id);

// One slot of a plan.
struct WebSearchSlot {
    std::string service;   // WebSearchService::id
    std::string api_key;   // empty for Free services
    // dial id -> value id. A dial that is absent, or whose value is not one
    // the service lists, is ignored -- a stale setting can never put arbitrary
    // text into a request.
    std::vector<std::pair<std::string, std::string>> dials;
};

struct WebSearchPlan {
    // Primary, Secondary, Fallback -- tried in this order. At most three are
    // used; an empty slot (service "") is skipped.
    std::vector<WebSearchSlot> slots;
    // ISO 639-1 code ("en", "de"), or "" for each service's own default.
    // Only services with WebSearchService::language set use it.
    std::string language;
};

// What the tool uses when the host supplies no plan: Brave Free, then
// DuckDuckGo Free, then Tavily Free. No key needed anywhere.
[[nodiscard]] WebSearchPlan default_web_search_plan();

// Read per call, so a change the user makes reaches the next search with no
// restart. Called on the tool's worker thread: implementations must be
// thread-safe and must not block.
class WebSearchPlanSource {
public:
    virtual ~WebSearchPlanSource() = default;
    [[nodiscard]] virtual WebSearchPlan plan() const = 0;
};

} // namespace mcp::tools
