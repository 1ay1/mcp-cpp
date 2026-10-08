// tests/test_exec.hpp — the smallest tools::Exec a test needs: run a program
// to completion with popen, merged output. No budgets, no sessions. A real
// host (agentty) implements Exec with its own clocks and process tree.
#pragma once

#if !defined(_WIN32)

#include <mcp/tools/host.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <sys/wait.h>

namespace mcp::test {

struct PopenExec final : tools::Exec {
    tools::ExecResult run(const tools::ExecRequest& r) override {
        auto q = [](const std::string& s) {
            std::string o = "'";
            for (char c : s) o += (c == '\'') ? std::string("'\\''") : std::string(1, c);
            return o + "'";
        };
        std::string cmd;
        if (r.cwd) cmd += "cd " + q(*r.cwd) + " && ";
        for (const auto& [k, v] : r.env) cmd += k + "=" + q(v) + " ";
        cmd += q(r.program.exe);
        for (const auto& a : r.program.args) cmd += " " + q(a);
        cmd += " 2>&1";
        tools::ExecResult out;
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) { out.outcome = tools::StartFailed{"popen failed"}; return out; }
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
            out.output.append(buf, n);
            if (r.max_output_bytes && out.output.size() > *r.max_output_bytes) {
                out.output.resize(*r.max_output_bytes);
                out.truncated = true;
            }
        }
        const int st = ::pclose(p);
        if (WIFEXITED(st)) {
            const int code = WEXITSTATUS(st);
            if (code == 127) out.outcome = tools::StartFailed{"not found: " + r.program.exe};
            else             out.outcome = tools::Exited{code};
        } else {
            out.outcome = tools::Signalled{WIFSIGNALED(st) ? WTERMSIG(st) : 0};
        }
        return out;
    }
    std::expected<std::shared_ptr<tools::Session>, std::string> start(const tools::ExecRequest&) override {
        return std::unexpected(std::string{"sessions not supported in tests"});
    }
    bool stops_whole_tree() const noexcept override { return false; }
};

}  // namespace mcp::test

#endif
