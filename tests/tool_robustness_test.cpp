// SPDX-License-Identifier: Apache-2.0
//
// tool_robustness_test.cpp — the argument shapes weaker models send to read,
// grep and shell. Each case is one that used to fail in real transcripts.

#include <mcp/tools/native_files.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include "test_state.hpp"
#include "test_exec.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define mcp_getpid _getpid
#else
#  include <unistd.h>
#  define mcp_getpid getpid
#endif

using namespace mcp::tools;
namespace fs = std::filesystem;
using mcp::Json;

namespace {
mcp::cap::Result call(mcp::cap::CapabilityProvider& p, const std::string& name, Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
void write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}
}  // namespace

TEST_CASE("tools tolerate the argument shapes weak models send") {
    auto root = fs::temp_directory_path() / ("mcp_robust_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    std::string body;
    for (int i = 1; i <= 50; ++i) body += "line " + std::to_string(i) + "\n";
    write_file(root / "notes.txt", body);
    write_file(root / "src" / "main.cpp", "int run_turn(int x) {\n    return x;\n}\n// CSI [>1u here\n");

    HostServices svc;
    svc.files = std::make_shared<NativeFileSystem>();
    svc.state = mcp::test::state_at(root);
#if !defined(_WIN32)
    svc.exec = std::make_shared<mcp::test::PopenExec>();
#endif
    auto provider = make_provider(svc, ToolsetConfig{}, "local");
    const std::string notes = (root / "notes.txt").string();

    SUBCASE("read: path with a line suffix") {
        for (const std::string p : {notes + ":10-12", notes + "#L10-L12"}) {
            auto r = call(*provider, "read", Json{{"path", p}});
            CHECK(!r.is_error);
            CHECK(has(r.text, "line 10"));
            CHECK(has(r.text, "line 12"));
            CHECK(!has(r.text, "line 13\n"));
        }
    }
    SUBCASE("read: range as a string or an array") {
        auto r = call(*provider, "read", Json{{"path", notes}, {"lines", "20-21"}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "line 20"));
        CHECK(!has(r.text, "line 22\n"));
        r = call(*provider, "read", Json{{"path", notes}, {"line_range", Json::array({30, 31})}});
        CHECK(has(r.text, "line 30"));
        CHECK(!has(r.text, "line 32\n"));
    }
    SUBCASE("read: backticked path, one-entry paths array") {
        auto r = call(*provider, "read", Json{{"path", "`" + notes + "`"}, {"limit", 2}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "line 1"));
        r = call(*provider, "read", Json{{"paths", Json::array({notes})}, {"limit", 2}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "line 2"));
        r = call(*provider, "read", Json{{"paths", Json::array({notes, notes})}});
        CHECK(r.is_error);
        CHECK(has(r.text, "one read call per file"));
    }
    SUBCASE("read: a directory lists it, a near miss names the file") {
        auto r = call(*provider, "read", Json{{"path", (root / "src").string()}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "main.cpp"));
        r = call(*provider, "read", Json{{"path", (root / "src" / "main.cc").string()}});
        CHECK(r.is_error);
        CHECK(has(r.text, "main.cpp"));
    }

#if !defined(_WIN32)
    SUBCASE("grep: a pattern that isn't a valid regex is searched as text") {
        auto r = call(*provider, "grep", Json{{"pattern", "run_turn("}, {"path", root.string()}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "main.cpp"));
        CHECK(has(r.text, "plain text"));
        r = call(*provider, "grep", Json{{"pattern", "\\\\[>1u|nomatch("}, {"path", root.string()}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "main.cpp"));
    }
    SUBCASE("grep: a pattern across lines works") {
        auto r = call(*provider, "grep", Json{{"pattern", "int x\\) \\{\\n    return"},
                                              {"path", root.string()}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "main.cpp"));
    }
    SUBCASE("grep: path with a line suffix, and a missing path") {
        auto r = call(*provider, "grep", Json{{"pattern", "return"},
                                              {"path", (root / "src" / "main.cpp").string() + ":2"}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "return x"));
        r = call(*provider, "grep", Json{{"pattern", "x"}, {"path", (root / "nope").string()}});
        CHECK(r.is_error);
        CHECK(has(r.text, "path not found"));
    }
    SUBCASE("shell: relative cd, argv arrays, fences and prompts") {
        auto r = call(*provider, "shell", Json{{"command", "pwd"}, {"cd", "src"}});
        CHECK(!r.is_error);
        CHECK(has(r.text, (root / "src").string()));
        r = call(*provider, "shell", Json{{"command", Json::array({"echo", "a b"})}});
        CHECK(has(r.text, "a b"));
        r = call(*provider, "shell", Json{{"command", Json::array({"sh", "-c", "echo x && echo y"})}});
        CHECK(has(r.text, "x\ny"));
        r = call(*provider, "shell", Json{{"command", "```bash\necho fenced\n```"}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "fenced"));
        r = call(*provider, "shell", Json{{"command", "$ echo prompted"}});
        CHECK(has(r.text, "prompted"));
        CHECK(!has(r.text, "command not found"));
    }
#endif
    fs::remove_all(root);
}
