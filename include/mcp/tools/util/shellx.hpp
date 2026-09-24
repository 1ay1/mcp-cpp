// SPDX-License-Identifier: Apache-2.0
//
// shellx — understand a shell command before running it.
//
// One real parse (tree-sitter-bash) feeds two consumers:
//   * the GUARD, which judges every simple command in the script — including
//     the ones hidden in pipelines, `&&` chains, `$(…)`, control flow,
//     `bash -c '…'`, `xargs …` and `find -exec …` — for catastrophic effects;
//   * the ACTION translation, which says what the command is FOR (read this
//     file's lines 10-40, search for X under Y, list Z) so a host can show a
//     meaningful card, measure detours, suggest the native tool, and — when the
//     translation is exact — run it natively with identical output.
//
// Design rules:
//   * Fail closed. A script that doesn't parse cleanly still gets a guard pass
//     (on whatever the parser recovered, plus a raw-text fallback) and is never
//     translated. `Unknown` is always a correct answer; a wrong `Read` is not.
//   * Static knowledge is typed. A word is either `Lit` (its value is known
//     without running anything) or `Dyn` (depends on expansion). Code that
//     needs a literal has to prove it has one — there is no "string that is
//     probably a path".
//   * Values, not handles. `analyze()` returns a self-contained `Script` that
//     owns its strings; no tree or parser outlives the call.
//   * Cheap. One parser per thread, reused. Median real command: ~20 us.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mcp::tools::util::shellx {

// ── Words ────────────────────────────────────────────────────────────────

// A word whose value is known statically: quotes removed, backslash escapes
// applied, no expansion anywhere inside it.
struct Lit { std::string value; };

// A word that needs the shell to evaluate it. `text` is the source spelling
// (for display and for the guard's conservative checks, e.g. `$HOME/..`).
struct Dyn {
    std::string text;
    enum class Why : std::uint8_t {
        Variable,      // $X / ${X}
        Substitution,  // $(…) / `…` / <(…)
        Glob,          // * ? [ …  — value depends on the filesystem
        Tilde,         // ~ / ~user
        Arithmetic,    // $((…))
        Brace,         // {a,b}
    } why = Why::Variable;
};

using Word = std::variant<Lit, Dyn>;

[[nodiscard]] inline const std::string* lit(const Word& w) noexcept {
    if (auto* l = std::get_if<Lit>(&w)) return &l->value;
    return nullptr;
}
// Source spelling of either alternative.
[[nodiscard]] inline std::string_view spelling(const Word& w) noexcept {
    return std::visit([](const auto& x) -> std::string_view {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, Lit>) return x.value;
        else return x.text;
    }, w);
}

// ── Redirects ────────────────────────────────────────────────────────────

struct Redirect {
    enum class Kind : std::uint8_t {
        In,          // < file
        Out,         // > file / >| file
        Append,      // >> file
        OutErr,      // &> file / &>> file
        DupFd,       // 2>&1, >&2 — no file touched
        HereDoc,     // << EOF
        HereString,  // <<< word
    } kind = Kind::Out;
    int  fd = -1;          // explicit fd (2> …), -1 = default for the kind
    Word target;           // file / fd / heredoc delimiter
    [[nodiscard]] bool writes_file() const noexcept;   // Out/Append/OutErr, not /dev/null
};

// ── Commands ─────────────────────────────────────────────────────────────

// Where a simple command sits. Bits combine; the guard ignores them (every
// command is judged), the translator refuses anything not at top level.
enum class Ctx : std::uint16_t {
    None         = 0,
    Piped        = 1 << 0,  // not the first stage of its pipeline
    Substitution = 1 << 1,  // inside $(…) / `…` / <(…)
    Conditional  = 1 << 2,  // inside if/while/for/case
    Subshell     = 1 << 3,  // ( … )
    Group        = 1 << 4,  // { …; }
    Function     = 1 << 5,  // body of a function definition
    Background   = 1 << 6,  // … &
    Negated      = 1 << 7,  // ! cmd
    Nested       = 1 << 8,  // reached by unwrapping `bash -c`, `xargs`, `find -exec`, `sudo`…
};
[[nodiscard]] constexpr Ctx operator|(Ctx a, Ctx b) noexcept {
    return static_cast<Ctx>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool has(Ctx set, Ctx bit) noexcept {
    return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(bit)) != 0;
}

// How this command joins the NEXT one at the same level.
enum class Join : std::uint8_t { End, Seq /* ; or newline */, And, Or, Pipe, PipeErr /* |& */ };

struct Command {
    std::vector<std::pair<std::string, Word>> env;  // FOO=bar prefix assignments
    std::vector<Word>     argv;       // argv[0] is the program (as written)
    std::vector<Redirect> redirects;
    Ctx                   ctx  = Ctx::None;
    Join                  join = Join::End;
    int                   pipeline = 0;  // pipeline id; stages share it
    int                   stage    = 0;  // 0 = first stage of its pipeline
    std::uint32_t         begin = 0, end = 0;  // byte span in the ORIGINAL source
                                               // (for nested: span of the outermost carrier)

    // basename of argv[0] if it is a literal ("/usr/bin/rm" -> "rm")
    [[nodiscard]] std::string_view program() const noexcept;
    [[nodiscard]] bool fully_literal() const noexcept;   // argv + redirect targets all Lit
};

// ── Script ───────────────────────────────────────────────────────────────

struct Script {
    std::string          source;
    std::vector<Command> commands;   // every simple command, pre-order, incl. nested
    bool clean = false;      // parsed with no ERROR/MISSING nodes
    bool truncated = false;  // hit a size/depth bound; analysis is partial

    // Top-level only (ctx == None or only Piped), in source order.
    [[nodiscard]] std::vector<const Command*> top_level() const;
};

// Parse and lower. Never throws; a parse failure yields clean=false with
// whatever commands the parser recovered.
[[nodiscard]] Script analyze(std::string_view source);

// ── Guard ────────────────────────────────────────────────────────────────

struct Refusal {
    enum class Rule : std::uint8_t {
        RecursiveDeleteRoot, RecursiveDeleteHome, RecursiveDeleteCwd,
        FindDeleteWide, RecursivePermsWide,
        DiskWrite, Mkfs, ForkBomb, Power,
        ForcePush, CurlPipeShell,
        Interactive,
    } rule;
    std::string message;      // what the model sees
    std::uint32_t begin = 0, end = 0;   // span of the offending command in source
};

// First refusal found, or nullopt. Judges EVERY command in the script.
[[nodiscard]] std::optional<Refusal> guard(const Script& s);

// ── Actions ──────────────────────────────────────────────────────────────
//
// What a command is for, when that is a file-inspection intent a native tool
// answers. Everything else is `Other` (carries the program for display/
// telemetry). Paths are as written (Lit), resolved against a tracked `cd`.

struct LineRange {
    std::optional<std::int64_t> first;   // 1-based, inclusive
    std::optional<std::int64_t> last;    // inclusive; nullopt = to EOF
    bool from_end = false;               // tail: `first` counts from the end
    bool clamp_inverted = false;         // sed 'A,Bp' with B<A prints line A
};

struct ReadAction   { std::string path; LineRange range; };
struct SearchAction {
    std::string pattern;
    std::vector<std::string> paths;      // empty = cwd
    bool regex = true, ignore_case = false, word = false, fixed = false;
    bool recursive = false, line_numbers = false, count = false, files_only = false;
    bool extended = false;               // -E (ERE syntax); false = BRE
    int  context_before = 0, context_after = 0;
    std::vector<std::string> include_globs;
};
struct ListAction   { std::string path; bool long_format = false, all = false, recursive = false;
                      std::optional<std::string> name_glob; std::optional<char> type; };
struct GitReadAction{ std::string sub; std::vector<std::string> args; };
struct OtherAction  { std::string program; };

using Action = std::variant<ReadAction, SearchAction, ListAction, GitReadAction, OtherAction>;

// A formatting stage after the action (… | head -20). Modelled stages have a
// typed meaning; anything else is opaque (still fine for display, blocks exact
// native execution).
struct Shape {
    enum class Kind : std::uint8_t { Head, Tail, CountLines, Sort, Uniq, Filter, Opaque } kind;
    std::int64_t n = 0;          // Head/Tail count
    std::string  arg;            // Filter pattern / Opaque program
};

struct Step {
    Action action;
    std::vector<Shape> shapes;   // downstream formatting stages, in order
    bool exact = false;          // action + every shape modelled, all literal, no redirect/ctx
    std::uint32_t begin = 0, end = 0;
};

struct Plan {
    std::vector<Step> steps;     // one per top-level pipeline that is not a no-op
    // Every non-noop step is an inspection action (Read/Search/List/GitRead)?
    [[nodiscard]] bool pure_inspection() const noexcept;
    [[nodiscard]] bool all_exact() const noexcept;
};

[[nodiscard]] Plan plan(const Script& s);

// Short human label for a step ("Read src/x.cpp:10-40", "Search `foo` in src")
[[nodiscard]] std::string describe(const Step& st);
// The equivalent native tool call for a typed step, spelled as the model
// would write it: `read path=src/x.cpp start_line=10 end_line=40`. Empty for
// Other / git (git_* tools don't take shell argv). Head/Tail shapes fold
// into the call's own bound (limit / offset) where the tool supports one.
[[nodiscard]] std::string native_call(const Step& st);
// Telemetry category of a whole plan: read|search|list|git|mixed|other|none
[[nodiscard]] std::string_view category(const Plan& p) noexcept;

// ── Native execution ──────────────────────────────────────────────────────────
//
// Run a whole command in-process when EVERY step is an exact read shape this
// layer implements byte-for-byte (today: `sed -n A,Bp F`, `cat F`,
// `head -N F`, `tail -N F`, `tail -n +K F`, optionally piped through
// `head -N` / `tail -N` / `wc -l`), producing exactly the stdout+stderr and
// exit status the shell would. Anything else — a flag, a glob, a dynamic
// word, a missing file whose error text we'd have to imitate, a binary file,
// a step we don't implement — returns nullopt and the caller runs the real
// shell. Declining is always correct; answering differently never is.
struct NativeResult {
    std::string output;   // stdout + stderr, in the order the shell prints them
    int exit_code = 0;
};
[[nodiscard]] std::optional<NativeResult> native_run(std::string_view command,
                                                     std::string_view cwd);

} // namespace mcp::tools::util::shellx
