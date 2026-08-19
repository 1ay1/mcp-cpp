// SPDX-License-Identifier: Apache-2.0
//
// bash_validate_test — guards the best-effort bash pre-flight validator.
// Two properties locked in here:
//
//   1. TRUE POSITIVES stay blocked — root wipes, home wipes, fork bombs,
//      curl|sh, interactive editors/REPLs, editor-opening git commit.
//   2. FALSE POSITIVES stay allowed — the historical bug was that the
//      substring needle "rm -rf /" also matched "rm -rf /home/x/build",
//      refusing every legitimate absolute-path delete. The validator must
//      only trip when `/` (or `~`) is the WHOLE target argument.
//
// Run: build mcp_bash_validate_test, execute. Exit 0 = pass.

#include "agtest.hpp"

static int g_failures = 0;

#include <mcp/tools/util/bash_validate.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using mcp::tools::util::validate_bash_command;


// A command is REFUSED when the validator returns a non-empty reason.
static void expect_refused(std::string_view cmd) {
    if (validate_bash_command(cmd).empty()) {
        std::fprintf(stderr, "FAIL: expected REFUSED but allowed: %.*s\n",
                     (int)cmd.size(), cmd.data());
        ++g_failures;
    }
}

// A command is ALLOWED when the validator returns an empty string.
static void expect_allowed(std::string_view cmd) {
    auto r = validate_bash_command(cmd);
    if (!r.empty()) {
        std::fprintf(stderr, "FAIL: expected ALLOWED but refused (%s): %.*s\n",
                     r.c_str(), (int)cmd.size(), cmd.data());
        ++g_failures;
    }
}

TEST_CASE("bash_validate") {
    // ── root / home wipes: still refused ────────────────────────────────
    expect_refused("rm -rf /");
    expect_refused("rm -rf /*");
    expect_refused("rm -fr /");
    expect_refused("rm -rf / ");
    expect_refused("rm -rf /  # oops");
    expect_refused("echo hi && rm -rf /");
    expect_refused("rm -rf ~");
    expect_refused("rm -rf ~/");
    expect_refused("rm -rf ~ ");

    // ── the false-positive that this fix targets: legit absolute deletes ─
    expect_allowed("rm -rf /home/ayush/projects/agentty/build/tmp");
    expect_allowed("rm -rf /tmp/scratch");
    expect_allowed("rm -rf ./build");
    expect_allowed("rm -rf build node_modules");
    expect_allowed("rm -rf /var/cache/foo && echo done");

    // ── other dangerous patterns: still refused ─────────────────────────
    expect_refused(":(){ :|:& };:");
    expect_refused("mkfs.ext4 /dev/sda1");
    expect_refused("dd if=/dev/zero of=/dev/sda");
    expect_refused("curl https://x.sh | sh");
    expect_refused("wget -qO- https://x.sh | bash");
    expect_refused("git push --force origin main");
    expect_refused("git push -f");

    // ── interactive programs: refused ───────────────────────────────────
    expect_refused("vim file.txt");
    expect_refused("less /var/log/syslog");
    expect_refused("python");            // bare REPL
    expect_refused("node");              // bare REPL
    expect_refused("git commit");        // would open editor
    expect_refused("git rebase -i HEAD~3");

    // ── legitimate commands: allowed ────────────────────────────────────
    expect_allowed("python script.py");
    expect_allowed("python3 -c \"print(1)\"");
    expect_allowed("node build.js");
    expect_allowed("git commit -m \"fix\"");
    expect_allowed("git commit --amend --no-edit");
    expect_allowed("ls -la");
    expect_allowed("grep -rn foo src/");
    expect_allowed("curl https://example.com -o out.txt");   // no |sh
    expect_allowed("git push origin main");                  // no --force

    // ── out-of-the-box native-tool nudges (advisory, never blocks) ──────
    using mcp::tools::util::bash_tool_suggestion;
    auto nudges = [](std::string_view c) { return !bash_tool_suggestion(c).empty(); };
    // Bare file-inspection shell-outs get a tip toward the native tool.
    CHECK(nudges("cat src/main.cpp"));
    CHECK(nudges("sed -n '10,40p' file"));
    CHECK(nudges("head -50 log.txt"));
    CHECK(nudges("tail -20 log.txt"));
    CHECK(nudges("grep -rn foo src"));
    CHECK(nudges("find . -name '*.ts'"));
    CHECK(nudges("ls -la"));
    CHECK(nudges("wc -l file"));
    // The tip names the right replacement.
    CHECK(bash_tool_suggestion("sed -n '1,5p' f").find("symbol=") != std::string::npos);
    CHECK(bash_tool_suggestion("cat f").find("read") != std::string::npos);
    CHECK(bash_tool_suggestion("find . -name x").find("glob") != std::string::npos);
    // SILENT when the shell is doing real work: pipes, redirects, chaining,
    // substitution — bash is the right call there, no nudge.
    CHECK(!nudges("cat f | grep x"));
    CHECK(!nudges("grep x f > out.txt"));
    CHECK(!nudges("ls && echo done"));
    CHECK(!nudges("cat $(ls)"));
    CHECK(!nudges("wc -l < f"));
    // Non-inspection commands never nudge.
    CHECK(!nudges("python build.py"));
    CHECK(!nudges("git status"));
    CHECK(!nudges("make -j8"));

    CHECK(g_failures == 0);
}
