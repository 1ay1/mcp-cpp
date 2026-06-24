// SPDX-License-Identifier: Apache-2.0
//
// diagnostics.cpp — register_diagnostics_tool: runs the project's build
// or lint command, auto-detecting the build system. Faithful port of
// agentty's src/tool/tools/diagnostics.cpp.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/sandbox.hpp>
#include <mcp/tools/util/subprocess.hpp>
#include <mcp/tools/util/error.hpp>

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
    if (fs::exists("build/build.ninja", ec) || fs::exists("build/Makefile", ec)) return BuildSystem::CMake;
    if (fs::exists("Cargo.toml", ec))    return BuildSystem::Cargo;
    if (fs::exists("go.mod", ec))        return BuildSystem::Go;
    if (fs::exists("package.json", ec))  return BuildSystem::Node;
    if (fs::exists("Makefile", ec))      return BuildSystem::Make;
    return BuildSystem::None;
}

[[nodiscard]] std::vector<std::string> build_argv_for(BuildSystem bs) {
    switch (bs) {
        case BuildSystem::CMake: return {"cmake", "--build", "build"};
        case BuildSystem::Cargo: return {"cargo", "check"};
        case BuildSystem::Go:    return {"go", "build", "./..."};
        case BuildSystem::Node:  return {"npx", "tsc", "--noEmit"};
        case BuildSystem::Make:  return {"make", "-n"};
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

} // namespace mcp::tools::detail
