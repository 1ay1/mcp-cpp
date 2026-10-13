#include <mcp/tools/util/fs_helpers.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>


namespace mcp::tools::util {

std::string read_file(const FileSystem& files, const fs::path& p) {
    auto r = files.read(p);
    return r ? std::move(*r) : std::string{};
}

std::string write_file(FileSystem& files, const fs::path& p, std::string_view content) {
    return files.write_atomic(p, content);
}

fs::path normalize_path(std::string_view s, const Bounds& b) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))  s.remove_suffix(1);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                          || (s.front() == '\'' && s.back() == '\'')
                          || (s.front() == '`' && s.back() == '`'))) {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }
    // Tilde expansion. The model writes paths the way a shell would
    // (`~/projects/foo`); without expansion they round-trip into
    // `<cwd>/~/projects/foo`, which doesn't exist, and the tool either
    // returns NotFound or — on filesystems that handle the bizarre
    // prefix slowly — sits on stat() long enough to hit the watchdog.
    // The shell convention: bare `~` or `~/...` expands to $HOME.
    // (`~user/...` per-user expansion isn't supported here; we'd need
    // getpwnam_r for that and the model's never seen it work.)
    std::string expanded;
    if (!s.empty() && s.front() == '~' && (s.size() == 1 || s[1] == '/')) {
        // The host says where home is (ToolState::home); without it `~`
        // stays literal.
        if (!b.home.empty()) {
            expanded = b.home.string();
            expanded.append(s.data() + 1, s.size() - 1);
            s = expanded;
        }
    }
    fs::path p{s};
    if (!p.is_absolute()) {
        // Resolve relative to the ACTIVE PROJECT (the process cwd clamped
        // inside the access boundary), NOT the raw access boundary and NOT
        // the tool thread's cwd. Three roots are in play:
        //   * workspace_root() — the widenable ACCESS BOUNDARY (`-w /`).
        //   * project_root()   — the cwd the user launched in (the project).
        //   * a worker thread's cwd — may differ (process_start cwd=".").
        // The tool schemas promise "relative to the workspace root", and by
        // default project_root() == workspace_root() so nothing changes. But
        // when the user widens the boundary (`--workspace /`), anchoring to
        // the boundary would turn `read src/foo.cpp` into `/src/foo.cpp`;
        // anchoring to project_root() keeps it in the launched project. The
        // result is still containment-checked against the boundary by the
        // caller, so this only ever narrows where a relative path lands.
        p = project_root(b) / p;
    }
    return p.lexically_normal();
}

Bounds bounds_from(FileSystem& files, const fs::path& workspace,
                   std::vector<fs::path> read_roots, fs::path home) {
    Bounds b;
    b.files = &files;
    if (workspace.empty()) {
        std::error_code ec;
        auto cwd = files.cwd();
        b.workspace = cwd.empty() ? fs::path{"/"} : files.canonical(cwd, ec);
        if (ec || b.workspace.empty()) b.workspace = cwd.empty() ? fs::path{"/"} : cwd;
    } else {
        b.workspace = workspace;
    }
    b.read_roots = std::move(read_roots);
    b.home       = std::move(home);
    return b;
}

fs::path canonical_root(const FileSystem& files, fs::path root) {
    std::error_code ec;
    auto canon = files.canonical(root, ec);
    return ec ? std::move(root) : std::move(canon);
}

TidyPath tidy_model_path(const FileSystem& files, const Bounds& b, std::string raw) {
    TidyPath out;
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
    std::size_t lead = 0;
    while (lead < raw.size() && std::isspace(static_cast<unsigned char>(raw[lead]))) ++lead;
    raw.erase(0, lead);
    if (raw.size() >= 2 && raw.front() == raw.back()
        && (raw.front() == '`' || raw.front() == '"' || raw.front() == '\''))
        raw = raw.substr(1, raw.size() - 2);
    out.path = raw;
    if (raw.empty() || files.stat(normalize_path(raw, b)).exists) return out;

    // Peel "#L10-L20", ":10-20", ":10:5" or ":10" off the end.
    auto num = [](std::string_view s, std::size_t& i) {
        int v = 0; std::size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
        return i > start ? v : -1;
    };
    for (std::size_t cut = raw.size(); cut-- > 0;) {
        const char c = raw[cut];
        if (c != ':' && c != '#') continue;
        std::string_view tail{raw.data() + cut + 1, raw.size() - cut - 1};
        std::size_t i = 0;
        if (c == '#' && i < tail.size() && (tail[i] == 'L' || tail[i] == 'l')) ++i;
        const int a = num(tail, i);
        if (a < 0) continue;
        int z = 0;
        if (i < tail.size() && (tail[i] == '-' || tail[i] == ':' || tail[i] == ',')) {
            const bool range = tail[i] == '-' || tail[i] == ',';
            ++i;
            if (i < tail.size() && (tail[i] == 'L' || tail[i] == 'l')) ++i;
            const int v = num(tail, i);
            if (v < 0) continue;
            if (range) z = v;   // ":10:5" is line:column
        }
        if (i != tail.size()) continue;
        std::string base = raw.substr(0, cut);
        if (base.empty() || !files.stat(normalize_path(base, b)).exists) continue;
        out.path = std::move(base);
        out.line_from = a;
        out.line_to = z >= a ? z : 0;
        break;
    }
    return out;
}

namespace {
// Component-wise "child is at-or-under parent". Both sides must already be
// canonical/normalised. Shared shape with is_within_workspace()'s prefix
// walk and git.cpp's path_under.
[[nodiscard]] bool path_at_or_under(const fs::path& child,
                                    const fs::path& parent) noexcept {
    auto pt = parent.begin();
    auto ct = child.begin();
    for (; pt != parent.end() && ct != child.end(); ++pt, ++ct)
        if (*pt != *ct) return false;
    return pt == parent.end();
}
} // namespace

fs::path project_root(const Bounds& b) {
    const fs::path& ws = b.workspace;   // already canonicalised
    if (!b.project.empty()) return b.project;   // the host said where
    std::error_code ec;
    fs::path cwd = b.files->cwd();
    if (cwd.empty()) return ws;
    fs::path cwdc = b.files->canonical(cwd, ec);
    if (ec || cwdc.empty()) cwdc = cwd;
    // The cwd is the active project ONLY if it sits inside the access
    // boundary. If the user launched outside a wider `-w` scope (unusual),
    // fall back to the boundary so we never resolve relative paths to a
    // location the tools would then reject as out-of-workspace.
    return path_at_or_under(cwdc, ws) ? cwdc : ws;
}

bool is_within_workspace(const fs::path& target, const Bounds& b) {
    if (target.empty()) return false;
    std::error_code ec;
    // weakly_canonical resolves what exists and leaves the rest as-is —
    // perfect for write targets where the file doesn't exist yet but the
    // parent directory does, and it resolves symlinks so an in-workspace
    // link pointing at /etc fails the prefix check.
    auto canon_target = b.files->canonical(target, ec);
    if (ec) {
        // weakly_canonical failed (e.g. a parent component is a dangling
        // symlink, or EACCES walking the chain). Do NOT fall back to a
        // plain absolute() — that skips symlink resolution and would let
        // a crafted link escape the boundary. Retry on the parent dir
        // (the common write-target case: file doesn't exist yet but the
        // directory does, and canonicalising it still resolves symlinks),
        // then re-attach the filename. If even that fails, deny.
        std::error_code pec;
        auto parent = b.files->canonical(target.parent_path(), pec);
        if (pec || parent.empty()) return false;
        canon_target = parent / target.filename();
    }
    const auto& canon_root = b.workspace;   // already canonicalised
    // Component-wise prefix check. Plain string startsWith would let
    // /home/user/project-other through when root is /home/user/project.
    if (path_at_or_under(canon_target, canon_root)) return true;
    for (const auto& r : b.extra_roots)
        if (path_at_or_under(canon_target, r)) return true;
    return false;
}

// ── Read-only allowlist roots ───────────────────────────────────────────
namespace {

// Component-wise "target under root" — same shape as the workspace prefix
// check above; both gates share the symlink-safe canonicalisation rules.
[[nodiscard]] bool under_root(const fs::path& canon_target, const fs::path& root) {
    auto rt = root.begin();
    auto tt = canon_target.begin();
    for (; rt != root.end() && tt != canon_target.end(); ++rt, ++tt)
        if (*rt != *tt) return false;
    return rt == root.end();
}

} // namespace

void allow_read_root(const FileSystem& files, std::vector<fs::path>& roots, const fs::path& root) {
    std::error_code ec;
    auto canon = files.canonical(root, ec);
    if (ec || canon.empty()) return;
    if (std::find(roots.begin(), roots.end(), canon) == roots.end())
        roots.push_back(std::move(canon));
}

bool is_read_allowlisted(const fs::path& target, const Bounds& b) {
    if (target.empty()) return false;
    std::error_code ec;
    auto canon = b.files->canonical(target, ec);
    if (ec) return false;   // reads need the file to exist; no parent retry
    for (const auto& r : b.read_roots)
        if (under_root(canon, r)) return true;
    return false;
}

std::expected<NormalizedPath, ToolError>
make_workspace_path(std::string_view raw, std::string_view tool_name, const Bounds& b) {
    NormalizedPath p{raw, b};
    if (!is_within_workspace(p.path(), b)) {
        // Helpful, actionable message: name the offending path, the
        // active root, and the two ways out (restart in a wider dir or
        // pass --workspace). The model can read this and either ask the
        // user to widen the scope or pick a different path.
        return std::unexpected(ToolError::out_of_workspace(
            "tool '" + std::string{tool_name} + "' refused: '"
            + p.string() + "' is outside the workspace root '"
            + b.workspace.string() + "'. "
            "Restart agentty in a parent directory or pass "
            "--workspace <dir> to widen the scope."));
    }
    return p;
}

// ── WorkspacePath factories ───────────────────────────────────────
std::expected<WorkspacePath, ToolError>
make_workspace_path_checked(std::string_view raw, std::string_view tool_name, const Bounds& b) {
    auto np = make_workspace_path(raw, tool_name, b);
    if (!np) return std::unexpected(std::move(np.error()));
    return WorkspacePath{std::move(*np)};
}

std::expected<WorkspacePath, ToolError>
promote_to_workspace_path(NormalizedPath p, std::string_view tool_name, const Bounds& b) {
    if (!is_within_workspace(p.path(), b)) {
        return std::unexpected(ToolError::out_of_workspace(
            "tool '" + std::string{tool_name} + "' refused: '"
            + p.string() + "' is outside the workspace root '"
            + b.workspace.string() + "'."));
    }
    return WorkspacePath{std::move(p)};
}

std::expected<WorkspacePath, ToolError>
make_readable_path_checked(std::string_view raw, std::string_view tool_name, const Bounds& b) {
    NormalizedPath p{raw, b};
    if (is_within_workspace(p.path(), b) || is_read_allowlisted(p.path(), b))
        return WorkspacePath{std::move(p)};
    // Name the ACTUAL readable roots. Observed failure: skills live in
    // ~/.agents/skills but the model GUESSED ~/.agentty/skills/<name> (a
    // sibling dialect root that happens not to exist) and the old message
    // ("not under any skill directory") gave it nothing to correct with —
    // it either retried the same wrong path or gave up. Listing the
    // allowlisted roots turns the refusal into a one-shot redirect.
    std::string msg = "tool '" + std::string{tool_name} + "' refused: '"
        + p.string() + "' is outside the workspace root '"
        + b.workspace.string() + "' and not under any skill "
        "directory.";
    {
        if (!b.read_roots.empty()) {
            msg += " Readable skill directories:";
            for (const auto& r : b.read_roots) {
                msg += ' ';
                msg += r.string();
                msg += ';';
            }
            msg.back() = '.';
            msg += " Use the exact directory listed (skills resolve "
                   "against several roots — do not guess a sibling "
                   "root).";
        } else {
            msg += " Restart agentty in a parent directory or pass "
                   "--workspace <dir> to widen the scope.";
        }
    }
    return std::unexpected(ToolError::out_of_workspace(std::move(msg)));
}

// ── Checked read/write delegates ────────────────────────────────
std::string read_file(const WorkspacePath& p) {
    return read_file(p.files(), p.path());
}

std::string write_file(const WorkspacePath& p, std::string_view content) {
    return write_file(p.files(), p.path(), content);
}

bool should_skip_dir(std::string_view name) noexcept {
    static const std::vector<std::string_view> skip = {
        ".git", "node_modules", "build", "target", "__pycache__",
        ".cache", "vendor", "dist", "out", ".next", ".venv",
        "cmake-build-debug", "cmake-build-release", ".idea", ".vscode",
        "_deps", "third_party", "thirdparty", "3rdparty", "external",
        ".mypy_cache", ".pytest_cache", ".tox", ".gradle", "Pods",
        "bazel-bin", "bazel-out", "DerivedData", ".terraform",
    };
    for (auto s : skip) if (name == s) return true;
    // Prefix families: out-of-source CMake build trees are conventionally
    // named build/, build-*, cmake-build-* (CLion), and they are FULL of
    // generated + fetched (_deps) source that would otherwise flood the
    // repo map / grep / find_definition walks with thousands of non-project
    // files. Match the whole family, not just the bare "build" name.
    if (name.starts_with("build-") || name.starts_with("cmake-build"))
        return true;
    return false;
}

const std::vector<std::string>& skip_dir_rg_globs() {
    // Built once from the SAME skip set should_skip_dir() enforces, so the
    // ripgrep-backed and built-in walkers prune identical directories. The
    // "build*" / "cmake-build*" prefix families are matched with a trailing
    // `*` glob to cover build-asan / cmake-build-debug / etc. Each entry is
    // emitted as an argv PAIR ("-g", "!<name>") ready to splice into the rg
    // command line. `!` makes it an exclude; a bare directory name in a glob
    // matches that directory at any depth, which is exactly should_skip_dir's
    // filename-only semantics.
    static const std::vector<std::string> globs = [] {
        static constexpr std::string_view names[] = {
            ".git", "node_modules", "target", "__pycache__",
            ".cache", "vendor", "dist", "out", ".next", ".venv",
            ".idea", ".vscode", "_deps", "third_party", "thirdparty",
            "3rdparty", "external", ".mypy_cache", ".pytest_cache", ".tox",
            ".gradle", "Pods", "bazel-bin", "bazel-out", "DerivedData",
            ".terraform",
        };
        std::vector<std::string> v;
        v.reserve(std::size(names) * 2 + 4);
        for (auto n : names) {
            v.emplace_back("-g");
            v.emplace_back("!" + std::string{n});
        }
        // Prefix families (build, build-*, cmake-build*): one glob each
        // covers the bare name and every suffixed sibling.
        v.emplace_back("-g"); v.emplace_back("!build*");
        v.emplace_back("-g"); v.emplace_back("!cmake-build*");
        return v;
    }();
    return globs;
}

bool is_binary_file(const FileSystem& files, const fs::path& p) {
    const auto head = files.read_prefix(p, 512);
    // Can't open (permission denied, transient lock, race-deleted): NOT
    // a binary verdict. Returning `true` here made grep/glob silently
    // skip the file and made read/edit report a bogus "cannot read
    // binary file". Return `false` so the caller's real read runs and
    // surfaces the actual open error instead of masking it.
    if (!head) return false;
    return head->find('\0') != std::string::npos;
}

// Identify an image file by its MAGIC BYTES (never the extension — a model may
// `read` a screenshot saved with no/odd suffix). Returns the MIME type when the
// header matches a format vision models accept, else "". Covers PNG, JPEG, GIF,
// and WebP — the same set the composer's paste path recognises.
std::string sniff_image_media_type(const FileSystem& files, const fs::path& p) {
    const auto head = files.read_prefix(p, 16);
    if (!head) return {};
    unsigned char h[16] = {0};
    const auto n = static_cast<std::ptrdiff_t>(std::min<std::size_t>(head->size(), sizeof(h)));
    for (std::ptrdiff_t i = 0; i < n; ++i) h[i] = static_cast<unsigned char>((*head)[static_cast<std::size_t>(i)]);
    if (n < 4) return {};
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (n >= 8 && h[0]==0x89 && h[1]=='P' && h[2]=='N' && h[3]=='G' &&
        h[4]==0x0D && h[5]==0x0A && h[6]==0x1A && h[7]==0x0A)
        return "image/png";
    // JPEG: FF D8 FF
    if (h[0]==0xFF && h[1]==0xD8 && h[2]==0xFF)
        return "image/jpeg";
    // GIF: "GIF87a" / "GIF89a"
    if (n >= 6 && h[0]=='G' && h[1]=='I' && h[2]=='F' && h[3]=='8' &&
        (h[4]=='7' || h[4]=='9') && h[5]=='a')
        return "image/gif";
    // WebP: "RIFF"????"WEBP"
    if (n >= 12 && h[0]=='R' && h[1]=='I' && h[2]=='F' && h[3]=='F' &&
        h[8]=='W' && h[9]=='E' && h[10]=='B' && h[11]=='P')
        return "image/webp";
    return {};
}

// ── File snapshots ──────────────────────────────────────────────────────────
namespace {

// Canonicalise without requiring the file to exist (write target might
// not exist yet at the moment we record). Falls back to the lexically-
// normal absolute form when weakly_canonical errors out — we still get
// a stable key per file across calls, just not symlink-aware.
[[nodiscard]] std::string canon_key(const FileSystem& files, const fs::path& p) noexcept {
    std::error_code ec;
    auto canon = files.canonical(p, ec);
    if (!ec) return canon.string();
    if (p.is_absolute()) return p.lexically_normal().string();
    const auto cwd = files.cwd();
    if (!cwd.empty()) return (cwd / p).lexically_normal().string();
    return p.string();
}

} // namespace

std::string snapshot_key(const FileSystem& files, const fs::path& path) noexcept { return canon_key(files, path); }

StaleVerdict staleness_of(const FileSystem& files, const fs::path& path,
                          const std::optional<FileSnapshot>& snap) noexcept {
    if (!snap) return StaleVerdict::Unknown;
    const auto st = files.stat(path);
    if (!st.exists) return StaleVerdict::Unknown;
    const auto cur_mtime = st.mtime;
    const auto cur_size  = st.regular ? st.size : 0;
    // mtime granularity is filesystem-dependent (HFS+ 1 s, ext4 ns, FAT
    // 2 s). Compare strictly: any difference — forward OR backward —
    // counts as stale. Size mismatch is also a stale signal even when
    // mtime didn't change (some tools touch-without-updating-mtime).
    if (snap->mtime == cur_mtime && snap->size == cur_size)
        return StaleVerdict::Fresh;
    return StaleVerdict::Stale;
}

} // namespace mcp::tools::util
