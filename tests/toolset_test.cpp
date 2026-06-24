// SPDX-License-Identifier: Apache-2.0
//
// toolset_test.cpp — proves the host-injection seam: a FAKE MemoryStore wired
// into mcp::tools::make_provider() drives the remember / forget / wipe shells
// end to end through the CapabilityProvider interface. Validates that:
//   • a tool is registered only when its backend is present;
//   • arg parsing + scope validation + dedup messaging live in the shell;
//   • effects ride back in the Result meta;
//   • the host backend receives exactly the parsed request.

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/cap/local.hpp>

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace mcp::tools;

namespace {

// A trivial in-memory fake the test controls.
class FakeStore : public MemoryStore {
public:
    std::vector<MemoryRecord> records;
    int next_id = 1;

    std::vector<std::string> scopes() const override { return {"project", "user"}; }

    MemoryAppendResult append(const MemoryAppendRequest& r) override {
        // dedup on exact text
        for (auto& rec : records)
            if (rec.text == r.text && rec.scope == r.scope) {
                rec.hits++;
                MemoryAppendResult out; out.id = rec.id; out.deduped = true; return out;
            }
        MemoryRecord rec;
        rec.id = "id" + std::to_string(next_id++);
        rec.text = r.text; rec.scope = r.scope; rec.pinned = r.pinned; rec.tags = r.tags;
        records.push_back(rec);
        MemoryAppendResult out; out.id = rec.id; return out;
    }
    std::size_t forget_by_id(const std::string& id) override {
        auto n = records.size();
        std::erase_if(records, [&](const MemoryRecord& r){ return r.id == id; });
        return n - records.size();
    }
    std::size_t forget_by_substring(const std::string& needle) override {
        auto n = records.size();
        std::erase_if(records, [&](const MemoryRecord& r){ return r.text.find(needle) != std::string::npos; });
        return n - records.size();
    }
    std::vector<MemoryRecord> preview_forget(const std::string& needle) override {
        std::vector<MemoryRecord> out;
        for (auto& r : records)
            if (needle.empty() || r.text.find(needle) != std::string::npos) out.push_back(r);
        return out;
    }
    std::optional<std::size_t> wipe(const std::string& scope) override {
        auto n = records.size();
        std::erase_if(records, [&](const MemoryRecord& r){ return r.scope == scope; });
        return n - records.size();
    }
};

class FakeSkills : public SkillResolver {
public:
    std::optional<std::string> load(const std::string& name, std::string& err) override {
        if (name == "known") return std::string{"# Known skill body"};
        err = "no such skill"; return std::nullopt;
    }
};

class FakeRetriever : public DocRetriever {
public:
    std::vector<DocPassage> retrieve(const DocQuery& q, std::string& mode, std::string& err) override {
        mode = "hybrid";
        if (q.query == "empty") return {};
        DocPassage p; p.source = "docs"; p.path = "a.md"; p.line_start = 1; p.line_end = 4;
        p.score = 0.9; p.text = "matched passage for " + q.query;
        return {p};
    }
};

class FakeTodo : public TodoSink {
public:
    std::vector<TodoItem> last;
    std::string set(std::vector<TodoItem> items) override { last = std::move(items); return {}; }
};

class FakeRunner : public SubagentRunner {
public:
    bool ok = true;
    bool available() const override { return ok; }
    std::string run(const SubagentRequest& r, bool& is_error) override {
        is_error = false; return "report for: " + r.prompt;
    }
};

int fails = 0;
void check(bool c, const char* what) {
    if (!c) { std::fprintf(stderr, "FAIL: %s\n", what); ++fails; }
}

mcp::cap::Result call(mcp::cap::CapabilityProvider& p, const std::string& tool, mcp::Json args) {
    return p.execute(mcp::cap::Request{tool, std::move(args)});
}

} // namespace

int main() {
    // ── backend absent → no memory tools registered ──────────────────────
    {
        HostServices empty;
        auto p = make_provider(empty);
        bool found = false;
        for (auto& t : p->list()) if (t.name == "remember") found = true;
        check(!found, "no remember tool when MemoryStore absent");
    }

    // ── backend present → tools registered + driven end to end ───────────
    auto store = std::make_shared<FakeStore>();
    HostServices svc; svc.memory = store;
    auto p = make_provider(svc);

    // remember + forget + wipe_memory all advertised
    std::map<std::string, mcp::Tool> by_name;
    for (auto& t : p->list()) by_name[t.name] = t;
    check(by_name.count("remember") == 1, "remember registered");
    check(by_name.count("forget") == 1, "forget registered");
    check(by_name.count("wipe_memory") == 1, "wipe_memory registered");

    // remember a fact
    auto r1 = call(*p, "remember", {{"text", "prefer fish shell"}, {"scope", "user"}});
    check(!r1.is_error, "remember ok");
    check(store->records.size() == 1, "one record stored");
    check(store->records[0].scope == "user", "scope passed through");
    check(read_effects(r1).bits() == 0, "remember effects empty in meta");

    // dedup path
    auto r2 = call(*p, "remember", {{"text", "prefer fish shell"}, {"scope", "user"}});
    check(!r2.is_error && store->records.size() == 1, "dedup: no new record");
    check(store->records[0].hits == 1, "dedup bumped hits");

    // unknown scope rejected by the shell (never reaches the store)
    auto r3 = call(*p, "remember", {{"text", "x"}, {"scope", "bogus"}});
    check(r3.is_error, "unknown scope rejected");

    // missing text rejected by the shell
    auto r4 = call(*p, "remember", {{"scope", "user"}});
    check(r4.is_error, "missing text rejected");

    // tags as comma-string normalised by the shell
    call(*p, "remember", {{"text", "tagged"}, {"tags", "a,b , c"}});
    auto& tagged = store->records.back();
    check(tagged.tags.size() == 3, "comma-string tags split to 3");

    // forget dry-run previews, doesn't mutate
    auto before = store->records.size();
    auto rd = call(*p, "forget", {{"substring", "fish"}, {"dry_run", true}});
    check(!rd.is_error && store->records.size() == before, "dry_run preview no mutation");

    // forget by substring mutates
    auto rf = call(*p, "forget", {{"substring", "fish"}});
    check(!rf.is_error && store->records.size() == before - 1, "forget removed one");

    // wipe requires confirm
    call(*p, "remember", {{"text", "user-scope fact"}, {"scope", "user"}});
    auto rw0 = call(*p, "wipe_memory", {{"scope", "user"}});
    check(!rw0.is_error, "wipe preview ok");
    std::size_t user_count = 0;
    for (auto& r : store->records) if (r.scope == "user") ++user_count;
    check(user_count >= 1, "a user-scope record exists before wipe");
    auto total_before = store->records.size();
    auto rw1 = call(*p, "wipe_memory", {{"scope", "user"}, {"confirm", true}});
    check(!rw1.is_error && store->records.size() == total_before - user_count,
          "wipe with confirm removed exactly the user scope");

    if (fails == 0) {
        // ── todo / skill / search_docs / task shells ──────────────────────
        auto todo = std::make_shared<FakeTodo>();
        auto skills = std::make_shared<FakeSkills>();
        auto ret = std::make_shared<FakeRetriever>();
        auto runner = std::make_shared<FakeRunner>();
        HostServices svc2; svc2.todo = todo; svc2.skills = skills;
        svc2.retriever = ret; svc2.subagent = runner;
        auto p2 = make_provider(svc2);

        std::map<std::string, mcp::Tool> n2;
        for (auto& t : p2->list()) n2[t.name] = t;
        check(n2.count("todo") && n2.count("skill") && n2.count("search_docs") && n2.count("task"),
              "todo/skill/search_docs/task all registered");
        check(n2.count("remember") == 0, "no memory tools when store absent");

        auto rt = call(*p2, "todo", {{"todos", mcp::Json::array({
            mcp::Json{{"content","do x"},{"status","in_progress"}}})}});
        check(!rt.is_error && todo->last.size() == 1 && todo->last[0].status == "in_progress",
              "todo sink received structured items");

        auto rs = call(*p2, "skill", {{"name","known"}});
        check(!rs.is_error && rs.text.find("Known skill body") != std::string::npos, "skill body returned");
        auto rs2 = call(*p2, "skill", {{"name","missing"}});
        check(rs2.is_error, "unknown skill errors");

        auto rsd = call(*p2, "search_docs", {{"query","hello"}, {"k", 3}});
        check(!rsd.is_error && rsd.text.find("matched passage") != std::string::npos, "docs retrieved");
        check(read_effects(rsd).has(Effect::Net), "search_docs carries Net effect");

        auto rtask = call(*p2, "task", {{"prompt","investigate"}});
        check(!rtask.is_error && rtask.text.find("report for: investigate") != std::string::npos, "task ran");
        runner->ok = false;
        auto rtask2 = call(*p2, "task", {{"prompt","x"}});
        check(rtask2.is_error, "task refuses when unavailable");
    }

    if (fails == 0) std::printf("mcp_toolset_test: all checks passed\n");
    return fails == 0 ? 0 : 1;
}
