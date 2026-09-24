// SPDX-License-Identifier: Apache-2.0
#pragma once
// Bash guards: reject interactive commands (vim, bare python REPL, etc.)
// and a handful of flagrantly destructive patterns. Returns empty string
// when acceptable, otherwise a human-readable rejection reason.
//
// Plus native-tool detour detection — see Detour below.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::util {

[[nodiscard]] std::string validate_bash_command(std::string_view cmd);

// ── Native-tool detour detection ────────────────────────────────────────
//
// Models reach for `cat`/`sed -n`/`grep`/`ls` out of shell habit even when a
// native tool does the same job better. Detecting that is worth doing, but
// the FIRST version of this was a bare string-returning guesser, and a probe
// against a real session's shell-outs showed why that shape was wrong:
//
//   * it caught 4 of 10 real inspection shell-outs, and
//   * every single miss was a `|` or a `>`, because it bailed on the first
//     pipe character ("a pipe means bash is doing real work"), and
//   * it FALSELY matched `sed -i` (in-place edit) and `cat > f` (write) with
//     a READ suggestion.
//
// Both defects come from the same root: a `std::string` answer cannot say
// *what the command is doing*, only *what to print*. So a caller that wants
// to do anything stronger than print — deny it, rewrite it, bound it — has
// no safe signal to act on, and the "read" advice on `sed -i` becomes an
// unrecoverable bug the moment anyone acts on it.
//
// The fix is to return the ANALYSIS and let the caller pick the action.

// What the command is trying to DO. The read/write split is the safety
// boundary: a caller may only substitute a native READ for a shell read.
enum class Intent : std::uint8_t {
    Other,     // not a file-inspection detour — leave it alone
    ReadFile,  // cat / head / tail / sed -n Np  → `read`
    Search,    // grep / rg                      → `grep`
    FindFiles, // find                           → `glob`
    ListDir,   // ls                             → `list_dir`
    CountOnly, // wc -l / grep -c                → a tool's count mode
    GitRead,   // git log/status/diff/show/blame → git_log / git_status / …
    Write,     // cat > f, heredoc, sed -i, tee  → NEVER a read tool
};

// A `| head -N` / `| tail -N` tail stage is NOT "the shell doing real work" —
// it is a RESULT LIMIT, which every native tool already takes as a parameter.
// Treating it as shell work is what made the old detector miss most of its
// corpus, so the pipeline is parsed and a bounded tail is recorded here
// rather than used as a reason to stay silent.
struct Bound {
    int  limit = 0;      // N from `head -N` / `tail -N` (0 = unbounded)
    bool from_tail = false;  // it was `tail`, so the LAST N
};

struct Detour {
    Intent intent = Intent::Other;
    // The native tool that does this job ("read", "grep", "glob",
    // "list_dir"), or empty when there is no better tool.
    std::string_view tool;
    // Why the native tool is better, phrased for a model to act on.
    std::string      reason;
    // The exact parameter this command maps to, read off its own argv:
    // `head -50 f` → "limit: 50", `sed -n 10,40p f` → "start_line: 10,
    // end_line: 40", `grep -l` → "output: \"files\"". Empty when the
    // command has nothing more specific to say than `reason`.
    std::string      param;
    // One "tool param" entry per native step, in order, when a call chains
    // several inspections (`git log -3; git status`). The tip lists them so
    // the model sees each call it should have made. Advisory text only.
    std::vector<std::string> steps;
    // A bounded tail stage folded into the verdict (see Bound).
    std::optional<Bound> bound;
    // True when the shell is genuinely required: an unbounded pipe into a
    // real transform, a redirect to a file, command substitution, chaining.
    // A caller must never substitute a native tool for one of these.
    bool needs_shell = false;

    // Safe to answer with a native READ-ONLY tool. False for Write intents
    // and for anything that genuinely needs the shell — the single predicate
    // a caller should gate on, so the read/write mistake cannot be made by
    // forgetting a check.
    [[nodiscard]] bool substitutable() const noexcept {
        return !needs_shell && !tool.empty()
            && intent != Intent::Other && intent != Intent::Write;
    }
};

// Analyse a shell command for a native-tool detour. Pure; no I/O.
[[nodiscard]] Detour analyze_detour(std::string_view cmd);

// Soft, NON-blocking advisory built on analyze_detour(): a one-line nudge
// toward the native tool, or "" when the command is fine as-is. The bash
// tool prepends this to its output; it NEVER blocks execution.
[[nodiscard]] std::string bash_tool_suggestion(std::string_view cmd);
// Same, from a verdict already computed (so a caller that also logs the
// verdict parses the command once).
[[nodiscard]] std::string bash_tool_suggestion(const Detour& d);

} // namespace mcp::tools::util
