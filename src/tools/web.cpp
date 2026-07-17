// SPDX-License-Identifier: Apache-2.0
//
// web.cpp — register_web_tools: web_fetch / web_search.
// Faithful port of agentty's src/tool/tools/{web_fetch,web_search}.cpp.
// The ONLY change is the transport seam: agentty called
// http::default_client().send() directly with a host/port/path Request;
// here every request routes through the injected HttpClient (host.hpp),
// which owns TLS + redirect-following. The HTML→text extraction, SSRF
// guard, SPA/jina fallback, fragment clipping, and multi-engine search
// parsing are ported verbatim.
//
// Because the injected HttpClient follows redirects itself, the manual
// per-hop redirect loop in agentty's web_fetch collapses to a single
// send(); the SSRF guard still runs on the requested host.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/host.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/utf8.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

// ── Transport adapter over the injected HttpClient ─────────────────────

struct HttpReply {
    bool ok = false;
    int status = 0;
    std::string content_type;
    std::string body;
    std::string err;
};

// Issue one request through the injected client. `url` is a full
// https:// URL; the client follows redirects + handles TLS. Header
// names are lower-cased by callers already.
HttpReply http_send(HttpClient& client, const std::string& method,
                    const std::string& url,
                    const std::vector<std::pair<std::string,std::string>>& headers,
                    const std::string& body = {}) {
    HttpRequest req;
    req.method = method;
    req.url = url;
    req.headers = headers;
    req.body = body;
    HttpResponse r = client.send(req);
    HttpReply out;
    if (r.status == 0) { out.err = r.error.empty() ? "transport failure" : r.error; return out; }
    out.ok = true;
    out.status = r.status;
    out.body = std::move(r.body);
    for (const auto& h : r.headers) {
        std::string name; name.reserve(h.first.size());
        for (char c : h.first) name.push_back(static_cast<char>(std::tolower((unsigned char)c)));
        if (name == "content-type") { out.content_type = h.second; break; }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════
//  web_fetch
// ═══════════════════════════════════════════════════════════════════════

struct WebFetchArgs {
    std::string url;
    std::string method;   // "GET" | "HEAD" | "POST"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string display_description;
    bool allow_jina = true;
};

std::expected<WebFetchArgs, ToolError> parse_web_fetch_args(const json& j) {
    util::ArgReader ar(j);
    auto url_opt = ar.require_str("url");
    if (!url_opt)
        return std::unexpected(ToolError::invalid_args("url required"));
    std::string url = *std::move(url_opt);
    if (!url.starts_with("https://"))
        return std::unexpected(ToolError::invalid_args(
            "url must start with https:// (web_fetch is TLS-only)"));
    std::vector<std::pair<std::string, std::string>> hdrs;
    bool allow_jina = true;
    if (const json* h = ar.raw("headers"); h && h->is_object()) {
        for (auto& [k, v] : h->items()) {
            std::string lower; lower.reserve(k.size());
            for (char c : k) lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));
            std::string val = v.is_string() ? v.get<std::string>() : v.dump();
            if (lower == "x-no-jina" || lower == "x-agentty-no-jina") {
                allow_jina = !(val == "1" || val == "true" || val == "yes");
                continue;
            }
            hdrs.emplace_back(std::move(lower), std::move(val));
        }
    }
    std::string method = ar.str("method", "GET");
    if (method == "HEAD" || method == "POST" || method == "GET") {} else method = "GET";
    return WebFetchArgs{
        std::move(url),
        std::move(method),
        std::move(hdrs),
        ar.str("display_description", ""),
        allow_jina,
    };
}

constexpr size_t kMaxFetchBytes = 64'000;

struct ParsedUrl {
    std::string host;
    uint16_t port = 443;
    std::string path = "/";
    std::string fragment;
};

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
    constexpr std::string_view k = "https://";
    if (!url.starts_with(k)) return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(k.size());
    auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    ParsedUrl out;
    std::string path_and_frag = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
    if (auto hash = path_and_frag.find('#'); hash != std::string::npos) {
        out.fragment = path_and_frag.substr(hash + 1);
        path_and_frag.resize(hash);
        if (path_and_frag.empty()) path_and_frag = "/";
    }
    out.path = std::move(path_and_frag);
    if (auto colon = authority.find(':'); colon != std::string_view::npos) {
        out.host.assign(authority.substr(0, colon));
        try {
            out.port = static_cast<uint16_t>(std::stoi(std::string{authority.substr(colon + 1)}));
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    } else {
        out.host.assign(authority);
    }
    if (out.host.empty()) return std::unexpected(std::string{"empty host"});
    return out;
}

[[nodiscard]] bool is_blocked_host(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    std::string h{host};
    for (char& c : h) c = static_cast<char>(std::tolower(c));
    if (h == "localhost" || h.ends_with(".localhost")) return true;
    if (h == "metadata" || h == "metadata.google.internal") return true;
    if (h == "0") return true;
    if (h == "::1" || h == "::") return true;
    if (h.starts_with("fc") || h.starts_with("fd")) return true;
    if (h.starts_with("fe80:") || h.starts_with("fe8")
        || h.starts_with("fe9") || h.starts_with("fea")
        || h.starts_with("feb")) return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(h.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4
        && a < 256 && b < 256 && c < 256 && d < 256) {
        if (a == 127) return true;
        if (a == 0)   return true;
        if (a == 10)  return true;
        if (a == 169 && b == 254) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 192 && b == 168) return true;
        if (a == 100 && b >= 64 && b <= 127) return true;
        if (a >= 224) return true;
    }
    return false;
}

// ── HTML → plain-text extraction (verbatim) ────────────────────────────

void decode_entities(std::string& s) {
    struct E { std::string_view from; char to; };
    static constexpr E table[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''},
        {"&apos;", '\''}, {"&nbsp;", ' '},
    };
    for (const auto& e : table) {
        size_t p = 0;
        while ((p = s.find(e.from, p)) != std::string::npos) {
            s.replace(p, e.from.size(), 1, e.to);
            p += 1;
        }
    }
    size_t p = 0;
    while ((p = s.find("&#", p)) != std::string::npos) {
        size_t semi = s.find(';', p);
        if (semi == std::string::npos || semi - p > 8) { p += 2; continue; }
        std::string_view body{s.data() + p + 2, semi - p - 2};
        unsigned code = 0;
        bool ok = false;
        try {
            if (!body.empty() && (body.front() == 'x' || body.front() == 'X'))
                code = std::stoul(std::string{body.substr(1)}, nullptr, 16);
            else
                code = std::stoul(std::string{body});
            ok = true;
        } catch (...) {}
        if (ok && code >= 0x20 && code < 0x7f) {
            s.replace(p, semi - p + 1, 1, static_cast<char>(code));
            p += 1;
        } else {
            p = semi + 1;
        }
    }
}

void strip_region(std::string& s, std::string_view tag) {
    auto find_ci = [&s](std::string_view needle, size_t from) {
        if (needle.empty() || from >= s.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= s.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                char a = s[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { match = false; break; }
            }
            if (match) return i;
        }
        return std::string::npos;
    };
    std::string open_tag  = "<"  + std::string{tag};
    std::string close_tag = "</" + std::string{tag} + ">";
    size_t p = 0;
    while ((p = find_ci(open_tag, p)) != std::string::npos) {
        char next = (p + open_tag.size() < s.size()) ? s[p + open_tag.size()] : '\0';
        if (next != '>' && next != ' ' && next != '\t' && next != '\n'
            && next != '\r' && next != '/') {
            p += open_tag.size();
            continue;
        }
        size_t end = find_ci(close_tag, p + open_tag.size());
        if (end == std::string::npos) { s.resize(p); break; }
        s.erase(p, (end + close_tag.size()) - p);
    }
}

void flatten_anchors(std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t open = s.find("<a ", pos);
        size_t open2 = s.find("<A ", pos);
        size_t a_start = std::min(open, open2);
        if (a_start == std::string::npos) break;
        size_t tag_close = s.find('>', a_start);
        if (tag_close == std::string::npos) break;
        std::string_view open_tag{s.data() + a_start, tag_close - a_start};
        std::string href;
        for (size_t i = 0; i + 5 < open_tag.size(); ++i) {
            char c0 = open_tag[i], c1 = open_tag[i+1], c2 = open_tag[i+2], c3 = open_tag[i+3];
            if ((c0 == 'h' || c0 == 'H') && (c1 == 'r' || c1 == 'R')
                && (c2 == 'e' || c2 == 'E') && (c3 == 'f' || c3 == 'F')) {
                size_t eq = open_tag.find('=', i);
                if (eq == std::string_view::npos) break;
                size_t v = eq + 1;
                while (v < open_tag.size() && (open_tag[v] == ' ' || open_tag[v] == '\t')) ++v;
                if (v >= open_tag.size()) break;
                char q = open_tag[v];
                size_t vend;
                if (q == '"' || q == '\'') {
                    vend = open_tag.find(q, v + 1);
                    if (vend == std::string_view::npos) break;
                    href.assign(open_tag.data() + v + 1, vend - v - 1);
                } else {
                    vend = v;
                    while (vend < open_tag.size() && open_tag[vend] != ' '
                           && open_tag[vend] != '>' && open_tag[vend] != '\t')
                        ++vend;
                    href.assign(open_tag.data() + v, vend - v);
                }
                break;
            }
        }
        size_t close = s.find("</a>", tag_close + 1);
        size_t close2 = s.find("</A>", tag_close + 1);
        size_t a_close = std::min(close, close2);
        if (a_close == std::string::npos) { pos = tag_close + 1; continue; }
        std::string inner = s.substr(tag_close + 1, a_close - tag_close - 1);
        std::string text;
        text.reserve(inner.size());
        bool in_tag = false;
        for (char c : inner) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) text += c;
        }
        std::string trimmed;
        trimmed.reserve(text.size());
        bool ws = false;
        for (char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!trimmed.empty() && !ws) { trimmed += ' '; ws = true; }
            } else {
                trimmed += c; ws = false;
            }
        }
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        std::string replacement;
        if (!trimmed.empty() && !href.empty() && href[0] != '#'
            && !href.starts_with("javascript:")) {
            replacement = "[" + trimmed + "](" + href + ")";
        } else if (!trimmed.empty()) {
            replacement = trimmed;
        }
        s.replace(a_start, (a_close + 4) - a_start, replacement);
        pos = a_start + replacement.size();
    }
}

void promote_headings(std::string& s) {
    for (int level = 1; level <= 6; ++level) {
        std::string open  = "<h" + std::to_string(level);
        std::string close = "</h" + std::to_string(level) + ">";
        size_t p = 0;
        while ((p = s.find(open, p)) != std::string::npos) {
            size_t tag_close = s.find('>', p);
            if (tag_close == std::string::npos) break;
            size_t end = s.find(close, tag_close);
            if (end == std::string::npos) break;
            std::string text = s.substr(tag_close + 1, end - tag_close - 1);
            std::string clean;
            bool in_tag = false;
            for (char c : text) {
                if (c == '<') in_tag = true;
                else if (c == '>') in_tag = false;
                else if (!in_tag) clean += c;
            }
            std::string hashes(static_cast<size_t>(level), '#');
            std::string replacement = "\n\n" + hashes + " " + clean + "\n\n";
            s.replace(p, (end + close.size()) - p, replacement);
            p += replacement.size();
        }
    }
}

void mark_block_boundaries(std::string& s) {
    static constexpr std::string_view blocks[] = {
        "</p>", "</div>", "</li>", "</tr>", "</article>", "</section>",
        "</header>", "</footer>", "</blockquote>", "</pre>",
        "<br>", "<br/>", "<br />", "<hr>", "<hr/>", "<hr />",
        "<li>", "<p>", "<tr>",
    };
    for (auto tag : blocks) {
        size_t p = 0;
        while ((p = s.find(tag, p)) != std::string::npos) {
            s.insert(p, "\n");
            p += tag.size() + 1;
        }
    }
}

void strip_tags(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!in_tag) {
            if (c == '<') {
                if (i + 3 < s.size() && s[i+1] == '!' && s[i+2] == '-' && s[i+3] == '-') {
                    size_t end = s.find("-->", i + 4);
                    i = (end == std::string::npos) ? s.size() : end + 2;
                    continue;
                }
                in_tag = true;
            } else {
                out += c;
            }
        } else if (c == '>') {
            in_tag = false;
        }
    }
    s = std::move(out);
}

void collapse_whitespace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    int consec_nl = 0;
    bool last_space = false;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') {
            consec_nl++;
            last_space = false;
            if (consec_nl <= 2) out += '\n';
        } else if (c == ' ' || c == '\t') {
            if (consec_nl > 0) continue;
            if (last_space) continue;
            out += ' ';
            last_space = true;
        } else {
            consec_nl = 0;
            last_space = false;
            out += c;
        }
    }
    size_t start = out.find_first_not_of(" \n\t");
    if (start != std::string::npos) out.erase(0, start);
    size_t end = out.find_last_not_of(" \n\t");
    if (end != std::string::npos) out.resize(end + 1);
    s = std::move(out);
}

std::string extract_main_content(std::string body) {
    struct Candidate { std::string_view tag; std::string_view attr_match; };
    static constexpr Candidate cands[] = {
        {"main",    ""},
        {"article", ""},
        {"div",     "role=\"main\""},
        {"div",     "id=\"content\""},
        {"div",     "id=\"main\""},
        {"div",     "id=\"main-content\""},
        {"div",     "id=\"mw-content-text\""},
        {"div",     "id=\"primary\""},
        {"div",     "class=\"content\""},
        {"section", "id=\"content\""},
    };
    auto find_ci = [&body](std::string_view needle, size_t from) -> size_t {
        if (needle.empty() || from >= body.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= body.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                char a = body[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return i;
        }
        return std::string::npos;
    };
    for (const auto& c : cands) {
        std::string open  = "<" + std::string{c.tag};
        std::string close = "</" + std::string{c.tag} + ">";
        size_t search_from = 0;
        size_t pick_start  = std::string::npos;
        while (search_from < body.size()) {
            size_t p = find_ci(open, search_from);
            if (p == std::string::npos) break;
            char nx = (p + open.size() < body.size()) ? body[p + open.size()] : '\0';
            if (nx != '>' && nx != ' ' && nx != '\t' && nx != '\n' && nx != '/') {
                search_from = p + open.size();
                continue;
            }
            size_t gt = body.find('>', p);
            if (gt == std::string::npos) break;
            if (c.attr_match.empty()
                || body.find(c.attr_match, p) < gt) {
                pick_start = p;
                break;
            }
            search_from = gt + 1;
        }
        if (pick_start == std::string::npos) continue;
        size_t end = std::string::npos;
        if (c.tag != "div" && c.tag != "section") {
            end = find_ci(close, pick_start + open.size());
        } else {
            int depth = 1;
            size_t cur = body.find('>', pick_start);
            if (cur == std::string::npos) continue;
            ++cur;
            while (cur < body.size() && depth > 0) {
                size_t no = find_ci(open,  cur);
                size_t nc = find_ci(close, cur);
                if (nc == std::string::npos) break;
                if (no != std::string::npos && no < nc) {
                    char nx = (no + open.size() < body.size()) ? body[no + open.size()] : '\0';
                    if (nx == '>' || nx == ' ' || nx == '\t' || nx == '\n' || nx == '/') {
                        ++depth;
                        cur = no + open.size();
                        continue;
                    }
                    cur = no + open.size();
                    continue;
                }
                --depth;
                if (depth == 0) { end = nc; break; }
                cur = nc + close.size();
            }
        }
        if (end == std::string::npos) continue;
        size_t subtree_len = end - pick_start;
        if (subtree_len < 200) continue;
        if (subtree_len < body.size() / 20) continue;
        return body.substr(pick_start, end - pick_start + close.size());
    }
    return body;
}

void strip_boilerplate_classes(std::string& s) {
    static constexpr std::string_view tokens[] = {
        "cookie", "banner", "consent", "gdpr",
        "ad-", "-ad ", " ad ", "advert", "sponsor",
        "share", "social", "newsletter", "subscribe", "signup",
        "related", "recommend", "trending", "popular",
        "comments", "disqus",
        "sidebar", "side-bar", "sider", "rail",
        "navbar", "navigation", "nav-", " nav ", "menu",
        "footer", "site-footer", "page-footer",
        "header", "site-header", "page-header", "masthead",
        "breadcrumb", "crumbs",
        "toolbar", "toolkit", "promo", "popup", "modal",
        "pagination", "pager",
        "toc-", "table-of-contents",
        "skip-link", "skip-to",
    };
    auto matches_token = [](std::string_view attr_val) {
        std::string lo;
        lo.reserve(attr_val.size() + 2);
        lo += ' ';
        for (char c : attr_val) lo += static_cast<char>(std::tolower((unsigned char)c));
        lo += ' ';
        for (auto tok : tokens) {
            if (lo.find(tok) != std::string::npos) return true;
        }
        return false;
    };
    size_t i = 0;
    while (i + 1 < s.size()) {
        if (s[i] != '<' || s[i+1] == '/' || s[i+1] == '!') { ++i; continue; }
        size_t gt = s.find('>', i);
        if (gt == std::string::npos) break;
        size_t name_end = i + 1;
        while (name_end < gt && s[name_end] != ' ' && s[name_end] != '\t'
               && s[name_end] != '\n' && s[name_end] != '/' && s[name_end] != '>')
            ++name_end;
        std::string tag;
        tag.reserve(name_end - i - 1);
        for (size_t k = i + 1; k < name_end; ++k)
            tag += static_cast<char>(std::tolower((unsigned char)s[k]));
        static constexpr std::string_view void_tags[] = {
            "br", "hr", "img", "input", "meta", "link", "source", "area", "col",
            "embed", "param", "track", "wbr"
        };
        bool is_void = false;
        for (auto v : void_tags) if (tag == v) { is_void = true; break; }
        if (is_void) { i = gt + 1; continue; }
        if (tag.empty()) { i = gt + 1; continue; }
        std::string_view tag_body{s.data() + i, gt - i};
        auto pull_attr = [&](std::string_view name) -> std::string_view {
            for (size_t p = 0; p + name.size() + 2 < tag_body.size(); ++p) {
                if ((p == 0 || tag_body[p-1] == ' ' || tag_body[p-1] == '\t')
                    && tag_body.compare(p, name.size(), name) == 0
                    && tag_body[p + name.size()] == '=') {
                    size_t v = p + name.size() + 1;
                    if (v >= tag_body.size()) return {};
                    char q = tag_body[v];
                    if (q == '"' || q == '\'') {
                        size_t ve = tag_body.find(q, v + 1);
                        if (ve == std::string_view::npos) return {};
                        return tag_body.substr(v + 1, ve - v - 1);
                    }
                    size_t ve = v;
                    while (ve < tag_body.size() && tag_body[ve] != ' '
                           && tag_body[ve] != '>' && tag_body[ve] != '\t')
                        ++ve;
                    return tag_body.substr(v, ve - v);
                }
            }
            return {};
        };
        auto cls = pull_attr("class");
        auto idv = pull_attr("id");
        bool drop = (!cls.empty() && matches_token(cls))
                 || (!idv.empty() && matches_token(idv));
        if (!drop) { i = gt + 1; continue; }
        std::string open  = "<"  + tag;
        std::string close = "</" + tag + ">";
        int depth = 1;
        size_t cur = gt + 1;
        size_t kill_end = std::string::npos;
        while (cur < s.size() && depth > 0) {
            size_t no = s.find(open,  cur);
            size_t nc = s.find(close, cur);
            if (nc == std::string::npos) break;
            if (no != std::string::npos && no < nc) {
                char nx = (no + open.size() < s.size()) ? s[no + open.size()] : '\0';
                if (nx == '>' || nx == ' ' || nx == '\t' || nx == '\n' || nx == '/') {
                    ++depth;
                    cur = no + open.size();
                    continue;
                }
                cur = no + open.size();
                continue;
            }
            --depth;
            if (depth == 0) { kill_end = nc + close.size(); break; }
            cur = nc + close.size();
        }
        if (kill_end == std::string::npos) { i = gt + 1; continue; }
        s.erase(i, kill_end - i);
    }
}

std::string drop_nav_lines(std::string body) {
    std::string out;
    out.reserve(body.size());
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string_view line{body.data() + i, e - i};
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        bool keep = true;
        if (line.empty()) {
        } else if (line.front() == '#') {
        } else if (line.size() <= 60) {
            int words_outside = 0;
            bool in_link_text = false;
            bool in_link_url  = false;
            bool in_word = false;
            for (size_t k = 0; k < line.size(); ++k) {
                char c = line[k];
                if (c == '[') { in_link_text = true; in_word = false; continue; }
                if (c == ']' && in_link_text) {
                    in_link_text = false;
                    if (k + 1 < line.size() && line[k+1] == '(') { in_link_url = true; ++k; }
                    continue;
                }
                if (c == ')' && in_link_url) { in_link_url = false; continue; }
                if (in_link_text || in_link_url) continue;
                bool ws = (c == ' ' || c == '\t' || c == '|'
                           || c == ':' || c == '-' || c == ',');
                if (!ws && !in_word) { ++words_outside; in_word = true; }
                else if (ws) in_word = false;
            }
            bool has_link = line.find("](") != std::string_view::npos;
            if (has_link && words_outside < 3) keep = false;
            else if (!has_link && line.size() < 4) keep = false;
            else if (line.size() <= 2) keep = false;
        }
        if (keep) {
            out.append(line.data(), line.size());
            out += '\n';
        }
        i = e + 1;
    }
    std::string final2;
    final2.reserve(out.size());
    int nl = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++nl <= 2) final2 += c;
        } else { nl = 0; final2 += c; }
    }
    while (!final2.empty() && (final2.back() == '\n' || final2.back() == ' '))
        final2.pop_back();
    return final2;
}

[[nodiscard]] bool looks_like_html(std::string_view content_type, std::string_view body) {
    if (content_type.find("html") != std::string_view::npos) return true;
    if (content_type.find("xml")  != std::string_view::npos) return true;
    if (!content_type.empty()) return false;
    size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r'
                               || body[i] == '\t' || body[i] == '\xef'))
        ++i;
    std::string_view head = body.substr(i, std::min<size_t>(body.size() - i, 32));
    std::string lower;
    for (char c : head) lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    return lower.starts_with("<!doctype") || lower.starts_with("<html")
        || lower.starts_with("<?xml");
}

std::string html_to_text(std::string body) {
    body = extract_main_content(std::move(body));
    strip_region(body, "script");
    strip_region(body, "style");
    strip_region(body, "noscript");
    strip_region(body, "svg");
    strip_region(body, "head");
    strip_region(body, "template");
    strip_region(body, "iframe");
    strip_region(body, "form");
    strip_region(body, "nav");
    strip_region(body, "aside");
    strip_region(body, "footer");
    strip_region(body, "header");
    strip_region(body, "button");
    strip_region(body, "figure");
    strip_region(body, "picture");
    strip_boilerplate_classes(body);
    promote_headings(body);
    flatten_anchors(body);
    mark_block_boundaries(body);
    strip_tags(body);
    decode_entities(body);
    collapse_whitespace(body);
    body = drop_nav_lines(std::move(body));
    return body;
}

std::string drop_link_rail_runs(std::string body) {
    std::vector<std::string_view> lines;
    lines.reserve(body.size() / 40 + 16);
    size_t i = 0;
    while (i <= body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        lines.emplace_back(body.data() + i, e - i);
        if (e == body.size()) break;
        i = e + 1;
    }
    auto is_link_rail = [](std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#') return false;
        size_t link_open = line.find('[');
        if (link_open == std::string_view::npos) return false;
        size_t link_close = line.find("](", link_open);
        if (link_close == std::string_view::npos) return false;
        size_t url_close = line.find(')', link_close + 2);
        if (url_close == std::string_view::npos) return false;
        std::string outside;
        size_t k = 0;
        bool in_text = false, in_url = false;
        while (k < line.size()) {
            char c = line[k];
            if (!in_text && !in_url && c == '[') { in_text = true; ++k; continue; }
            if (in_text && c == ']' && k + 1 < line.size() && line[k+1] == '(') {
                in_text = false; in_url = true; k += 2; continue;
            }
            if (in_url && c == ')') { in_url = false; ++k; continue; }
            if (!in_text && !in_url) outside.push_back(c);
            ++k;
        }
        int prose_chars = 0;
        for (char c : outside) {
            if (c == ' ' || c == '\t' || c == '|' || c == '\xe2'
                || c == '\xa0' || c == '*' || c == '-' || c == '\xc2')
                continue;
            ++prose_chars;
        }
        return prose_chars <= 8;
    };
    std::vector<bool> rail(lines.size(), false);
    for (size_t k = 0; k < lines.size(); ++k) rail[k] = is_link_rail(lines[k]);
    std::string out;
    out.reserve(body.size());
    for (size_t k = 0; k < lines.size(); ) {
        if (!rail[k]) {
            out.append(lines[k].data(), lines[k].size());
            out.push_back('\n');
            ++k;
            continue;
        }
        size_t run_end = k;
        while (run_end < lines.size()) {
            bool empty = true;
            for (char c : lines[run_end])
                if (c != ' ' && c != '\t') { empty = false; break; }
            if (!rail[run_end] && !empty) break;
            ++run_end;
        }
        size_t rail_count = 0;
        for (size_t j = k; j < run_end; ++j) if (rail[j]) ++rail_count;
        if (rail_count >= 3) {
            out.append("\n[\xe2\x80\xa6 ");
            out.append(std::to_string(rail_count));
            out.append(" link-rail lines elided]\n\n");
            k = run_end;
        } else {
            for (size_t j = k; j < run_end; ++j) {
                out.append(lines[j].data(), lines[j].size());
                out.push_back('\n');
            }
            k = run_end;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string slugify(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool last_dash = false;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') uc = uc + 32;
        bool alnum = (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9');
        if (alnum) { out.push_back(static_cast<char>(uc)); last_dash = false; }
        else if (!last_dash && !out.empty()) { out.push_back('-'); last_dash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string clip_to_fragment(std::string body, std::string_view fragment) {
    if (fragment.empty()) return body;
    std::string want = slugify(fragment);
    if (want.empty()) return body;
    size_t pos = 0;
    size_t found = std::string::npos;
    int found_level = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line{body.data() + pos, eol - pos};
        if (!line.empty() && line.front() == '#') {
            int level = 0;
            size_t k = 0;
            while (k < line.size() && line[k] == '#' && level < 6) { ++level; ++k; }
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            std::string_view text = line.substr(k);
            std::string slug = slugify(text);
            if (slug == want || (slug.find(want) != std::string::npos
                                 && want.size() >= 4)) {
                found = pos;
                found_level = level;
                break;
            }
        }
        if (eol == body.size()) break;
        pos = eol + 1;
    }
    if (found == std::string::npos) return body;
    size_t scan = body.find('\n', found);
    if (scan == std::string::npos) return body.substr(found);
    ++scan;
    size_t end = body.size();
    while (scan < body.size()) {
        size_t eol = body.find('\n', scan);
        if (eol == std::string::npos) eol = body.size();
        std::string_view line{body.data() + scan, eol - scan};
        if (!line.empty() && line.front() == '#') {
            int level = 0;
            size_t k = 0;
            while (k < line.size() && line[k] == '#' && level < 6) { ++level; ++k; }
            if (level > 0 && level <= found_level) {
                end = scan;
                break;
            }
        }
        if (eol == body.size()) break;
        scan = eol + 1;
    }
    return body.substr(found, end - found);
}

[[nodiscard]] bool looks_like_spa_shell(std::string_view cleaned,
                                         std::string_view raw) {
    auto count_occurrences = [](std::string_view hay, std::string_view needle) {
        size_t n = 0, p = 0;
        while ((p = hay.find(needle, p)) != std::string_view::npos) {
            ++n; p += needle.size();
        }
        return n;
    };
    if (cleaned.size() < 600 && raw.size() > 50'000) return true;
    if (count_occurrences(cleaned, "Loading\xe2\x80\xa6") >= 3) return true;
    if (count_occurrences(cleaned, "Loading...") >= 3) return true;
    if (cleaned.size() < 2000 && count_occurrences(raw, "<script") >= 8)
        return true;
    return false;
}

[[nodiscard]] bool jina_enabled_globally() {
    const char* env = std::getenv("AGENTTY_NO_JINA");
    return !(env && env[0] != '\0' && env[0] != '0');
}

ExecResult run_web_fetch(HttpClient& client, const WebFetchArgs& a) {
    auto pu = parse_url(a.url);
    if (!pu) return std::unexpected(
        ToolError::invalid_args("could not parse url: " + a.url + " (" + pu.error() + ")"));
    if (is_blocked_host(pu->host))
        return std::unexpected(ToolError::invalid_args(
            "web_fetch refused: '" + pu->host + "' is a loopback, private, "
            "or link-local/metadata address. Fetching internal endpoints is "
            "blocked (SSRF protection)."));
    std::string fragment = pu->fragment;

    // The injected client follows redirects + handles TLS. Send the URL
    // with the #fragment stripped (servers ignore it; we clip locally).
    std::string wire_url = "https://" + pu->host;
    if (pu->port != 443) wire_url += ":" + std::to_string(pu->port);
    wire_url += pu->path;

    std::vector<std::pair<std::string,std::string>> headers;
    headers.push_back({"user-agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
    headers.push_back({"accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,"
        "text/plain;q=0.8,*/*;q=0.7"});
    headers.push_back({"accept-language", "en-US,en;q=0.9"});
    for (const auto& [k, v] : a.headers) headers.push_back({k, v});

    auto resp = http_send(client, a.method, wire_url, headers);
    if (!resp.ok)
        return std::unexpected(ToolError::network("fetch failed: " + resp.err));

    std::string raw_body = std::move(resp.body);
    bool html_in = looks_like_html(resp.content_type, raw_body);
    size_t raw_size = raw_body.size();

    std::string body;
    if (html_in) {
        body = html_to_text(raw_body);
        body = drop_link_rail_runs(std::move(body));
    } else {
        body = raw_body;
    }

    bool used_jina = false;
    if (html_in && a.allow_jina && jina_enabled_globally()
        && looks_like_spa_shell(body, raw_body)
        && a.method == "GET")
    {
        std::vector<std::pair<std::string,std::string>> jh;
        jh.push_back({"user-agent",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
        jh.push_back({"accept", "text/plain, text/markdown, */*"});
        jh.push_back({"x-return-format", "markdown"});
        auto jr = http_send(client, "GET", "https://r.jina.ai/" + a.url, jh);
        if (jr.ok && jr.status >= 200 && jr.status < 300 && !jr.body.empty()) {
            std::string jbody = drop_link_rail_runs(std::move(jr.body));
            if (jbody.size() > body.size() + 200) {
                body = std::move(jbody);
                used_jina = true;
            }
        }
    }

    if (!fragment.empty()) {
        std::string clipped = clip_to_fragment(body, fragment);
        if (clipped.size() >= 200 && clipped.size() < body.size() * 9 / 10)
            body = std::move(clipped);
    }

    bool truncated = false;
    if (body.size() > kMaxFetchBytes) {
        body.resize(util::safe_utf8_cut(body, kMaxFetchBytes));
        truncated = true;
    }
    body = util::to_valid_utf8(std::move(body));

    std::ostringstream out;
    out << "HTTP " << resp.status;
    if (!resp.content_type.empty()) out << " (" << resp.content_type << ")";
    if (used_jina) out << " [via r.jina.ai: SPA fallback]";
    if (!fragment.empty()) out << " [#" << fragment << "]";
    out << "\n\n" << body;
    if (html_in && !used_jina) {
        out << "\n[extracted text from " << raw_size << " bytes of HTML";
        if (truncated) out << ", truncated at 64KB";
        out << "]";
    } else if (truncated) {
        out << "\n[body truncated at 64KB]";
    }
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

// ═══════════════════════════════════════════════════════════════════════
//  web_search
// ═══════════════════════════════════════════════════════════════════════

struct WebSearchArgs {
    std::string query;
    int count;
    std::string display_description;
};

std::expected<WebSearchArgs, ToolError> parse_web_search_args(const json& j) {
    util::ArgReader ar(j);
    auto q_opt = ar.require_str("query");
    if (!q_opt)
        return std::unexpected(ToolError::invalid_args("query required"));
    return WebSearchArgs{
        *std::move(q_opt),
        ar.integer("count", 10),
        ar.str("display_description", ""),
    };
}

std::string url_escape(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
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

std::string url_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i+1]), lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

void clean_text(std::string& s) {
    struct E { std::string_view from; char to; };
    static constexpr E table[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&#x27;", '\''},
        {"&apos;", '\''}, {"&nbsp;", ' '},
    };
    for (auto e : table) {
        size_t p = 0;
        while ((p = s.find(e.from, p)) != std::string::npos) {
            s.replace(p, e.from.size(), 1, e.to);
            p += 1;
        }
    }
    std::string no_tags;
    no_tags.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) no_tags += c;
    }
    std::string out;
    out.reserve(no_tags.size());
    bool ws = false;
    for (char c : no_tags) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!out.empty() && !ws) { out += ' '; ws = true; }
        } else {
            out += c; ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(0, 1);
    s = std::move(out);
}

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

std::string_view get_attr(std::string_view tag, std::string_view name) {
    for (size_t p = 0; p + name.size() + 2 < tag.size(); ++p) {
        if (p > 0 && tag[p-1] != ' ' && tag[p-1] != '\t' && tag[p-1] != '\n') continue;
        if (tag.compare(p, name.size(), name) != 0) continue;
        size_t after = p + name.size();
        if (after >= tag.size() || tag[after] != '=') continue;
        size_t v = after + 1;
        if (v >= tag.size()) return {};
        char q = tag[v];
        if (q == '"' || q == '\'') {
            size_t ve = tag.find(q, v + 1);
            if (ve == std::string_view::npos) return {};
            return tag.substr(v + 1, ve - v - 1);
        }
        size_t ve = v;
        while (ve < tag.size() && tag[ve] != ' ' && tag[ve] != '>'
               && tag[ve] != '\t')
            ++ve;
        return tag.substr(v, ve - v);
    }
    return {};
}

[[nodiscard]] std::string unwrap_ddg_link(std::string_view href) {
    if (href.find("/y.js") != std::string_view::npos
        || href.find("y.js?") != std::string_view::npos) {
        return {};
    }
    auto p = href.find("uddg=");
    if (p == std::string_view::npos) return std::string{href};
    std::string_view enc = href.substr(p + 5);
    auto amp = enc.find('&');
    if (amp != std::string_view::npos) enc = enc.substr(0, amp);
    return url_unescape(enc);
}

std::vector<SearchHit> parse_ddg(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        auto title_start = body.find("class=\"result__a\"", pos);
        if (title_start == std::string::npos) break;
        std::string link;
        size_t a_open = body.rfind("<a ", title_start);
        if (a_open != std::string::npos && title_start - a_open < 512) {
            size_t te = body.find('>', a_open);
            if (te != std::string::npos && te >= title_start) {
                std::string_view tag_body{body.data() + a_open, te - a_open};
                auto v = get_attr(tag_body, "href");
                if (!v.empty()) link = unwrap_ddg_link(v);
            }
        }
        auto tag_end = body.find('>', title_start);
        if (tag_end == std::string::npos) break;
        auto text_end = body.find('<', tag_end + 1);
        std::string title;
        if (text_end != std::string::npos)
            title = body.substr(tag_end + 1, text_end - tag_end - 1);
        auto snippet_start = body.find("class=\"result__snippet\"", text_end);
        std::string snippet;
        if (snippet_start != std::string::npos) {
            auto stag = body.find('>', snippet_start);
            if (stag != std::string::npos) {
                auto send = body.find("</a>", stag);
                if (send != std::string::npos)
                    snippet = body.substr(stag + 1, send - stag - 1);
            }
            pos = snippet_start + 10;
        } else {
            pos = text_end ? text_end + 1 : body.size();
        }
        clean_text(title);
        clean_text(snippet);
        if (!title.empty() && !link.empty()) {
            hits.push_back({std::move(title), std::move(link), std::move(snippet)});
        }
    }
    return hits;
}

std::vector<SearchHit> parse_brave(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        size_t blk = body.find("class=\"snippet ", pos);
        if (blk == std::string::npos) break;
        size_t div_open = body.rfind("<div ", blk);
        if (div_open == std::string::npos) break;
        size_t blk_end = body.find("class=\"snippet ", blk + 16);
        if (blk_end == std::string::npos) blk_end = body.size();
        if (blk_end < body.size()) {
            size_t next_div = body.rfind("<div ", blk_end);
            if (next_div > div_open) blk_end = next_div;
        }
        std::string_view block{body.data() + div_open, blk_end - div_open};
        SearchHit h;
        size_t a = block.find("<a href=\"http");
        if (a == std::string_view::npos) a = block.find("<a href='http");
        if (a != std::string_view::npos) {
            size_t s = a + 9;
            char q = block[s - 1];
            size_t e = block.find(q, s);
            if (e != std::string_view::npos) h.url.assign(block.substr(s, e - s));
        }
        size_t ti = block.find("class=\"title");
        if (ti != std::string_view::npos) {
            size_t tg = block.find('>', ti);
            if (tg != std::string_view::npos) {
                size_t te = block.find('<', tg + 1);
                if (te != std::string_view::npos)
                    h.title.assign(block.substr(tg + 1, te - tg - 1));
            }
        }
        size_t ds = block.find("class=\"generic-snippet");
        if (ds != std::string_view::npos) {
            size_t dg = block.find('>', ds);
            if (dg != std::string_view::npos) {
                size_t de = block.find("</div", dg + 1);
                if (de != std::string_view::npos)
                    h.snippet.assign(block.substr(dg + 1, de - dg - 1));
            }
        }
        if (h.snippet.empty()) {
            size_t ds2 = block.find("snippet-description");
            if (ds2 != std::string_view::npos) {
                size_t dg = block.find('>', ds2);
                if (dg != std::string_view::npos) {
                    size_t de = block.find("</", dg + 1);
                    if (de != std::string_view::npos)
                        h.snippet.assign(block.substr(dg + 1, de - dg - 1));
                }
            }
        }
        clean_text(h.title);
        clean_text(h.snippet);
        if (!h.title.empty() && !h.url.empty()
            && h.url.find("brave.com") == std::string::npos) {
            hits.push_back(std::move(h));
        }
        pos = blk_end;
    }
    return hits;
}

std::vector<SearchHit> parse_startpage(const std::string& body, int count) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (pos < body.size() && static_cast<int>(hits.size()) < count) {
        size_t ti = body.find("class=\"result-title", pos);
        if (ti == std::string::npos) ti = body.find("class=\"w-gl__result-title", pos);
        if (ti == std::string::npos) break;
        size_t a_open = body.rfind("<a ", ti);
        if (a_open == std::string::npos || ti - a_open > 256) {
            pos = ti + 1;
            continue;
        }
        size_t tag_end = body.find('>', a_open);
        if (tag_end == std::string::npos) break;
        std::string_view tag_body{body.data() + a_open, tag_end - a_open};
        std::string url{get_attr(tag_body, "href")};
        size_t a_close = body.find("</a>", tag_end);
        if (a_close == std::string::npos) { pos = tag_end + 1; continue; }
        std::string title = body.substr(tag_end + 1, a_close - tag_end - 1);
        std::string snippet;
        size_t scan_end = std::min(body.size(), a_close + 2048);
        size_t ds = body.find("description", a_close);
        if (ds != std::string::npos && ds < scan_end) {
            size_t dg = body.find('>', ds);
            if (dg != std::string::npos && dg < scan_end) {
                size_t de = body.find("</", dg + 1);
                if (de != std::string::npos)
                    snippet = body.substr(dg + 1, de - dg - 1);
            }
        }
        clean_text(title);
        clean_text(snippet);
        if (!title.empty() && url.starts_with("http")) {
            hits.push_back({std::move(title), std::move(url), std::move(snippet)});
        }
        pos = a_close + 4;
    }
    return hits;
}

ExecResult run_web_search(HttpClient& client, const WebSearchArgs& a) {
    using Parser = std::vector<SearchHit>(*)(const std::string&, int);
    struct Engine {
        std::string name;
        std::string host;
        std::string path;     // includes ?q=... for GET
        std::string body;     // form body for POST
        bool        post;
        Parser      parse;
    };
    std::string q_esc = url_escape(a.query);
    const Engine chain[] = {
        {"Brave",      "search.brave.com",
         "/search?q=" + q_esc + "&source=web", "",
         false, parse_brave},
        {"DuckDuckGo", "html.duckduckgo.com",
         "/html/",
         "q=" + q_esc + "&kl=us-en",
         true,  parse_ddg},
        {"Startpage",  "www.startpage.com",
         "/sp/search?query=" + q_esc, "",
         false, parse_startpage},
    };

    std::vector<std::pair<std::string, std::string>> diagnostics;
    std::vector<SearchHit> hits;
    std::string used_engine;

    for (const auto& e : chain) {
        std::vector<std::pair<std::string,std::string>> headers;
        headers.push_back({"user-agent",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"});
        headers.push_back({"accept",
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
        headers.push_back({"accept-language", "en-US,en;q=0.9"});
        if (e.post)
            headers.push_back({"content-type", "application/x-www-form-urlencoded"});
        headers.push_back({"referer", std::string{"https://"} + e.host + "/"});

        std::string url = "https://" + e.host + e.path;
        auto resp = http_send(client, e.post ? "POST" : "GET", url, headers, e.body);
        if (!resp.ok) {
            diagnostics.emplace_back(e.name, "transport: " + resp.err);
            continue;
        }
        if (resp.status >= 400) {
            diagnostics.emplace_back(e.name,
                "HTTP " + std::to_string(resp.status));
            continue;
        }
        auto parsed = e.parse(resp.body, a.count);
        if (parsed.empty()) {
            diagnostics.emplace_back(e.name,
                "parser found 0 results (" + std::to_string(resp.body.size())
                + " bytes; layout may have changed)");
            continue;
        }
        hits = std::move(parsed);
        used_engine = e.name;
        break;
    }

    auto canon_key = [](std::string_view url) -> std::string {
        for (auto prefix : {"https://", "http://"}) {
            std::string_view p{prefix};
            if (url.size() >= p.size()
                && std::equal(p.begin(), p.end(), url.begin(),
                              [](char x, char y){
                                  return std::tolower(x) == std::tolower(y);
                              }))
            { url.remove_prefix(p.size()); break; }
        }
        if (auto q = url.find('?'); q != std::string_view::npos) url = url.substr(0, q);
        if (auto h = url.find('#'); h != std::string_view::npos) url = url.substr(0, h);
        std::string out;
        out.reserve(url.size());
        auto slash = url.find('/');
        std::string_view host = url.substr(0, slash);
        for (auto pfx : {"www.", "m.", "amp."}) {
            std::string_view p{pfx};
            if (host.size() > p.size()
                && std::equal(p.begin(), p.end(), host.begin(),
                              [](char x, char y){
                                  return std::tolower(x) == std::tolower(y);
                              }))
            { host.remove_prefix(p.size()); break; }
        }
        for (char c : host) out.push_back(static_cast<char>(std::tolower(c)));
        if (slash != std::string_view::npos) out.append(url.substr(slash));
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out;
    };
    {
        std::vector<SearchHit> uniq;
        uniq.reserve(hits.size());
        std::vector<std::string> seen;
        seen.reserve(hits.size());
        for (auto& h : hits) {
            std::string k = canon_key(h.url);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
            seen.push_back(std::move(k));
            uniq.push_back(std::move(h));
        }
        hits = std::move(uniq);
    }

    if (hits.empty()) {
        std::ostringstream err;
        err << "search returned no results for: " << a.query << "\n"
            << "tried " << diagnostics.size() << " engines:";
        for (const auto& [name, why] : diagnostics)
            err << "\n  - " << name << ": " << why;
        return ToolOutput{err.str(), std::nullopt};
    }

    std::ostringstream out;
    out << "[via " << used_engine << "]\n\n";
    int i = 1;
    for (const auto& h : hits) {
        out << i++ << ". " << h.title << "\n";
        out << "   " << h.url << "\n";
        if (!h.snippet.empty()) out << "   " << h.snippet << "\n";
        out << "\n";
    }
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

// ── Schemas ────────────────────────────────────────────────────────────

json web_fetch_schema() {
    return json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"url",     {{"type","string"}, {"description","The URL to fetch (https only)"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
}

json web_search_schema() {
    return json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"query", {{"type","string"}, {"description","Search query"}}},
            {"count", {{"type","integer"}, {"description","Max results (default: 10)"}}},
        }},
    };
}

} // namespace

void register_web_tools(Shells& sh, const std::shared_ptr<HttpClient>& http) {
    if (!http) return;   // no transport → no web tools

    sh.add("web_fetch",
        "Fetch a URL (HTTPS, follows redirects). For HTML pages, returns "
        "extracted plain text with links preserved as [text](url) — not raw "
        "HTML. Up to 64KB of cleaned content. Use for docs, articles, APIs. "
        "Supports #fragment to clip the output to that section. "
        "JS/SPA pages are auto-rendered via r.jina.ai when the primary "
        "fetch returns a near-empty shell (disable per-request by sending "
        "a `x-no-jina: 1` header, or globally by setting AGENTTY_NO_JINA=1).",
        web_fetch_schema(), EffectSet{Effect::Net},
        [http](const json& args) -> mcp::cap::Result {
            auto parsed = parse_web_fetch_args(args);
            if (!parsed) return mcp::cap::Result::error(parsed.error().render());
            return lower(run_web_fetch(*http, *parsed));
        }, 20'000);

    sh.add("web_search",
        "Search the web. Tries Brave → DuckDuckGo → Startpage until one "
        "returns results. Output is `[via ENGINE]` followed by numbered "
        "title / URL / snippet for each hit. Use web_fetch on a result URL "
        "to read the page. Standard search operators work in the query: "
        "`site:docs.python.org`, `\"exact phrase\"`, `-exclude`. Duplicate "
        "URLs (AMP / mobile / archive mirrors) are auto-deduped.",
        web_search_schema(), EffectSet{Effect::Net},
        [http](const json& args) -> mcp::cap::Result {
            auto parsed = parse_web_search_args(args);
            if (!parsed) return mcp::cap::Result::error(parsed.error().render());
            return lower(run_web_search(*http, *parsed));
        }, 0);
}

} // namespace mcp::tools::detail
