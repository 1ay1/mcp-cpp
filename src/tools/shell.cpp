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
#include <mcp/tools/util/shellx.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/sandbox.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/utf8.hpp>

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
    std::vector<std::pair<std::string, std::string>> env;  // caller overrides
    std::string display_description;
    // Bound the output returned TO THE MODEL without bounding what the USER
    // sees. `cmd | head -20` filters at the wrong layer: it throws the rest
    // away before the terminal card ever gets it, so the human loses output
    // they were watching in order to save the model's context. These do the
    // filtering at the boundary instead — the card keeps the full stream,
    // the model gets the slice. 0 = unbounded.
    int         head_lines = 0;
    int         tail_lines = 0;
};

// Keep the first `head` and/or last `tail` lines, marking what was dropped.
// When both are set the two windows are returned with the elision between
// them, which is the shape a build log wants: the command that started it
// and the error that ended it.
std::string bound_lines(const std::string& in, int head, int tail) {
    if (head <= 0 && tail <= 0) return in;
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= in.size()) {
        size_t nl = in.find('\n', start);
        if (nl == std::string::npos) {
            if (start < in.size()) lines.emplace_back(in.data() + start, in.size() - start);
            break;
        }
        lines.emplace_back(in.data() + start, nl - start);
        start = nl + 1;
    }
    const int n = static_cast<int>(lines.size());
    const int h = head > 0 ? std::min(head, n) : 0;
    const int t = tail > 0 ? std::min(tail, n - h) : 0;
    if (h + t >= n) return in;   // nothing actually elided

    std::string out;
    for (int i = 0; i < h; ++i) { out += lines[static_cast<size_t>(i)]; out += '\n'; }
    out += "\n… " + std::to_string(n - h - t) + " lines elided (the terminal "
           "card still shows them in full) …\n\n";
    for (int i = n - t; i < n; ++i) { out += lines[static_cast<size_t>(i)]; out += '\n'; }
    return out;
}

std::expected<BashArgs, ToolError> parse_bash_args(const json& j) {
    util::ArgReader ar(j);
    auto cmd_opt = ar.require_str("command");
    if (!cmd_opt)
        return std::unexpected(ToolError::invalid_args("command required"));
    std::string cmd = *std::move(cmd_opt);
    // AST guard: judges every command in the script (pipes, chains, $(…),
    // control flow, bash -c, sudo/env/xargs/find -exec). If the script does
    // not parse cleanly, ALSO run the legacy text validator — fail closed.
    {
        const auto script = util::shellx::analyze(cmd);
        if (auto r = util::shellx::guard(script))
            return std::unexpected(ToolError::invalid_args(std::move(r->message)));
        if (!script.clean || script.truncated)
            if (auto why = util::validate_bash_command(cmd); !why.empty())
                return std::unexpected(ToolError::invalid_args(std::move(why)));
    }
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
        if (auto wp = util::make_workspace_path_checked(cd, "shell"); !wp)
            return std::unexpected(std::move(wp.error()));
    }

    // Optional env overrides: {"env": {"CI": "1", "RUST_LOG": "debug"}}.
    // Values are stringified defensively (a model may pass a number/bool).
    //
    // Refused keys: a denylist floor over the variables that make some OTHER
    // program run code of the caller's choosing. These defeat command
    // validation entirely -- the command string stays `git status` and passes
    // every check, while GIT_SSH_COMMAND or LD_PRELOAD is what actually
    // executes. That is a known agent-tool break (CVE-2026-55743 is the
    // env-prefix form of the same trick), and the host's consent card shows
    // the command, so the payload rides along unseen.
    //
    // A denylist cannot be complete -- GOFLAGS=-toolexec=, RUSTC_WRAPPER and
    // friends keep arriving -- so this is a floor, not a boundary. The
    // boundary is the sandbox. What it buys is that the easy, well-known
    // forms stop working silently.
    static constexpr std::string_view kRefusedEnv[] = {
        // dynamic loader
        "LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH", "LD_DEBUG_OUTPUT",
        "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
        // shell startup + word splitting
        "BASH_ENV", "ENV", "ZDOTDIR", "PROMPT_COMMAND", "IFS", "SHELLOPTS",
        "BASH_FUNC_",            // exported-function smuggling (shellshock shape)
        // what git runs on your behalf
        "GIT_SSH", "GIT_SSH_COMMAND", "GIT_ASKPASS", "GIT_EDITOR",
        "GIT_PAGER", "GIT_EXTERNAL_DIFF", "GIT_PROXY_COMMAND",
        "GIT_CONFIG", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM",
        "GIT_CONFIG_COUNT", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        // interpreters and toolchains that take a hook from the environment
        "PYTHONSTARTUP", "PYTHONPATH", "PYTHONHOME", "PERL5OPT", "PERL5LIB",
        "RUBYOPT", "RUBYLIB", "NODE_OPTIONS", "NODE_REPL_EXTERNAL_MODULE",
        "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS",
        "GOFLAGS", "RUSTC_WRAPPER", "RUSTC", "CARGO_BUILD_RUSTC",
        "MAKEFLAGS", "EDITOR", "VISUAL", "PAGER_COMMAND",
        // and PATH itself: repoint it and every bare command is yours
        "PATH",
    };
    const auto refused = [](std::string_view k) {
        for (std::string_view bad : kRefusedEnv) {
            if (bad.back() == '_') {          // prefix entry
                if (k.size() >= bad.size()
                    && std::equal(bad.begin(), bad.end(), k.begin(),
                                  [](char a, char b) {
                                      return std::toupper((unsigned char)a)
                                           == std::toupper((unsigned char)b);
                                  }))
                    return true;
                continue;
            }
            if (k.size() == bad.size()
                && std::equal(bad.begin(), bad.end(), k.begin(),
                              [](char a, char b) {
                                  return std::toupper((unsigned char)a)
                                       == std::toupper((unsigned char)b);
                              }))
                return true;
        }
        return false;
    };

    std::vector<std::pair<std::string, std::string>> env;
    if (const json* e = ar.raw("env"); e && e->is_object()) {
        std::string bad_keys;
        for (auto it = e->begin(); it != e->end(); ++it) {
            if (it.key().empty()) continue;
            if (refused(it.key())) {
                if (!bad_keys.empty()) bad_keys += ", ";
                bad_keys += it.key();
                continue;
            }
            std::string val;
            if (it.value().is_string())        val = it.value().get<std::string>();
            else if (it.value().is_number_integer()) val = std::to_string(it.value().get<long long>());
            else if (it.value().is_boolean())  val = it.value().get<bool>() ? "1" : "0";
            else if (!it.value().is_null())    val = it.value().dump();
            env.emplace_back(it.key(), std::move(val));
        }
        // Refuse the call rather than running it with the key dropped: a
        // command written to depend on one of these would otherwise do
        // something subtly different from what was asked, which is worse
        // than a clear no.
        if (!bad_keys.empty())
            return std::unexpected(ToolError::invalid_args(
                "env key(s) refused: " + bad_keys +
                ". These make another program execute code of your choosing, "
                "so they would bypass command validation and the approval the "
                "user gave. Put what you need IN the command instead."));
    }

    // Clamped: a model asking for 100k lines wants "all of it", and the
    // spill path already handles genuinely huge output better than a slice
    // would. Negative is meaningless, so it reads as unset.
    auto bound = [&](const char* key) {
        int v = ar.integer(key, 0);
        if (v < 0) v = 0;
        if (v > 2000) v = 2000;
        return v;
    };

    return BashArgs{
        std::move(cmd),
        timeout_int,
        std::move(cd),
        std::move(env),
        ar.str("display_description", ""),
        bound("head_lines"),
        bound("tail_lines"),
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

// The shell this platform means by "run a command line". Named rather than
// inlined so the two spellings sit together instead of being spread across
// an #ifdef at the call site.
[[nodiscard]] constexpr const char* shell_program() {
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}
[[nodiscard]] constexpr const char* shell_flag() {
#ifdef _WIN32
    return "/c";
#else
    return "-c";
#endif
}

ExecResult run_bash(const BashArgs& a, Exec& exec) {
    auto t0 = std::chrono::steady_clock::now();
    const std::string& cmd_str = a.command;
    const int           tmo_s   = a.timeout;

    std::string effective = cmd_str;
    // Real working directory (passed to the child via chdir), NOT a `cd &&`
    // shell prefix — the path can't be re-parsed/mangled and works even for a
    // command that isn't shell-wrapped.
    const std::string& cwd = a.cd;

    // Non-interactive, deterministic child environment. These defaults keep
    // captured output clean (no ANSI colour to bloat the model's context) and
    // stop tools from BLOCKING on a tty they'll never get (git credential/
    // editor prompts, pagers). Model-supplied env (a.env) layers on top and
    // wins, so a caller can still force colour or a pager if it really wants.
    std::vector<std::pair<std::string, std::string>> child_env = {
        {"NO_COLOR", "1"},              // https://no-color.org
        {"CLICOLOR", "0"},
        {"CLICOLOR_FORCE", "0"},
        {"TERM", "dumb"},               // discourages cursor/colour escapes
        {"PAGER", "cat"},
        {"GIT_PAGER", "cat"},
        {"GIT_TERMINAL_PROMPT", "0"},   // never block on a credential prompt
        {"GIT_OPTIONAL_LOCKS", "0"},
        {"DEBIAN_FRONTEND", "noninteractive"},
        {"PYTHONUNBUFFERED", "1"},      // stream python output to the idle watchdog
    };
    for (const auto& kv : a.env) child_env.push_back(kv);   // caller overrides win

    constexpr std::size_t kCaptureCap       = 8u * 1024u * 1024u;
    constexpr std::size_t kModelPreviewBytes = 30000;
    constexpr std::size_t kSpillPreviewHead = 2000;   // first 2 KB
    constexpr std::size_t kSpillPreviewTail = 1000;   // last 1 KB

    // Ask the host to run it. We no longer own a poll loop, a deadline, a
    // pipe or a signal: the embedding application does, over whatever
    // platform layer it has, and it is the same one it uses for its own
    // work. `timeout` is the IDLE budget -- the clock output resets -- and
    // the wall ceiling is the host's to set, because only it knows what a
    // runaway costs on this machine.
    ExecRequest ereq;
    ereq.program          = {shell_program(), {shell_flag(), effective}};
    if (!cwd.empty()) ereq.cwd = cwd;
    ereq.env              = child_env;
    ereq.budgets.idle     = std::chrono::seconds{tmo_s};
    ereq.max_output_bytes = kCaptureCap;

    const auto res = exec.run(ereq);

    // Shaped like what the rest of this function already reads, so changing
    // the runner underneath did not turn into a rewrite of the formatting.
    struct Captured {
        std::string output;
        int  exit_code   = 0;
        bool started     = true;
        bool truncated   = false;
        bool timed_out   = false;
        bool hit_wall    = false;   // WHICH clock, see below
        std::string start_error;
    } r;
    // Strip cursor moves, colour and the rest. The env above (NO_COLOR,
    // TERM=dumb) suppresses most of it at the SOURCE, which is cleaner than
    // post-stripping and also kills progress-bar cursor thrash -- but a
    // program that writes escapes unconditionally still gets through, and
    // the model should not be reading them. Presentation, so it belongs to
    // the tool rather than to the exec capability, which only promises
    // valid UTF-8.
    r.output    = util::strip_terminal_controls(res.output);
    r.truncated = res.truncated;
    std::visit([&]<class T>(const T& o) {
        if constexpr (std::is_same_v<T, Exited>)        r.exit_code = o.code;
        else if constexpr (std::is_same_v<T, Signalled>) r.exit_code = 128 + o.signal;
        else if constexpr (std::is_same_v<T, StartFailed>) {
            r.started = false; r.start_error = o.reason;
        } else if constexpr (std::is_same_v<T, Cancelled>) {
            r.exit_code = 130;   // the shell's convention for an interrupt
        } else if constexpr (std::is_same_v<T, StoppedEarly>) {
            // Got what it asked for; not a failure.
        } else {
            static_assert(std::is_same_v<T, TimedOut>, "unhandled ExecOutcome arm");
            r.timed_out = true;
            r.hit_wall  = o.which == TimedOut::budget::wall;
        }
    }, res.outcome);

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

    // Slice for the MODEL only, and only after the spill path has had its
    // say — the user's card already holds the whole stream, and a spilled
    // output is a preview envelope that must not be re-sliced.
    if ((a.head_lines > 0 || a.tail_lines > 0) && !r.output.empty()
        && r.output.rfind("<persisted-output>", 0) != 0)
        r.output = bound_lines(r.output, a.head_lines, a.tail_lines);

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
    // Advisory only: the command already ran. The tip names the PARAMETER
    // that replaces the shell idiom (limit:20, offset:-50, head_lines), not
    // a whole native call. Naming the tool is the advice models already
    // ignore; naming the parameter fixes the belief behind the pipe.
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
                "Working directory to run the command in (a real chdir in the "
                "child, so relative paths and the command's own $PWD are "
                "correct). Must be an existing directory inside the workspace."}}},
            {"env",     {{"type","object"},
                {"additionalProperties", {{"type","string"}}},
                {"description",
                "Extra environment variables for THIS command, e.g. "
                "{\"CI\":\"1\",\"RUST_LOG\":\"debug\"}. Layered on top of the "
                "inherited environment (your values win). The tool already "
                "forces a clean non-interactive env (NO_COLOR, PAGER=cat, "
                "GIT_TERMINAL_PROMPT=0, TERM=dumb, \u2026) so output is quiet and "
                "nothing blocks on a prompt \u2014 only set this to add/override."}}},
            {"timeout", {{"type","integer"}, {"description","Timeout in seconds (default 60, max 300)"}}},
            {"timeout_ms", {{"type","integer"}, {"description",
                "Alternative timeout in milliseconds (rounded up to seconds)."}}},
            {"head_lines", {{"type","integer"}, {"description",
                "Return only the FIRST N lines of output to you. Use this "
                "instead of piping to `head`: the pipe throws the rest away "
                "before the terminal card sees it, so the user loses output "
                "they were watching \u2014 this bounds only what reaches you, "
                "and the card still shows everything."}}},
            {"tail_lines", {{"type","integer"}, {"description",
                "Return only the LAST N lines of output to you (the usual "
                "choice for build/test logs, where the failure is at the "
                "end). Use this instead of piping to `tail`. Combine with "
                "head_lines to get both ends with the middle elided."}}},
        }},
    };
}

} // namespace

void register_shell_tools(Shells& sh, const std::shared_ptr<Exec>& exec) {
    if (!exec) return;   // no way to run anything → no shell tool

    sh.add("shell",
#ifdef _WIN32
        "Executes a shell command and returns its combined output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "This runs under cmd.exe on Windows — use native equivalents like "
        "`dir`, `where`, `systeminfo`, `type`, `findstr`, or `powershell -c`. "
        "Do NOT use POSIX-only commands (`uname`, `cat /etc/os-release`, "
        "`sw_vers`, `ls`, `grep`, `sed`, `awk`, heredocs) — they will fail. "
        "Do NOT use for file IO — use the write/edit/read tools instead.",
#else
        "Executes a shell command and returns its combined output. The "
        "command runs under the operating-system shell (POSIX /bin/sh on "
        "Linux and macOS), NOT bash — keep commands portable and avoid "
        "bashisms. Output is truncated at 30k chars. Use for builds, tests, "
        "git, etc. Do NOT use for file IO — use the write/edit/read tools "
        "instead (no cat/echo/sed/heredoc to create or modify files).",
#endif
        bash_schema(), EffectSet{Effect::Exec},
        body_with<BashArgs>([exec](const BashArgs& a) { return run_bash(a, *exec); },
                            parse_bash_args), 30'000);
}

} // namespace mcp::tools::detail
