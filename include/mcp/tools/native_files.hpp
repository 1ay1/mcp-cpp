// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/native_files.hpp — a FileSystem over the process's own disk.
//
//   Link mcp::tools_native_files and hand one to HostServices::files when the
//   host has no reason to route file access anywhere special:
//
//     svc.files = std::make_shared<mcp::tools::NativeFileSystem>();
//
//   Writes are atomic (temp file in the same directory, fsync, rename, then
//   fsync the directory), keep the target's mode, and follow a symlink to
//   the real file instead of replacing the link.
#pragma once

#include <mcp/tools/files.hpp>

namespace mcp::tools {

class NativeFileSystem final : public FileSystem {
public:
    [[nodiscard]] FileStat stat(const fs::path& p) const override;
    [[nodiscard]] fs::path canonical(const fs::path& p, std::error_code& ec) const override;
    [[nodiscard]] fs::path read_symlink(const fs::path& p, std::error_code& ec) const override;
    [[nodiscard]] fs::path cwd() const override;
    [[nodiscard]] fs::path scratch_dir() const override;
    [[nodiscard]] std::optional<std::string> read(const fs::path& p) const override;
    [[nodiscard]] std::optional<std::string> read_prefix(const fs::path& p,
                                                         std::size_t n) const override;
    [[nodiscard]] std::string write_atomic(const fs::path& p, std::string_view content) override;
    [[nodiscard]] std::vector<DirEntry> list(const fs::path& dir,
                                             std::error_code& ec) const override;
    bool walk(const fs::path& root,
              const std::function<WalkStep(const DirEntry&)>& visit) const override;
    bool create_directories(const fs::path& p, std::error_code& ec) override;
    bool remove(const fs::path& p, std::error_code& ec) override;
    std::uintmax_t remove_all(const fs::path& p, std::error_code& ec) override;
    void rename(const fs::path& from, const fs::path& to, std::error_code& ec) override;
    void copy(const fs::path& from, const fs::path& to, std::error_code& ec) override;
};

}  // namespace mcp::tools
