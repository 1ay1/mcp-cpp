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

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#  include <process.h>   // _getpid
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

int main() {
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
    std::puts("ALL FS TOOL TESTS PASSED");
    return 0;
}
