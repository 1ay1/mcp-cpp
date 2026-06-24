// SPDX-License-Identifier: Apache-2.0
//
// shell.cpp — register_shell_tools: bash.
// Faithful port of agentty's src/tool/tools/bash.cpp. The refined
// domain::NonEmpty/Bounded types are replaced with plain string/int
// (the parser already enforces the same invariants). ANSI stripping,
// spill-to-disk, and the per-state output formatting are verbatim.

#include "tool_shell.hpp"
#include "tool_body.hpp"

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/bash_validate.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/sandbox.hpp>
#include <mcp/tools/util/subprocess.hpp>
#include <mcp/tools/util/error.hpp>

#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

// Strip ANSI / OSC escape sequences from captured subprocess output.
std::string strip_ansi_escapes(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        if (b != 0x1b) {              // not an escape — keep verbatim
            out.push_back(in[i]);
            ++i;
            continue;
        }
        if (i + 1 >= in.size()) { ++i; continue; }   // dangling ESC
        unsigned char next = static_cast<unsigned char>(in[i + 1]);
        if (next == '[') {
            i += 2;
            while (i < in.size()) {
                unsigned char c = static_cast<unsigned char>(in[i++]);
                if (c >= 0x40 && c <= 0x7e) break;
            }
        } else if (next == ']') {
            i += 2;
            std::size_t cap = std::min(in.size(), i + 4096);
            while (i < cap) {
                unsigned char c = static_cast<unsigned char>(in[i]);
                if (c == 0x07) { ++i; break; }
                if (c == 0x1b && i + 1 < in.size()
                    && in[i + 1] == '\\') { i += 2; break; }
                ++i;
            }
        } else {
            i += 2;
        }
    }
    return out;
}

struct BashArgs {
    std::string command;
    int         timeout;   // [1, 300]
    std::string cd;        // optional; empty = inherit cwd
    std::string display_description;
};

std::expected<BashArgs, ToolError> parse_bash_args(const json& j) {
    util::ArgReader ar(j);
    auto cmd_opt = ar.require_str("command");
    if (!cmd_opt)
        return std::unexpected(ToolError::invalid_args("command required"));
    std::string cmd = *std::move(cmd_opt);
    if (auto why = util::validate_bash_command(cmd); !why.empty())
        return std::unexpected(ToolError::invalid_args(std::move(why)));
    if (cmd.empty())
        return std::unexpected(ToolError::invalid_args("command must not be empty"));

    int timeout_int = ar.integer("timeout", 60);
    if (ar.has("timeout_ms")) {
        int ms = ar.integer("timeout_ms", 0);
        if (ms > 0) timeout_int = (ms + 999) / 1000;
    }
    if (timeout_int <= 0 || timeout_int > 300) timeout_int = 60;

    std::string cd = ar.str("cd", "");
    if (!cd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(cd, ec))
            return std::unexpected(ToolError::invalid_args(
                "cd '" + cd + "' is not a directory"));
        if (auto wp = util::make_workspace_path_checked(cd, "bash"); !wp)
            return std::unexpected(std::move(wp.error()));
    }
    return BashArgs{
        std::move(cmd),
        timeout_int,
        std::move(cd),
        ar.str("display_description", ""),
    };
}

ExecResult run_bash(const BashArgs& a) {
    auto t0 = std::chrono::steady_clock::now();
    const std::string& cmd_str = a.command;
    const int           tmo_s   = a.timeout;

    std::string effective = cmd_str;
    if (!a.cd.empty()) {
#ifdef _WIN32
        if (a.cd.find('"') != std::string::npos)
            return std::unexpected(ToolError::invalid_args(
                "cd path contains '\"', which cmd.exe cannot quote"));
        effective = "cd /d \"" + a.cd + "\" && " + cmd_str;
#else
        std::string q;
        q.reserve(a.cd.size() + 4);
        q.push_back('\'');
        for (char c : a.cd) { if (c == '\'') q += "'\\''"; else q.push_back(c); }
        q.push_back('\'');
        effective = "cd " + q + " && " + cmd_str;
#endif
    }
    constexpr std::size_t kCaptureCap       = 8u * 1024u * 1024u;
    constexpr std::size_t kModelPreviewBytes = 30000;
    constexpr std::size_t kSpillPreviewHead = 2000;   // first 2 KB
    constexpr std::size_t kSpillPreviewTail = 1000;   // last 1 KB
    auto r = util::sandbox::run_shell_command(effective, kCaptureCap,
                                              std::chrono::seconds{tmo_s});
    r.output = strip_ansi_escapes(r.output);

    std::string spill_path;
    std::size_t spill_total = 0;
    if (r.output.size() > kModelPreviewBytes) {
        spill_total = r.output.size();
        try {
            namespace fs = std::filesystem;
            auto dir = fs::temp_directory_path() / "agentty-bash";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::random_device rd;
            std::mt19937_64 gen(rd());
            char name[32];
            std::snprintf(name, sizeof(name), "out-%016llx.txt",
                          static_cast<unsigned long long>(gen()));
            auto path = dir / name;
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f) {
                f.write(r.output.data(),
                        static_cast<std::streamsize>(r.output.size()));
                f.close();
                spill_path = path.string();
            }
        } catch (...) {
        }
        std::string head = r.output.substr(0, kSpillPreviewHead);
        std::string tail;
        if (r.output.size() > kSpillPreviewHead + kSpillPreviewTail + 100) {
            tail = r.output.substr(r.output.size() - kSpillPreviewTail);
        }

        std::vector<std::string> error_lines;
        {
            std::size_t pos = 0;
            while (pos < r.output.size() && error_lines.size() < 10) {
                std::size_t eol = r.output.find('\n', pos);
                if (eol == std::string::npos) eol = r.output.size();
                std::string_view line{r.output.data() + pos, eol - pos};
                bool is_error = (line.find("error:") != std::string_view::npos ||
                                 line.find("Error:") != std::string_view::npos ||
                                 line.find("ERROR:") != std::string_view::npos ||
                                 line.find("FAILED") != std::string_view::npos ||
                                 line.find("error[") != std::string_view::npos ||
                                 line.find("panicked") != std::string_view::npos ||
                                 line.find("Traceback") != std::string_view::npos ||
                                 line.find("Exception") != std::string_view::npos);
                if (is_error) {
                    error_lines.emplace_back(line);
                }
                pos = eol + 1;
            }
        }

        std::ostringstream env;
        env << "<persisted-output>\n";
        env << "Output too large (" << (spill_total / 1024) << " KB total). ";
        if (!spill_path.empty()) {
            env << "Full output saved to: " << spill_path
                << "\n\nIf you need bytes past the preview, use the read tool "
                   "on that path with offset/limit.\n\n";
        } else {
            env << "(spill file unavailable; output truncated.)\n\n";
        }
        env << "Preview (first " << kSpillPreviewHead << " bytes):\n"
            << head;
        if (!error_lines.empty()) {
            env << "\n\n\xe2\x9d\x8c Errors found (extracted from full output):\n";
            for (const auto& el : error_lines) {
                env << "  " << el << "\n";
            }
        }
        if (!tail.empty()) {
            env << "\n\n... [" << (spill_total - kSpillPreviewHead - kSpillPreviewTail)
                << " bytes elided] ...\n\n"
                << "Tail (last " << kSpillPreviewTail << " bytes):\n"
                << tail;
        }
        env << "\n</persisted-output>";
        r.output    = std::move(env).str();
        r.truncated = false;   // spilled, not lost
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (!r.started)
        return std::unexpected(ToolError::spawn(
            "failed to spawn command: " + r.start_error));

    auto fence = [](const std::string& body) {
        return std::string{"```\n"} + body + (body.empty() || body.back() == '\n'
                                              ? "" : "\n") + "```";
    };

    std::ostringstream out;
    if (r.timed_out) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. No output was captured.";
        } else {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. Output captured before timeout:\n\n"
                << fence(r.output);
        }
    } else if (r.exit_code != 0) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".";
        } else {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".\n\n" << fence(r.output);
        }
    } else if (r.output.empty()) {
        out << "Command executed successfully.";
    } else {
        out << fence(r.output);
    }
    if (r.truncated)
        out << "\n\n[output truncated at " << kCaptureCap << " bytes]";
    if (elapsed_ms >= 500)
        out << "\n\n[elapsed: "
            << (elapsed_ms < 10000
                ? (std::to_string(elapsed_ms) + " ms")
                : (std::to_string(elapsed_ms / 1000) + "."
                   + std::to_string((elapsed_ms % 1000) / 100) + " s"))
            << "]";

    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

json bash_schema() {
    return json{
        {"type","object"},
        {"required", {"command"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI — e.g. "
                               "'Run the test suite'. Optional but strongly "
                               "recommended."}}},
            {"command", {{"type","string"}, {"description","The shell command to execute"}}},
            {"cd",      {{"type","string"}, {"description",
                "Working directory for the command. If set, runs as `cd <dir> && <command>`."}}},
            {"timeout", {{"type","integer"}, {"description","Timeout in seconds (default 60, max 300)"}}},
            {"timeout_ms", {{"type","integer"}, {"description",
                "Alternative timeout in milliseconds (rounded up to seconds)."}}},
        }},
    };
}

} // namespace

void register_shell_tools(Shells& sh) {
    sh.add("bash",
#ifdef _WIN32
        "Run a shell command via Windows cmd.exe and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "This runs under cmd.exe on Windows — use native equivalents like "
        "`dir`, `where`, `systeminfo`, `type`, `findstr`, or `powershell -c`. "
        "Do NOT use POSIX-only commands (`uname`, `cat /etc/os-release`, "
        "`sw_vers`, `ls`, `grep`, `sed`, `awk`, heredocs) — they will fail. "
        "Do NOT use for file IO — use the write/edit/read tools instead.",
#else
        "Run a shell command and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "Do NOT use for file IO — use the write/edit/read tools instead "
        "(no cat/echo/sed/heredoc to create or modify files).",
#endif
        bash_schema(), EffectSet{Effect::Exec},
        body<BashArgs>(run_bash, parse_bash_args), 30'000);
}

} // namespace mcp::tools::detail
