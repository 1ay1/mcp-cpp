// shell_bound_test — head_lines / tail_lines bound what reaches the MODEL
// without bounding what the user's terminal card shows.
//
// Why this exists: models pipe `| head -20` / `| tail -20` to keep output
// out of their context. That filters at the wrong layer — the pipe discards
// the rest before the card ever sees it, so the HUMAN loses output they were
// watching in order to save the model's context. These parameters do the
// slicing at the boundary instead.
//
// Drives the REAL provider, not the helper, so the schema/parse/apply path
// is covered end to end.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace mcp::tools;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    auto root = fs::temp_directory_path() / ("mcp_shell_bound_" + std::to_string(::getpid()));
    fs::create_directories(root);
    util::set_workspace_root(root);

    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    auto run = [&](mcp::Json a) {
        return provider->execute(mcp::cap::Request{"shell", std::move(a)});
    };
    // 30 lines: line1 … line30.
    const char* seq = "i=1; while [ $i -le 30 ]; do echo line$i; i=$((i+1)); done";

    // ── tail_lines keeps the END, which is where a build failure lives ──
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["tail_lines"] = 3;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line30"));
        CHECK(has(r.text, "line28"));
        CHECK(!has(r.text, "line1\n"));   // the head is gone
        CHECK(!has(r.text, "line15"));
        // The elision is HONEST about how much it dropped — a silent slice
        // would let the model conclude the command only printed 3 lines.
        CHECK(has(r.text, "27 lines elided"));
        // …and says the user still has it, so the model doesn't re-run the
        // command unbounded just to "show" the user something they can see.
        CHECK(has(r.text, "terminal card"));
    }

    // ── head_lines keeps the START ──────────────────────────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["head_lines"] = 2;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line2"));
        CHECK(!has(r.text, "line30"));
        CHECK(has(r.text, "28 lines elided"));
    }

    // ── both ends, middle elided (the build-log shape) ──────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["head_lines"] = 2;
        a["tail_lines"] = 2;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line30"));
        CHECK(!has(r.text, "line15"));
        CHECK(has(r.text, "26 lines elided"));
    }

    // ── a bound LARGER than the output elides nothing ───────────────────
    // Reporting "0 lines elided" on complete output would be a lie of
    // emphasis; the text must come back untouched.
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "echo a; echo b; echo c";
        a["tail_lines"] = 50;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "a"));
        CHECK(has(r.text, "c"));
        CHECK(!has(r.text, "elided"));
    }

    // ── absent parameters change nothing ────────────────────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line30"));
        CHECK(!has(r.text, "elided"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::puts("shell_bound_test: OK");
    return g_failures == 0 ? 0 : 1;
}
