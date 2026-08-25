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
#include <algorithm>

#include <nlohmann/json.hpp>

namespace mcp::tools::detail {

using json = nlohmann::json;
using util::ToolError;
using util::ToolOutput;
using util::ExecResult;

namespace {

// (ANSI/OSC stripping moved to the subprocess capture boundary itself —
// util::strip_terminal_controls in tools/util/utf8.cpp — so LIVE progress
// snapshots are cleaned too, not just the final output. The local
// strip_ansi_escapes that used to live here handled only the settled
// body, which is exactly why running bash cards painted stray CSI
// parameter bytes mid-stream.)

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
    // Out-of-range: clamp UP to the max rather than silently resetting a
    // too-large request to the 60s default — a model asking for 600s wants
    // MORE time, and 60 would time out the very command it was raised for.
    if (timeout_int <= 0) timeout_int = 60;
    else if (timeout_int > 300) timeout_int = 300;

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

// Pull the lines that look like compiler/test/runtime errors out of captured
// output, so a failing command doesn't force the model to eyeball the whole
// dump. Same signal set the spill path uses; capped so we never balloon the
// message. Deduped preserves order.
std::vector<std::string> extract_error_lines(std::string_view output,
                                             std::size_t max_lines = 12) {
    static constexpr std::string_view kMarkers[] = {
        "error:", "Error:", "ERROR:", "error[", "FAILED", "FAIL:",
        "panicked", "Traceback", "Exception", "fatal:", "fatal error",
        "undefined reference", "assertion failed", "SIGSEGV", "cannot find",
        "No such file", "Permission denied", "command not found",
    };
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < output.size() && out.size() < max_lines) {
        std::size_t eol = output.find('\n', pos);
        if (eol == std::string_view::npos) eol = output.size();
        std::string_view line{output.data() + pos, eol - pos};
        for (auto m : kMarkers) {
            if (line.find(m) != std::string_view::npos) {
                // Trim to keep the digest tight; skip if we already have it.
                std::string_view t = line;
                while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                    t.remove_prefix(1);
                if (!t.empty()
                    && std::find(out.begin(), out.end(), std::string{t}) == out.end())
                    out.emplace_back(t);
                break;
            }
        }
        pos = eol + 1;
    }
    return out;
}

// Decode the exit codes a model most often misreads. Shells encode a
// signal-terminated child as 128+signum, and 126/127 have fixed meanings.
// Returns "" when the code carries no extra meaning worth a hint.
std::string explain_exit_code(int code) {
    switch (code) {
        case 124: return "(124: timed out — the coreutils `timeout` wrapper killed it)";
        case 126: return "(126: found but not executable — check the file's +x bit or that it's a script)";
        case 127: return "(127: command not found — check the name, PATH, or that the tool is installed)";
        case 128: return "(128: invalid exit argument)";
        case 130: return "(130: interrupted, SIGINT / Ctrl-C)";
        case 137: return "(137: killed, SIGKILL — usually the OOM killer; the process ran out of memory)";
        case 139: return "(139: segfault, SIGSEGV)";
        case 143: return "(143: terminated, SIGTERM)";
        default:  return {};
    }
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

        std::vector<std::string> error_lines = extract_error_lines(r.output, 10);

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
        // Teach the recovery path: a bare "timed out" invites re-running the
        // same command with the same deadline.
        const char* next_step =
            (a.timeout < 300)
                ? "\n\nIf the command needs more time, retry with a larger "
                  "`timeout` (max 300s); for servers/watchers that never "
                  "exit, use process_start instead."
                : "\n\nThis was already the maximum timeout (300s); for "
                  "long builds or servers, use process_start and poll it.";
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. No output was captured." << next_step;
        } else {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. Output captured before timeout:\n\n"
                << fence(r.output) << next_step;
        }
    } else if (r.exit_code != 0) {
        const std::string code_hint = explain_exit_code(r.exit_code);
        out << "Command \"" << a.command << "\" failed with exit code "
            << r.exit_code << ".";
        if (!code_hint.empty()) out << " " << code_hint;
        if (!r.output.empty()) {
            // Lead with a digest of the error-looking lines so the model sees
            // the failure cause first, then the full output for context. On a
            // 300-line build log this is the difference between a targeted fix
            // and re-scanning everything.
            auto errs = extract_error_lines(r.output, 12);
            if (!errs.empty()) {
                out << "\n\n\xe2\x9d\x8c Key error line"
                    << (errs.size() == 1 ? "" : "s") << ":\n";
                for (const auto& e : errs) out << "  " << e << "\n";
            }
            out << "\n" << fence(r.output);
        } else {
            out << " No output was captured"
                << (code_hint.empty()
                        ? " — the command signals only via its exit status."
                        : ".");
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
    // Out-of-the-box nudge: if this was a bare file-inspection shell-out
    // (cat/sed/head/grep/find/ls/wc) that a smart native tool does better,
    // prepend a one-line tip. NEVER blocks — the command already ran; this
    // just teaches the model to reach for `read`/`grep`/`glob`/`list_dir`
    // next time. Silent for pipes/redirects, where bash is the right call.
    if (auto tip = util::bash_tool_suggestion(a.command); !tip.empty())
        body = tip + "\n\n" + body;
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
