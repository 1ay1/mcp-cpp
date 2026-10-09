#pragma once
// Shared filesystem helpers. Tool implementations need normalized paths,
// binary detection, and a predictable "which directories to skip during
// traversal" list — centralised here so the rules are consistent across
// grep / glob / list_dir / find_definition.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

#include <mcp/tools/state.hpp>        // FileSnapshot
#include <mcp/tools/util/error.hpp>   // ToolError + factories

namespace mcp::tools::util {

namespace fs = std::filesystem;

// Where a tool may reach: the workspace boundary (canonical) and the extra
// directories a read may also reach (skills). A value: tool bodies copy it
// out of the shared state and check paths against it with no lock held.
struct Bounds {
    fs::path              workspace;
    std::vector<fs::path> read_roots;
    fs::path              home;   // what `~` expands to; empty: no expansion
};

// A Bounds from the state's fields. An empty workspace means the process cwd.
[[nodiscard]] Bounds bounds_from(const fs::path& workspace, std::vector<fs::path> read_roots,
                                 fs::path home = {});
// Canonicalise a root to store in the state (keeps the input if it can't).
[[nodiscard]] fs::path canonical_root(fs::path root);

// Forward declarations — WorkspacePath sits below ToolError-using factories.
class WorkspacePath;

// Read an entire file as a binary blob. Returns "" on open failure (callers
// that need to distinguish missing vs empty should stat first).
//
// Two overloads: the `WorkspacePath` form is the workspace-checked entry
// the model-facing tools should use; the `fs::path` form is the
// unchecked escape hatch for paths the runtime owns (credentials,
// thread persistence, memory store under ~/.agentty/). Routing through
// a typed parameter keeps the boundary visible at the call site —
// reviewers can see at a glance which form a tool picked.
[[nodiscard]] std::string read_file(const fs::path& p);
[[nodiscard]] std::string read_file(const WorkspacePath& p);

// Write content atomically-ish (truncate + write + flush). Returns the
// empty string on success, or a human-readable error otherwise. Keeps tool
// lambdas terse while still forcing callers to surface failures.
//
// See `read_file` above for the WorkspacePath vs fs::path overload split.
[[nodiscard]] std::string write_file(const fs::path& p, std::string_view content);
[[nodiscard]] std::string write_file(const WorkspacePath& p, std::string_view content);

// Normalise a user-supplied path. Accepts forward slashes on Windows
// (the model frequently produces them), strips surrounding whitespace and
// quotes, and returns an absolute path relative to cwd when not already
// absolute — so error messages name an unambiguous location.
[[nodiscard]] fs::path normalize_path(std::string_view s, const Bounds& b);

// Strong typedef for an already-normalised filesystem path. The only way
// to construct one is from a raw string via `NormalizedPath{"..."}`, which
// calls `normalize_path` — so "did I already normalize this?" is answered
// by the type. Passed by value (cheap: holds a single fs::path).
struct NormalizedPath {
    fs::path value;

    NormalizedPath(std::string_view raw, const Bounds& b) : value(normalize_path(raw, b)) {}

    [[nodiscard]] const fs::path& path() const noexcept { return value; }
    [[nodiscard]] std::string string() const { return value.string(); }
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};

// ── Workspace boundary ──────────────────────────────────────────────────
// Every filesystem-touching tool refuses paths outside the boundary in its
// Bounds (the host sets it in the tool state: from the cwd, or --workspace).
//
// The boundary is the simplest sandbox layer: it doesn't stop a model
// from running shell commands that walk anywhere, but it does stop the
// fast path of "model casually `read`s ~/.ssh/id_rsa or `write`s to
// /etc/hosts". Pair it with an OS sandbox for defense in depth.

// The ACTIVE PROJECT directory: the process cwd (the directory the user
// launched agentty in), clamped to stay inside the access boundary. This
// is distinct from the workspace boundary, which is the widenable ACCESS
// BOUNDARY (`--workspace /` opens the whole disk). Relative tool paths and
// repo-scoped defaults resolve from HERE, not the boundary, so that
// `read src/foo.cpp` under `--workspace /` still lands in the project the
// user launched in rather than at `/src/foo.cpp`. Falls back to the
// boundary only when the cwd is unusable or escapes it (an unusual launch
// from outside a wider `-w` scope). Computed fresh each call (cheap: one
// current_path() + canonicalise) since agentty never chdir's but tool
// worker threads or embedders theoretically could.
[[nodiscard]] fs::path project_root(const Bounds& b);

// True if `target` is at-or-under the workspace root after canonicalising
// both sides. Symlink escape is blocked: a link inside the workspace that
// points to /etc would resolve to /etc and fail the prefix check. Uses
// weakly_canonical so a not-yet-existing path (e.g. write target) is
// still checked correctly against its existing parent components.
[[nodiscard]] bool is_within_workspace(const fs::path& target, const Bounds& b);

// Construct a NormalizedPath that's been workspace-checked in one shot.
// Tools call:
//     auto p = util::make_workspace_path(*raw, "read", bounds);
//     if (!p) return std::unexpected(p.error());
// `tool_name` only appears in the error message and is purely cosmetic.
[[nodiscard]] std::expected<struct NormalizedPath, ToolError>
make_workspace_path(std::string_view raw, std::string_view tool_name, const Bounds& b);

// ── WorkspacePath ───────────────────────────────────────────
// A NormalizedPath that carries a *type-level* proof of workspace
// containment. The only public way to obtain one is through
// `WorkspacePath::checked` (or the gated builder factories below),
// which delegate to is_within_workspace(). Once a function accepts
// `const WorkspacePath&`, reviewers know the containment check has
// already happened — forgetting it is a compile error, not a missing
// runtime gate.
//
// Today's tools already route every fs path through make_workspace_path
// before any IO; this type makes that discipline a property of the type
// system instead of a code-review convention. New fs APIs should accept
// `WorkspacePath`; only the runtime's own non-workspace paths (under
// ~/.agentty/ for threads/memory/credentials) bypass it via the
// `fs::path` overloads of read_file/write_file.
class WorkspacePath {
    NormalizedPath inner_;
    // Private; only the factory friends can mint one. No public ctor =
    // no way to skip the containment check.
    explicit WorkspacePath(NormalizedPath n) noexcept : inner_(std::move(n)) {}

    friend std::expected<WorkspacePath, ToolError>
        make_workspace_path_checked(std::string_view, std::string_view, const Bounds&);
    friend std::expected<WorkspacePath, ToolError>
        promote_to_workspace_path(NormalizedPath, std::string_view, const Bounds&);
    friend std::expected<WorkspacePath, ToolError>
        make_readable_path_checked(std::string_view, std::string_view, const Bounds&);

public:
    [[nodiscard]] const fs::path&   path()   const noexcept { return inner_.path(); }
    [[nodiscard]] std::string       string() const          { return inner_.string(); }
    [[nodiscard]] bool              empty()  const noexcept { return inner_.empty(); }
    [[nodiscard]] const NormalizedPath& normalized() const noexcept { return inner_; }
};

// Workspace-checked factory. Same contract as make_workspace_path but
// yields a WorkspacePath instead of a NormalizedPath — use this in new
// code so the gate's success travels with the value.
[[nodiscard]] std::expected<WorkspacePath, ToolError>
make_workspace_path_checked(std::string_view raw, std::string_view tool_name, const Bounds& b);

// ── Read-only allowlist roots ───────────────────────────────────────────
// Skill directories (agentskills.io tier-3 resources) may live OUTSIDE
// the workspace (~/.agentty/skills/, ~/.agents/skills/). The spec's
// client-implementation guidance: allowlist skill directories for reads
// so the model can fetch bundled scripts/references without the
// boundary refusing them. The allowlist is READ-ONLY by construction —
// only `make_readable_path_checked` consults it; the write/edit gates
// (make_workspace_path_checked) never do, so an allowlisted root can't
// become a write escape.
//
// Add `root` (canonicalised) to a read-roots list, once. The host calls it
// on the state's list when the skills scanner finds a directory.
void allow_read_root(std::vector<fs::path>& roots, const fs::path& root);

// True when `target` sits under one of b's read roots (post-
// canonicalisation, symlink-escape checked like the workspace).
[[nodiscard]] bool is_read_allowlisted(const fs::path& target, const Bounds& b);

// Read-gate factory: passes when the path is within the workspace OR
// under a read-allowlist root. Use ONLY in read-side tools (`read`).
[[nodiscard]] std::expected<WorkspacePath, ToolError>
make_readable_path_checked(std::string_view raw, std::string_view tool_name, const Bounds& b);

// Promote an already-normalised path through the containment gate.
// Useful when the caller composed a NormalizedPath itself (e.g.
// resolving an attachment path against the workspace) and now
// wants the typed proof.
[[nodiscard]] std::expected<WorkspacePath, ToolError>
promote_to_workspace_path(NormalizedPath p, std::string_view tool_name, const Bounds& b);

// True for directory names we want recursive traversals (grep / glob /
// list_dir) to skip by default. Keeps the skip list in one place so tools
// stay in sync (e.g. adding `_deps` to every tool at once).
[[nodiscard]] bool should_skip_dir(std::string_view name) noexcept;

// The same skip set rendered as ripgrep exclude-glob arguments
// (`-g`, `!node_modules`, `-g`, `!build*`, …). Appended to the `rg`
// argv so the ripgrep-backed grep / find_definition PRUNE the same
// directories the built-in walker prunes via should_skip_dir(), instead
// of relying on ripgrep to stat + gitignore-check every build/vendor
// file (a large cold-cache cost, and wrong entirely when there's no
// .gitignore). Returned as flat argv pairs so the caller can splice them
// straight into its argument vector.
[[nodiscard]] const std::vector<std::string>& skip_dir_rg_globs();

// Heuristic: scan the first 512 bytes for a NUL. Good enough to avoid
// grep'ing PNGs / executables / model weights into the prompt.
[[nodiscard]] bool is_binary_file(const fs::path& p);

// Identify an image file by its magic bytes (PNG/JPEG/GIF/WebP) — returns the
// MIME type, or "" if it isn't a recognised image. Extension-agnostic. Lets
// `read` hand an image to a vision model instead of refusing it as binary.
[[nodiscard]] std::string sniff_image_media_type(const fs::path& p);

// ── Per-file state cache ────────────────────────────────────────────────
// Shared across tools so edit/write can detect "the file changed since the
// model last looked at it". Keyed on canonical path; value is the mtime
// the tool saw plus a content-fingerprint hash so we catch sub-second
// edits that don't bump mtime (some filesystems have 1 s mtime resolution).
//
// Records are written by `read` after every successful read, and by
// `write` / `edit` after a successful mutation (so the next call from
// the same session sees the new state and doesn't false-alarm on its
// own change). Lookups never block on IO — the cache is purely an
// in-memory hint surface.
//
// The snapshots live in the tool state (ToolState::files), which the host
// owns for as long as it likes.
using FileSnapshot = ::mcp::tools::state::FileSnapshot;

// The key a file's snapshot is stored under in the tool state (its
// canonical path; works for a file that doesn't exist yet).
[[nodiscard]] std::string snapshot_key(const fs::path& path) noexcept;

// Compute FNV-1a 64-bit over a byte range. Inlineable; used by tools
// that have already read the file to record its hash in the snapshot.
// FNV-1a was picked over xxHash because it's branch-free, zero-alloc,
// and pulls in no dependencies — collision risk at 64 bits across one
// session's worth of files is negligible.
//
// COST: it is a serial multiply chain, one byte at a time — measured
// 1.04 GB/s, i.e. ~0.9 ms for a 900 KiB file. That is not free, so do not
// call it on a whole file just to fill in a snapshot field. Every current
// caller goes through cheap_content_hash() below; see the note there for
// why, and read that before adding a call to this one.
[[nodiscard]] inline std::uint64_t content_fnv1a(std::string_view bytes) noexcept {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x00000100000001b3ULL;
    std::uint64_t h = kOffset;
    for (char ch : bytes) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= kPrime;
    }
    return h;
}

// The hash actually stored in a FileSnapshot.
//
// staleness_of() decides Fresh/Stale from (mtime, size) ALONE — it never
// looks at the stored hash, and nothing else in the tree reads that field
// either (checked: 5 write sites, 0 readers). So hashing every byte of
// every file the tools touch was pure overhead: ~0.9 ms on a 900 KiB read,
// which was 72% of what the read cost after the line loop was fixed.
//
// Keep the field — it is the hook for a future content-level staleness
// check, and callers already pass it — but pay for it in proportion to what
// it can currently detect. Hash the HEAD and TAIL plus the length: O(1),
// catches the in-place edits an mtime-preserving tool would hide, and is
// exactly as strong as "we sampled the file" claims to be. If a real
// content-equality consumer ever appears, it must hash the full bytes
// ITSELF at that call site, where the cost is visible and justified.
[[nodiscard]] inline std::uint64_t cheap_content_hash(std::string_view bytes) noexcept {
    constexpr std::size_t kSample = 4096;
    if (bytes.size() <= 2 * kSample)
        return content_fnv1a(bytes);
    std::uint64_t h = content_fnv1a(bytes.substr(0, kSample));
    h ^= content_fnv1a(bytes.substr(bytes.size() - kSample));
    // Fold the length in so a pure insertion in the middle still moves it.
    h ^= static_cast<std::uint64_t>(bytes.size()) * 0x9e3779b97f4a7c15ULL;
    return h;
}

// Staleness classification. Computed by checking the file's current
// (mtime, size) and optionally content hash against the cached snapshot.
enum class StaleVerdict : std::uint8_t {
    Unknown,      // no prior snapshot — caller decides what to do
    Fresh,        // snapshot matches current on-disk state
    Stale,        // file changed since the snapshot was recorded
};

// Check whether `path` looks stale relative to its last snapshot.
// Stat-based (cheap): compares mtime + size. Returns Unknown when no
// snapshot exists or stat fails. For a stronger guarantee, the caller
// can additionally hash the file's current bytes and compare against
// the snapshot's `content_hash`.
// `snap` is what the state holds for path (nullopt: never seen).
[[nodiscard]] StaleVerdict staleness_of(const fs::path& path,
                                        const std::optional<FileSnapshot>& snap) noexcept;

} // namespace mcp::tools::util
