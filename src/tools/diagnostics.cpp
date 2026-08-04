// SPDX-License-Identifier: Apache-2.0
//
// diagnostics.cpp — register_diagnostics_tool: runs the project's build
// or lint command, auto-detecting the build system. Faithful port of
// agentty's src/tool/tools/diagnostics.cpp.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/sandbox.hpp>
#include <mcp/tools/util/subprocess.hpp>
#include <mcp/tools/util/error.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
namespace fs = std::filesystem;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

struct DiagnosticsArgs {
    std::string command;  // empty means auto-detect
    std::string display_description;
};

enum class BuildSystem { None, CMake, Cargo, Go, Node, Make };

[[nodiscard]] BuildSystem detect_build_system() noexcept {
    std::error_code ec;
    const auto& root = util::workspace_root();
    if (fs::exists(root / "build/build.ninja", ec) || fs::exists(root / "build/Makefile", ec)) return BuildSystem::CMake;
    if (fs::exists(root / "Cargo.toml", ec))    return BuildSystem::Cargo;
    if (fs::exists(root / "go.mod", ec))        return BuildSystem::Go;
    if (fs::exists(root / "package.json", ec))  return BuildSystem::Node;
    if (fs::exists(root / "Makefile", ec))      return BuildSystem::Make;
    return BuildSystem::None;
}

[[nodiscard]] std::vector<std::string> build_argv_for(BuildSystem bs) {
    const auto root = util::workspace_root().string();
    switch (bs) {
        case BuildSystem::CMake: return {"cmake", "--build", (util::workspace_root() / "build").string()};
        case BuildSystem::Cargo: return {"cargo", "check", "--manifest-path", (util::workspace_root() / "Cargo.toml").string()};
        case BuildSystem::Go:    return {"go", "-C", root, "build", "./..."};
        case BuildSystem::Node:  return {"npm", "--prefix", root, "exec", "--", "tsc", "--noEmit"};
        case BuildSystem::Make:  return {"make", "-C", root, "-n"};
        case BuildSystem::None:  return {};
    }
    return {};
}

std::expected<DiagnosticsArgs, ToolError> parse_diagnostics_args(const json& j) {
    util::ArgReader ar(j);
    return DiagnosticsArgs{
        ar.str("command", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_diagnostics(const DiagnosticsArgs& a) {
    std::vector<std::string> auto_argv;
    if (a.command.empty()) {
        auto_argv = build_argv_for(detect_build_system());
        if (auto_argv.empty())
            return std::unexpected(ToolError::not_found("no build system detected; pass a command"));
    }
    auto sub = auto_argv.empty()
        ? util::sandbox::run_shell_command(a.command, /*max_bytes*/100'000,
                                           std::chrono::seconds{120})
        : util::sandbox::run_argv(auto_argv, /*max_bytes*/100'000,
                                  std::chrono::seconds{120});
    auto output = util::legacy_format(sub, std::chrono::seconds{120});
    if (output.empty()) return ToolOutput{"no diagnostics (clean build)", std::nullopt};

    int errors = 0, warnings = 0;
    std::vector<std::string> error_lines;
    error_lines.reserve(10);
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        bool is_error = (line.find("error:") != std::string::npos ||
                         line.find("Error:") != std::string::npos ||
                         line.find("ERROR:") != std::string::npos ||
                         line.find(" error ") != std::string::npos ||
                         line.find("error[") != std::string::npos);
        bool is_warning = (line.find("warning:") != std::string::npos ||
                           line.find("Warning:") != std::string::npos ||
                           line.find("WARNING:") != std::string::npos ||
                           line.find("warn[") != std::string::npos);
        if (is_error) {
            ++errors;
            if (error_lines.size() < 10) error_lines.push_back(line);
        }
        if (is_warning) ++warnings;
    }

    std::ostringstream result;
    if (errors > 0 || warnings > 0) {
        result << "\xe2\x9d\x8c " << errors << " error(s), " << warnings << " warning(s)\n\n";
        if (!error_lines.empty()) {
            result << "First errors:\n";
            for (const auto& el : error_lines) {
                result << "  " << el << "\n";
            }
            result << "\n";
        }
        result << "Full output:\n";
    }
    result << output;

    std::string body = result.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

struct TestArgs {
    std::string command;
    std::string filter;
    int timeout_seconds = 120;
    int repeat = 1;
};

std::expected<TestArgs, ToolError> parse_test_args(const json& j) {
    util::ArgReader ar(j);
    return TestArgs{
        ar.str("command", ""),
        ar.str("filter", ar.str("target", "")),
        std::clamp(ar.integer("timeout_seconds", 120), 1, 1800),
        std::clamp(ar.integer("repeat", 1), 1, 100),
    };
}

std::vector<std::string> test_argv_for(BuildSystem bs, const TestArgs& a) {
    std::vector<std::string> argv;
    const auto root = util::workspace_root().string();
    switch (bs) {
        case BuildSystem::CMake:
            argv = {"ctest", "--test-dir", (util::workspace_root() / "build").string(), "--output-on-failure"};
            if (!a.filter.empty()) argv.insert(argv.end(), {"-R", a.filter});
            if (a.repeat > 1) argv.insert(argv.end(), {"--repeat", "until-fail:" + std::to_string(a.repeat)});
            break;
        case BuildSystem::Cargo:
            argv = {"cargo", "test", "--manifest-path", (util::workspace_root() / "Cargo.toml").string()};
            if (!a.filter.empty()) argv.push_back(a.filter);
            break;
        case BuildSystem::Go:
            argv = {"go", "-C", root, "test", "./..."};
            if (!a.filter.empty()) argv.insert(argv.end(), {"-run", a.filter});
            if (a.repeat > 1) argv.insert(argv.end(), {"-count", std::to_string(a.repeat)});
            break;
        case BuildSystem::Node:
            argv = {"npm", "--prefix", root, "test", "--"};
            if (!a.filter.empty()) argv.push_back(a.filter);
            break;
        case BuildSystem::Make:
            argv = {"make", "-C", root, "test"};
            break;
        case BuildSystem::None:
            break;
    }
    return argv;
}

ExecResult run_tests(const TestArgs& a) {
    const auto bs = a.command.empty() ? detect_build_system() : BuildSystem::None;
    auto argv = a.command.empty() ? test_argv_for(bs, a)
                                  : std::vector<std::string>{};
    if (a.command.empty() && argv.empty())
        return std::unexpected(ToolError::not_found(
            "no test runner detected; pass command explicitly"));
    const auto timeout = std::chrono::seconds{a.timeout_seconds};

    // CTest (until-fail:N) and Go (-count N) express `repeat` natively in the
    // argv built above. Every other runner — Cargo, npm, make, and custom
    // commands — has no repeat flag, so honor it with an outer loop instead
    // of silently ignoring the arg the schema advertises. Stop early on the
    // first failure (repeat is for confirming stability / hunting flakes).
    const bool native_repeat = (bs == BuildSystem::CMake || bs == BuildSystem::Go);
    const int loops = (a.repeat > 1 && !native_repeat) ? a.repeat : 1;

    util::SubprocessResult sub;
    int run_no = 0;
    for (; run_no < loops; ++run_no) {
        sub = a.command.empty()
            ? util::sandbox::run_argv(argv, 200'000, timeout)
            : util::sandbox::run_shell_command(a.command, 200'000, timeout);
        if (sub.exit_code != 0 || sub.timed_out) break;
    }
    const int runs_done = std::min(run_no + 1, loops);

    std::string output = util::legacy_format(sub, timeout);
    std::ostringstream summary;
    summary << (sub.exit_code == 0 && !sub.timed_out ? "PASS" : "FAIL")
            << " exit=" << sub.exit_code;
    if (sub.timed_out) summary << " timeout=" << a.timeout_seconds << "s";
    if (loops > 1)
        summary << " (run " << runs_done << "/" << loops
                << (sub.exit_code == 0 && !sub.timed_out ? " all passed" : " — stopped on failure") << ")";
    summary << '\n';
    if (!output.empty()) summary << output;
    return ToolOutput{summary.str(), std::nullopt};
}

json test_schema() {
    return json{{"type","object"}, {"properties", {
        {"command", {{"type","string"}, {"description","Custom test command; otherwise auto-detect."}}},
        {"filter", {{"type","string"}, {"description","CTest regex, Cargo test name, Go -run regex, or npm test filter."}}},
        {"target", {{"type","string"}, {"description","Alias for filter."}}},
        {"timeout_seconds", {{"type","integer"}, {"minimum",1}, {"maximum",1800}, {"default",120}}},
        {"repeat", {{"type","integer"}, {"minimum",1}, {"maximum",100}, {"default",1}}}
    }}};
}

json diagnostics_schema() {
    return json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"command", {{"type","string"}, {"description",
                "Custom build command. If omitted, auto-detects."}}},
        }},
    };
}

} // namespace

void register_diagnostics_tool(Shells& sh) {
    sh.add("diagnostics",
        "Run the project's build or lint command and return errors/warnings. "
        "Auto-detects build system (CMake, cargo, go, npm, make).",
        diagnostics_schema(), EffectSet{Effect::Exec},
        body<DiagnosticsArgs>(run_diagnostics, parse_diagnostics_args), 30'000);
}

void register_test_tool(Shells& sh) {
    sh.add("test",
        "Run focused project tests with structured pass/fail status, live output, filtering, repetition, and timeout. "
        "Auto-detects CTest, Cargo, Go, npm, or Make; pass command for custom runners.",
        test_schema(), EffectSet{Effect::Exec},
        body<TestArgs>(run_tests, parse_test_args), 40'000);
}

} // namespace mcp::tools::detail
