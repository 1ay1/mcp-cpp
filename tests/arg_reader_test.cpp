// SPDX-License-Identifier: Apache-2.0
//
// arg_reader_test — the single robust arg-reading layer every tool path flows
// through (native tool_calls, salvaged leaked calls, JSON-protocol, MCP-server
// tools, the stdio MCP server). Locks in two robustness guarantees against
// weak local models:
//
//   1. VALUE-TYPE COERCION — "42" in an int slot, an int in a bool slot,
//      "true"/"yes" strings as bools, a non-string dumped, a string array
//      newline-joined. A model that gets the type wrong still drives the tool.
//   2. KEY ALIASING — the canonical key a tool reads is found even when the
//      model parked the value under a synonym (cmd→command, file→path,
//      query→pattern, uri→url, old_text→old_string, …). One robust table,
//      not per-call-site patching.
//
// Run: build mcp_arg_reader_test, execute. Exit 0 = pass.

#include <mcp/tools/util/arg_reader.hpp>

#include <cstdio>
#include <string>

using mcp::tools::util::ArgReader;
using nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── 1. integer() coercion ───────────────────────────────────────────────────
static void test_integer_coercion() {
    CHECK(ArgReader(json{{"offset", 7}}).integer("offset", 1) == 7);          // native int
    CHECK(ArgReader(json{{"offset", "7"}}).integer("offset", 1) == 7);        // "7" string → 7
    CHECK(ArgReader(json{{"offset", 7.9}}).integer("offset", 1) == 7);        // float → truncated
    CHECK(ArgReader(json{{"offset", "nope"}}).integer("offset", 1) == 1);     // garbage → default
    CHECK(ArgReader(json{{"offset", nullptr}}).integer("offset", 1) == 1);    // null → default
    CHECK(ArgReader(json::object()).integer("offset", 1) == 1);               // missing → default
}

// ── 2. boolean() coercion ───────────────────────────────────────────────────
static void test_boolean_coercion() {
    CHECK(ArgReader(json{{"replace_all", true}}).boolean("replace_all", false) == true);
    CHECK(ArgReader(json{{"replace_all", "true"}}).boolean("replace_all", false) == true);
    CHECK(ArgReader(json{{"replace_all", "True"}}).boolean("replace_all", false) == true); // case-insens
    CHECK(ArgReader(json{{"replace_all", "yes"}}).boolean("replace_all", false) == true);
    CHECK(ArgReader(json{{"replace_all", "1"}}).boolean("replace_all", false) == true);
    CHECK(ArgReader(json{{"replace_all", 1}}).boolean("replace_all", false) == true);   // int → bool
    CHECK(ArgReader(json{{"replace_all", "false"}}).boolean("replace_all", true) == false);
    CHECK(ArgReader(json{{"replace_all", "no"}}).boolean("replace_all", true) == false);
    CHECK(ArgReader(json{{"replace_all", 0}}).boolean("replace_all", true) == false);
    CHECK(ArgReader(json::object()).boolean("replace_all", true) == true);              // default
}

// ── 3. str() coercion (array-join, non-string dump) ──────────────────────────
static void test_str_coercion() {
    CHECK(ArgReader(json{{"content", "hi"}}).str("content") == "hi");
    // Array of strings → newline-joined (weak model split a command/body).
    CHECK(ArgReader(json{{"content", json::array({"a", "b", "c"})}}).str("content")
          == "a\nb\nc");
    // A number in a string slot is dumped, not dropped.
    CHECK(ArgReader(json{{"content", 42}}).str("content") == "42");
    // Missing → default.
    CHECK(ArgReader(json::object()).str("content", "def") == "def");
}

// ── 4. key aliasing (the widened weak-model vocabulary) ──────────────────────
static void test_command_aliases() {
    // bash: cmd / shell / script / run / cmdline / shell_command → command
    for (const char* k : {"cmd", "shell", "script", "run", "cmdline", "shell_command"}) {
        json j; j[k] = "ls -la";
        CHECK(ArgReader(j).str("command") == "ls -la");
    }
    // Exact key still wins when both present.
    CHECK(ArgReader(json{{"command", "right"}, {"cmd", "wrong"}}).str("command") == "right");
}

static void test_path_aliases() {
    for (const char* k : {"file", "filepath", "filename", "file_path",
                          "dir", "directory", "target", "pathname"}) {
        json j; j[k] = "/tmp/x";
        CHECK(ArgReader(j).str("path") == "/tmp/x");
    }
    // file_path canonical accepts path/file/target.
    CHECK(ArgReader(json{{"path", "/a"}}).str("file_path") == "/a");
    CHECK(ArgReader(json{{"file", "/b"}}).str("file_path") == "/b");
}

static void test_edit_aliases() {
    // old_string canonical ← old_text / old / search / find / from
    for (const char* k : {"old_text", "old", "search", "find", "from", "old_str"}) {
        json j; j[k] = "needle";
        CHECK(ArgReader(j).str("old_string") == "needle");
    }
    for (const char* k : {"new_text", "new", "replace", "replacement", "to", "new_str"}) {
        json j; j[k] = "hay";
        CHECK(ArgReader(j).str("new_string") == "hay");
    }
}

static void test_search_aliases() {
    // grep/glob pattern ← query / q / regex / search / term / glob / match / pat
    for (const char* k : {"query", "q", "regex", "search", "term", "glob", "match", "pat"}) {
        json j; j[k] = "foo.*";
        CHECK(ArgReader(j).str("pattern") == "foo.*");
    }
    // web_search/search_docs query ← q / search / term / text / prompt / question
    for (const char* k : {"q", "search", "term", "text", "prompt", "question"}) {
        json j; j[k] = "how to";
        CHECK(ArgReader(j).str("query") == "how to");
    }
    // web_fetch url ← uri / link / address / href
    for (const char* k : {"uri", "link", "address", "href"}) {
        json j; j[k] = "https://x";
        CHECK(ArgReader(j).str("url") == "https://x");
    }
}

// ── 5. coercion + aliasing COMPOSE (the real weak-model failure) ─────────────
static void test_alias_and_coercion_compose() {
    // read with offset under the `start_line` alias AND as a string.
    CHECK(ArgReader(json{{"start_line", "12"}}).integer("offset", 1) == 12);
    // bash command split into an array under the `cmd` alias.
    CHECK(ArgReader(json{{"cmd", json::array({"echo", "hi"})}}).str("command")
          == "echo\nhi");
}

int main() {
    test_integer_coercion();
    test_boolean_coercion();
    test_str_coercion();
    test_command_aliases();
    test_path_aliases();
    test_edit_aliases();
    test_search_aliases();
    test_alias_and_coercion_compose();

    if (g_failures == 0) {
        std::printf("arg_reader_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "arg_reader_test: %d check(s) failed\n", g_failures);
    return 1;
}
