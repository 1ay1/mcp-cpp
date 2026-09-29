// SPDX-License-Identifier: Apache-2.0
//
// tool_bench — time the real tool bodies against a real source tree.
//
// Not a test: nothing here asserts. It calls the SAME make_provider()
// surface an agent drives, so whatever it reports is what a turn actually
// pays. Point it at a checkout:
//
//     mcp_tool_bench /path/to/some/repo
//
// Why this exists: the three biggest wins in the read/outline path were all
// invisible to reasoning and obvious to measurement — a windowed read that
// scanned the whole file, a reserve() sized to the file instead of the
// window, and a content hash computed for a field nothing reads. Each was
// found by running this and asking why a number was large. Keep it working.
//
// Numbers on this machine (12 cores, warm page cache, Release) at the time
// the read/outline work landed, over the agentty tree:
//
//     read, 250-line window of a 898 KiB file ....  0.31 ms  (was 1.66)
//     outline, 187 KiB .cpp ......................  4.59 ms  (was 7.45)
//     grep, literal, whole tree ..................  6.3  ms  (shells to rg)
//     extract, capture group, whole tree ......... 36.6  ms  (std::regex)
//
// extract is the outstanding outlier: see the PERF note in textproc.cpp.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/cap/local.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mcp::tools;

namespace {

mcp::Json obj() { return mcp::Json::object(); }

struct Stat { double best; double med; std::size_t bytes; bool err; };

Stat time_call(mcp::cap::CapabilityProvider& p, const std::string& name,
               mcp::Json args, int reps) {
    std::vector<double> ms;
    std::size_t bytes = 0;
    bool err = false;
    for (int i = 0; i < reps; ++i) {
        // read/outline dedup per (context, file): a repeat returns the
        // sentinel in microseconds and would time nothing at all. Give every
        // rep its own context so each one does the real work.
        if (name == "read" || name == "outline")
            util::set_read_context("bench-" + std::to_string(i));
        const auto t0 = std::chrono::steady_clock::now();
        auto r = p.execute(mcp::cap::Request{name, args});
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        bytes = r.text.size();
        err = r.is_error;
    }
    std::sort(ms.begin(), ms.end());
    return {ms.front(), ms[ms.size() / 2], bytes, err};
}

void row(const char* label, const Stat& s) {
    std::printf("  %-44s best %8.2f ms   med %8.2f ms   out %7zu B%s\n",
                label, s.best, s.med, s.bytes, s.err ? "  [ERR]" : "");
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = argc > 1 ? argv[1] : ".";
    std::error_code ec;
    root = fs::absolute(root, ec);
    if (ec || !fs::is_directory(root, ec)) {
        std::printf("usage: mcp_tool_bench <directory>\n");
        return 2;
    }
    util::set_workspace_root(root);
    HostServices svc;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    std::printf("tool_bench on %s\n", root.string().c_str());

    // Largest source file in the tree — the interesting case for read/outline,
    // since both used to do work proportional to it regardless of the ask.
    fs::path big;
    std::uintmax_t bigsz = 0;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        const auto ext = it->path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;
        const auto s = it->path().string();
        if (s.find("/build") != std::string::npos) continue;
        if (s.find("/.git") != std::string::npos) continue;
        const auto sz = it->file_size(ec);
        if (!ec && sz > bigsz) { bigsz = sz; big = it->path(); }
    }
    if (big.empty()) {
        std::printf("no .cpp/.hpp found under %s\n", root.string().c_str());
        return 1;
    }
    std::printf("largest source file: %s (%ju KiB)\n\n",
                big.string().c_str(), static_cast<std::uintmax_t>(bigsz / 1024));

    std::puts("read ---------------------------------------------------------");
    {
        auto a = obj(); a["path"] = big.string(); a["offset"] = 1; a["limit"] = 250;
        row("250-line window of largest file", time_call(*provider, "read", a, 7));
    }
    {
        // Both windows land at roughly the same cost, and that is expected:
        // after the emit/count split the loop stops emitting early, but the
        // TAIL still has to be walked for total_lines (the footer needs it),
        // and memchr over the remainder dominates both. What regressed
        // before was the EMIT half running to EOF byte-at-a-time; if these
        // two ever climb back toward 1.6 ms, that is what came back.
        auto a = obj(); a["path"] = big.string(); a["offset"] = 1; a["limit"] = 2000;
        row("2000-line window", time_call(*provider, "read", a, 7));
    }
    {
        auto a = obj(); a["path"] = big.string(); a["offset"] = -200;
        row("tail (offset=-200)", time_call(*provider, "read", a, 7));
    }

    std::puts("\noutline ------------------------------------------------------");
    {
        auto a = obj(); a["path"] = big.string();
        row("largest file", time_call(*provider, "outline", a, 7));
    }

    std::puts("\nsearch -------------------------------------------------------");
    {
        auto a = obj(); a["pattern"] = "workspace_root"; a["path"] = root.string();
        row("grep literal, whole tree", time_call(*provider, "grep", a, 5));
    }
    {
        auto a = obj(); a["pattern"] = "std::(unique|shared)_ptr<[A-Za-z_]+>";
        a["path"] = root.string();
        row("grep regex, whole tree", time_call(*provider, "grep", a, 5));
    }
    {
        auto a = obj(); a["pattern"] = "*.cpp"; a["path"] = root.string();
        row("glob *.cpp", time_call(*provider, "glob", a, 5));
    }
    {
        auto a = obj(); a["path"] = root.string(); a["recursive"] = true;
        row("list_dir recursive", time_call(*provider, "list_dir", a, 5));
    }

    std::puts("\ntextproc -----------------------------------------------------");
    {
        // The known outlier — std::regex where grep gets ripgrep.
        auto a = obj(); a["pattern"] = "#include <([^>]+)>"; a["group"] = 1;
        a["path"] = root.string(); a["unique"] = true;
        row("extract includes, unique", time_call(*provider, "extract", a, 3));
    }
    {
        auto a = obj(); a["pattern"] = "TODO"; a["by"] = "file";
        a["path"] = root.string();
        row("aggregate TODO by file", time_call(*provider, "aggregate", a, 3));
    }

    std::puts("\nstructural / map ---------------------------------------------");
    {
        auto a = obj(); a["pattern"] = "std::regex_search($$$)";
        a["path"] = root.string();
        row("search_structural call shape", time_call(*provider, "search_structural", a, 3));
    }
    {
        auto a = obj(); a["path"] = root.string();
        row("repo_map", time_call(*provider, "repo_map", a, 3));
    }
    return 0;
}
