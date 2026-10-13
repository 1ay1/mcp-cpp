// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/files.hpp — the host's filesystem, as the builtin tools see it.
//
//   mcp-cpp's tools read, write, list and stat files, but they don't do it
//   themselves: every touch of the disk goes through a FileSystem the host
//   hands them (HostServices::files, reachable in a tool body as call.fs()).
//   The library proper does no IO at all. That lets a host decide how files
//   are reached — on its own IO threads, sandboxed, virtual, recorded for a
//   replay, or faked in a test — and keeps the "passive library" rule
//   checkable by a lint instead of by trust.
//
//   mcp/tools/native_files.hpp is a ready-made std::filesystem one, in its
//   own target (mcp::tools_native_files), for hosts that want the obvious.
//
//   Errors: nothing here throws. A failed query returns an empty optional
//   or sets the error code, exactly as the std::filesystem error_code
//   overloads do, so tool bodies keep their existing degrade-don't-fail
//   handling.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mcp::tools {

namespace fs = std::filesystem;

/// What a stat answers. `exists` false means every other field is unset.
struct FileStat {
    bool                   exists     = false;
    bool                   regular    = false;
    bool                   directory  = false;
    bool                   symlink    = false;   // the path itself is a link
    std::uintmax_t         size       = 0;       // regular files only
    fs::file_time_type     mtime{};
};

/// One entry seen by a directory walk.
struct DirEntry {
    fs::path       path;
    bool           regular   = false;
    bool           directory = false;
    bool           symlink   = false;
    std::uintmax_t size      = 0;
    fs::file_time_type mtime{};
    int            depth     = 0;   // 0 = a direct child of the walk root
};

/// What a walk callback tells the walker to do next.
enum class WalkStep : std::uint8_t {
    next,       // keep going (and descend into this entry if it's a directory)
    skip_dir,   // don't descend into this directory
    stop,       // end the walk now
};

// Called from tool bodies and from the host's Splitter shares, so an
// implementation must be safe to use from several threads at once.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // ── queries ────────────────────────────────────────────────────────
    /// stat; follows symlinks for the type, but reports `symlink` for the
    /// path itself. A missing path gives exists=false, not an error.
    [[nodiscard]] virtual FileStat stat(const fs::path& p) const = 0;

    /// weakly_canonical: resolves what exists, keeps the rest lexical.
    [[nodiscard]] virtual fs::path canonical(const fs::path& p, std::error_code& ec) const = 0;

    /// The target of a symlink.
    [[nodiscard]] virtual fs::path read_symlink(const fs::path& p, std::error_code& ec) const = 0;

    /// The process's working directory, for resolving relative paths.
    [[nodiscard]] virtual fs::path cwd() const = 0;

    /// A directory the host lets tools put scratch files in.
    [[nodiscard]] virtual fs::path scratch_dir() const = 0;

    // ── contents ───────────────────────────────────────────────────────
    /// The whole file, or nullopt when it can't be opened/read.
    [[nodiscard]] virtual std::optional<std::string> read(const fs::path& p) const = 0;

    /// The first `n` bytes (fewer at EOF), or nullopt when it can't be opened.
    [[nodiscard]] virtual std::optional<std::string> read_prefix(const fs::path& p,
                                                                 std::size_t n) const = 0;

    /// Replace the file with `content`, atomically where the platform
    /// allows (temp file + rename), creating parent directories. Returns ""
    /// on success, else a human-readable error.
    [[nodiscard]] virtual std::string write_atomic(const fs::path& p,
                                                   std::string_view content) = 0;

    // ── listing ────────────────────────────────────────────────────────
    /// The direct children of `dir`. Unreadable entries are skipped.
    [[nodiscard]] virtual std::vector<DirEntry> list(const fs::path& dir,
                                                     std::error_code& ec) const = 0;

    /// Walk `root` depth-first, calling `visit` for every entry. Never
    /// follows a symlinked directory (a cyclic link would never end).
    /// Permission-denied subtrees are skipped. Returns false if `root`
    /// couldn't be opened at all.
    virtual bool walk(const fs::path& root,
                      const std::function<WalkStep(const DirEntry&)>& visit) const = 0;

    // ── changes ────────────────────────────────────────────────────────
    virtual bool create_directories(const fs::path& p, std::error_code& ec) = 0;
    /// Remove a file or empty directory. False (no error) if it was absent.
    virtual bool remove(const fs::path& p, std::error_code& ec) = 0;
    /// Remove a tree. Returns how many entries went.
    virtual std::uintmax_t remove_all(const fs::path& p, std::error_code& ec) = 0;
    virtual void rename(const fs::path& from, const fs::path& to, std::error_code& ec) = 0;
    /// Copy a file or a directory tree. Symlinks are copied as links.
    virtual void copy(const fs::path& from, const fs::path& to, std::error_code& ec) = 0;
};

/// std::filesystem::relative, through `files`: canonicalise both through the
/// host, then compare lexically. Empty (and `ec` set) when either can't be
/// resolved.
[[nodiscard]] inline fs::path relative_to(const FileSystem& files, const fs::path& p,
                                          const fs::path& base, std::error_code& ec) {
    const auto cp = files.canonical(p, ec);
    if (ec) return {};
    const auto cb = files.canonical(base, ec);
    if (ec) return {};
    return cp.lexically_relative(cb);
}

}  // namespace mcp::tools
