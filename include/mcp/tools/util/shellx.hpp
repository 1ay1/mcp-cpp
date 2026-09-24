// SPDX-License-Identifier: Apache-2.0
//
// shellx — understand a shell command before running it.
//
// One real parse (tree-sitter-bash), two consumers:
//   * the GUARD, which judges every simple command in the script — including
//     the ones hidden in pipelines, `&&` chains, `$(…)`, control flow,
//     `bash -c '…'`, `xargs …` and `find -exec …` — for catastrophic effects;
//   * analyze_detour (bash_validate), which reads the parse to build an
//     ADVISORY tip. Nothing here ever runs a native tool in place of the
//     shell: a tip is survivable, acting on a verdict is not.
//
// Design rules:
//   * Fail closed. A script that doesn't parse cleanly still gets a guard pass
//     (on whatever the parser recovered, plus a raw-text fallback) and no
//     advice.
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
// command is judged), analyze_detour only advises on top-level commands.
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

} // namespace mcp::tools::util::shellx
