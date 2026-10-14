// SPDX-License-Identifier: Apache-2.0
//
// effects.cpp — host services that exist only to be registered against.
//
// effects_for_builtin() reads each tool's declared EffectSet off the same
// registrations make_provider() uses. Host-coupled families register only
// when their backend is non-null, so the table is built against these. None
// is ever called: building the table registers tools, it runs nothing. Each
// throws if that ever changes, rather than quietly answering.

#include "tool_shell.hpp"

#include <mcp/tools/files.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/web_search.hpp>

#include <stdexcept>

namespace mcp::tools::detail {

namespace {

[[noreturn]] void never(const char* what) {
    throw std::logic_error(std::string{"mcp-cpp: declare-only "} + what + " was called");
}

struct NoMemory final : MemoryStore {
    std::vector<std::string> scopes() const override { return {"project"}; }
    MemoryAppendResult append(const MemoryAppendRequest&) override { never("MemoryStore"); }
    std::size_t forget_by_id(const std::string&) override { never("MemoryStore"); }
    std::size_t forget_by_substring(const std::string&) override { never("MemoryStore"); }
    std::vector<MemoryRecord> preview_forget(const std::string&) override { never("MemoryStore"); }
    std::optional<std::size_t> preview_wipe(const std::string&) override { never("MemoryStore"); }
    std::optional<std::size_t> wipe(const std::string&) override { never("MemoryStore"); }
};
struct NoTodo final : TodoSink {
    std::string set(std::vector<TodoItem>) override { never("TodoSink"); }
};
struct NoSkills final : SkillResolver {
    std::optional<std::string> load(const Call&, const std::string&, std::string&) override {
        never("SkillResolver");
    }
};
struct NoDocs final : DocRetriever {
    std::vector<DocPassage> retrieve(const DocQuery&, std::string&, std::string&) override {
        never("DocRetriever");
    }
};
struct NoSubagent final : SubagentRunner {
    std::string unavailable_reason(const Call&) const override { never("SubagentRunner"); }
    std::string run(const Call&, const SubagentRequest&, bool&) override { never("SubagentRunner"); }
};
struct NoHttp final : HttpClient {
    HttpResponse send(const HttpRequest&) override { never("HttpClient"); }
};
struct NoExec final : Exec {
    ExecResult run(const Call&, const ExecRequest&) override { never("Exec"); }
    std::expected<std::shared_ptr<Session>, std::string> start(const Call&, const ExecRequest&) override {
        never("Exec");
    }
    bool stops_whole_tree() const noexcept override { return false; }
};
struct NoFiles final : FileSystem {
    FileStat stat(const fs::path&) const override { never("FileSystem"); }
    fs::path canonical(const fs::path&, std::error_code&) const override { never("FileSystem"); }
    fs::path read_symlink(const fs::path&, std::error_code&) const override { never("FileSystem"); }
    fs::path cwd() const override { never("FileSystem"); }
    fs::path scratch_dir() const override { never("FileSystem"); }
    std::optional<std::string> read(const fs::path&) const override { never("FileSystem"); }
    std::optional<std::string> read_prefix(const fs::path&, std::size_t) const override {
        never("FileSystem");
    }
    std::string write_atomic(const fs::path&, std::string_view) override { never("FileSystem"); }
    std::vector<DirEntry> list(const fs::path&, std::error_code&) const override {
        never("FileSystem");
    }
    bool walk(const fs::path&, const std::function<WalkStep(const DirEntry&)>&) const override {
        never("FileSystem");
    }
    bool create_directories(const fs::path&, std::error_code&) override { never("FileSystem"); }
    bool remove(const fs::path&, std::error_code&) override { never("FileSystem"); }
    std::uintmax_t remove_all(const fs::path&, std::error_code&) override { never("FileSystem"); }
    void rename(const fs::path&, const fs::path&, std::error_code&) override { never("FileSystem"); }
    void copy(const fs::path&, const fs::path&, std::error_code&) override { never("FileSystem"); }
};

}  // namespace

HostServices declare_only_services() {
    HostServices s;
    s.memory         = std::make_shared<NoMemory>();
    s.todo           = std::make_shared<NoTodo>();
    s.skills         = std::make_shared<NoSkills>();
    s.retriever      = std::make_shared<NoDocs>();
    s.code_retriever = std::make_shared<NoDocs>();
    s.subagent       = std::make_shared<NoSubagent>();
    s.http           = std::make_shared<NoHttp>();
    s.exec           = std::make_shared<NoExec>();
    s.files          = std::make_shared<NoFiles>();
    return s;
}

}  // namespace mcp::tools::detail
