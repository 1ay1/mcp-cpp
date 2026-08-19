// SPDX-License-Identifier: Apache-2.0
//
// fs_tools_test.cpp — exercises the Tier-1 filesystem tools (read / write /
// edit / list_dir) end-to-end through make_provider(), proving the ported
// bodies behave and that write/edit carry a FileChange back via the meta.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#  include <process.h>   // _getpid
#  include <io.h>        // _chsize_s, _fileno
#  define mcp_getpid _getpid
#else
#  include <unistd.h>    // getpid
#  define mcp_getpid getpid
#endif

using namespace mcp::tools;
namespace fs = std::filesystem;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}

static mcp::Json obj() { return mcp::Json::object(); }

TEST_CASE("fs_tools") {
    auto root = fs::temp_directory_path() / ("mcp_fs_test_" + std::to_string(mcp_getpid()));
    fs::create_directories(root);
    util::set_workspace_root(root);

    HostServices svc;  // no host backends needed — Tier-1 is self-contained
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── write creates a file and carries a FileChange ────────────────────
    auto wpath = (root / "hello.txt").string();
    {
        auto args = obj();
        args["file_path"] = wpath;
        args["content"]   = "line1\nline2\nline3\n";
        auto wr = call(*provider, "write", args);
        assert(!wr.is_error);
        auto change = read_change(wr);
        assert(change.has_value());
        assert(change->path == wpath);
        assert(change->added == 3);
        assert(change->after == "line1\nline2\nline3\n");
        assert(read_effects(wr).has(Effect::WriteFs));
        std::puts("write: ok (FileChange carried, 3 added)");
    }

    // ── read returns the content ─────────────────────────────────────────
    {
        auto args = obj(); args["path"] = wpath;
        auto rd = call(*provider, "read", args);
        assert(!rd.is_error);
        assert(rd.text.find("line1") != std::string::npos);
        assert(rd.text.find("line3") != std::string::npos);
        assert(read_effects(rd).has(Effect::ReadFs));
        std::puts("read: ok");
    }

    // ── read again (unchanged) returns the stale-read sentinel ───────────
    {
        auto args = obj(); args["path"] = wpath;
        auto rd2 = call(*provider, "read", args);
        assert(!rd2.is_error);
        assert(rd2.text.find("File unchanged since last read") != std::string::npos);
        std::puts("read: stale-read sentinel ok");
    }

    // ── read past EOF gives a clear message, not a broken range ──────────
    {
        auto args = obj(); args["path"] = wpath;
        args["offset"] = 1000;   // file has 3 lines
        auto rd = call(*provider, "read", args);
        assert(!rd.is_error);
        // The old code emitted a nonsensical inverted range like
        // "[showing lines 1000-999 of 3]". Assert the actionable message and
        // the ABSENCE of that inverted form.
        assert(rd.text.find("past the end of the file") != std::string::npos);
        assert(rd.text.find("1000-999") == std::string::npos);
        std::puts("read: past-EOF offset gives a clear message");
    }

    // ── read symbol= returns just the named function's body (sed-killer) ──
    {
        auto code = (root / "lib.cpp").string();
        auto wargs = obj();
        wargs["file_path"] = code;
        wargs["content"] =
            "#include <cstdio>\n"                       // L1
            "int helper(int a) {\n"                     // L2
            "    return a + 1;\n"                        // L3
            "}\n"                                       // L4
            "int compute(int x, int y) {\n"             // L5  <-- target
            "    int t = helper(x);\n"                   // L6
            "    return t * y;\n"                        // L7
            "}\n"                                       // L8
            "int main() { return compute(2, 3); }\n";   // L9
        call(*provider, "write", wargs);

        auto args = obj();
        args["path"]   = code;
        args["symbol"] = "compute";
        auto rd = call(*provider, "read", args);
        assert(!rd.is_error);
        // Header names the location.
        assert(rd.text.find("`compute` defined at") != std::string::npos);
        // Body of compute is present…
        assert(rd.text.find("int compute(int x, int y)") != std::string::npos);
        assert(rd.text.find("return t * y;") != std::string::npos);
        // …and the UNRELATED helper() body is NOT (we scoped to one symbol).
        assert(rd.text.find("return a + 1;") == std::string::npos);
        std::puts("read: symbol= returns just that function's body");
    }

    // ── read symbol= for a missing symbol errors clearly ───────────────
    {
        auto args = obj();
        args["path"]   = (root / "lib.cpp").string();
        args["symbol"] = "no_such_symbol";
        auto rd = call(*provider, "read", args);
        assert(rd.is_error);
        assert(rd.text.find("no definition of") != std::string::npos);
        std::puts("read: symbol= missing symbol errors cleanly");
    }

    // ── read symbol= is INDENT-scoped for Python (braces in the body, e.g.
    //    a dict, must NOT close the block early) ─────────────────────────
    {
        auto py = (root / "mod.py").string();
        auto wargs = obj();
        wargs["file_path"] = py;
        wargs["content"] =
            "def build():\n"            // L1  <-- target
            "    config = {\n"          // L2  multi-line dict opens a brace
            "        'a': 1,\n"         // L3
            "    }\n"                    // L4  dict close — must NOT end the def
            "    return config\n"        // L5  <-- must be included
            "def sibling():\n"          // L6  <-- must NOT be included
            "    return 0\n";           // L7
        call(*provider, "write", wargs);

        auto args = obj();
        args["path"]   = py;
        args["symbol"] = "build";
        auto rd = call(*provider, "read", args);
        assert(!rd.is_error);
        assert(rd.text.find("def build():") != std::string::npos);
        assert(rd.text.find("return config") != std::string::npos);  // full body
        assert(rd.text.find("def sibling") == std::string::npos);    // stops at next def
        std::puts("read: symbol= indent-scoped for Python (dict braces ignored)");
    }

    // ── read symbol= handles C++ raw strings (a `"` or `}` inside R"(…)" must
    //    not mis-balance the brace scan) ───────────────────────────────
    {
        auto cc = (root / "raw.cpp").string();
        auto wargs = obj();
        wargs["file_path"] = cc;
        wargs["content"] =
            "int f() {\n"                                    // L1
            "    auto re = R\"(a \" quote and } brace)\";\n"  // L2 traps: quote+brace in raw string
            "    return 1;\n"                                // L3 must be included
            "}\n"                                            // L4
            "int g() { return 2; }\n";                       // L5 must NOT be included
        call(*provider, "write", wargs);

        auto args = obj();
        args["path"]   = cc;
        args["symbol"] = "f";
        auto rd = call(*provider, "read", args);
        assert(!rd.is_error);
        assert(rd.text.find("return 1;") != std::string::npos);   // full body
        assert(rd.text.find("int g()") == std::string::npos);     // didn't over-capture
        std::puts("read: symbol= handles C++ raw strings ok");
    }

    // ── edit applies a fuzzy splice and carries a FileChange ─────────────
    {
        auto e = obj();
        e["old_text"] = "line2";
        e["new_text"] = "LINE-TWO";
        auto args = obj();
        args["path"] = wpath;
        args["edits"] = mcp::Json::array({e});
        auto ed = call(*provider, "edit", args);
        assert(!ed.is_error);
        assert(ed.text.find("Edited") != std::string::npos);
        auto echange = read_change(ed);
        assert(echange.has_value());
        assert(echange->after.find("LINE-TWO") != std::string::npos);
        assert(echange->before.find("line2") != std::string::npos);
        std::puts("edit: ok (FileChange carried)");
    }

    // confirm the on-disk file actually changed
    {
        std::FILE* f = std::fopen(wpath.c_str(), "rb");
        assert(f);
        std::string buf; char tmp[256]; size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
        std::fclose(f);
        assert(buf.find("LINE-TWO") != std::string::npos);
        assert(buf.find("line2") == std::string::npos);
        std::puts("edit: on-disk content updated");
    }

    // ── edit ambiguity surfaces an error ─────────────────────────────────
    {
        auto wp2 = (root / "dup.txt").string();
        auto wargs = obj(); wargs["file_path"] = wp2; wargs["content"] = "x\nx\ny\n";
        call(*provider, "write", wargs);
        auto e = obj(); e["old_text"] = "x"; e["new_text"] = "z";
        auto args = obj(); args["path"] = wp2; args["edits"] = mcp::Json::array({e});
        auto bad = call(*provider, "edit", args);
        assert(bad.is_error);
        assert(bad.text.find("appears") != std::string::npos);
        std::puts("edit: ambiguous match errors correctly");
    }

    // ── oversized sparse target is rejected before whole-file loading ─────
    {
        auto large_path = (root / "large-sparse.txt").string();
        std::FILE* f = std::fopen(large_path.c_str(), "wb");
        assert(f);
#ifdef _WIN32
        assert(_chsize_s(_fileno(f), 2u * 1024u * 1024u) == 0);
#else
        assert(ftruncate(fileno(f), 2u * 1024u * 1024u) == 0);
#endif
        std::fclose(f);

        auto e = obj(); e["old_text"] = "needle"; e["new_text"] = "replacement";
        auto args = obj(); args["path"] = large_path; args["edits"] = mcp::Json::array({e});
        auto too_large = call(*provider, "edit", args);
        assert(too_large.is_error);
        assert(too_large.text.find("1 MiB edit cap") != std::string::npos);
        assert(too_large.text.find("rejected before reading") != std::string::npos);
        assert(fs::file_size(large_path) == 2u * 1024u * 1024u);
        std::puts("edit: oversized sparse target rejected before read");
    }

    // ── list_dir shows the files ─────────────────────────────────────────
    {
        auto args = obj(); args["path"] = root.string();
        auto ls = call(*provider, "list_dir", args);
        assert(!ls.is_error);
        assert(ls.text.find("hello.txt") != std::string::npos);
        std::puts("list_dir: ok");
    }

    // ── workspace boundary refuses outside paths ─────────────────────────
    {
        auto args = obj(); args["path"] = "/etc/hostname";
        auto esc = call(*provider, "read", args);
        assert(esc.is_error);
        std::puts("workspace boundary: outside path refused");
    }

    fs::remove_all(root);
}
