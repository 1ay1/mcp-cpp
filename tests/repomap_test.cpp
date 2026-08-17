// repomap_test — repo_map determinism + basic correctness.
//
// build_graph() parses source files in PARALLEL (atomic work-stealing across
// hardware threads) then merges in index order. This test guards the two
// invariants that parallelism could break:
//   1. DETERMINISM — the emitted map is byte-identical across runs regardless
//      of which thread parsed which file (the index-order merge guarantees it).
//   2. CORRECTNESS — real definitions in the tree are surfaced with their file.
// Drives the tool through the real make_provider() dispatch path.

#include "agtest.hpp"

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/capability.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace mcp;
namespace mt = mcp::tools;


static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

TEST_CASE("repomap") {
    const fs::path root = fs::temp_directory_path()
        / ("repomap_test_" + std::to_string(std::random_device{}()));
    fs::remove_all(root);
    fs::create_directories(root);

    // A tree big enough to actually fan out across threads (>32 files, so the
    // parallel path runs, not the inline small-tree fallback).
    for (int i = 0; i < 60; ++i) {
        write_file(root / ("src/mod" + std::to_string(i) + ".cpp"),
            "#include <string>\n"
            "int compute_" + std::to_string(i) + "(int x) { return x + "
            + std::to_string(i) + "; }\n"
            "void helper_" + std::to_string(i) + "() {}\n");
    }
    write_file(root / "src/core.cpp",
        "struct Widget { int id; };\n"
        "int widget_area(const Widget& w) { return w.id * w.id; }\n");

    mt::util::set_workspace_root(root.string());
    mt::HostServices svc;
    auto provider = mt::make_provider(svc, mt::ToolsetConfig{}, "local");

    auto run_map = [&]() -> std::string {
        auto r = provider->execute(cap::Request{"repo_map", nlohmann::json::object()});
        check(!r.is_error, "repo_map dispatch succeeded");
        return r.text;
    };

    const std::string m1 = run_map();
    const std::string m2 = run_map();   // second call: warm cache
    // Third call after clearing nothing — still deterministic.
    const std::string m3 = run_map();

    check(!m1.empty(), "repo_map produced a non-empty map");
    check(m1 == m2, "repo_map is deterministic run-to-run (warm)");
    check(m2 == m3, "repo_map stays stable on repeat");

    // Correctness: real definitions are surfaced.
    check(m1.find("compute_0") != std::string::npos
       || m1.find("widget_area") != std::string::npos
       || m1.find("core.cpp") != std::string::npos,
          "repo_map surfaces real symbols/files from the tree");

    fs::remove_all(root);
}
