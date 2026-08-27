// fuzz_apply_patch.cpp — libFuzzer / GCC-driver harness for the apply_patch
// unified-diff hunk parser.
//
// parse_hunks() (fs_edit.cpp) splits a fully model-controlled patch string into
// hunks: it scans for `@@` headers, strtol's the old_start hint out of the
// `@@ -a,b +c,d @@` line, and classifies each body line by its leading marker
// (' '/'+'/'-'/sentinel/bare). Lots of substring / index work on untrusted
// bytes — a crash or OOB here is reachable from one apply_patch call. Each
// parsed hunk's before/after text then feeds the (separately-fuzzed) fuzzy
// locator, so this harness covers the patch-side of that pipeline.
//
// parse_hunks lives in an anonymous namespace in fs_edit.cpp, so we re-declare
// a byte-for-byte copy here (kept in lockstep with the source; the DoS/robust-
// ness properties we care about are structural, not tied to the exact call
// site). The fuzzer feeds it arbitrary bytes and asserts it never crashes.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Hunk {
    std::string before;
    std::string after;
    long        old_start = 0;
    int         index = 0;
};

// Mirror of fs_edit.cpp parse_hunks (see note above).
std::vector<Hunk> parse_hunks(std::string_view patch, std::string& err) {
    std::vector<Hunk> hunks;
    std::vector<std::string> lines;
    { std::size_t s = 0, n;
      while ((n = patch.find('\n', s)) != std::string_view::npos) {
          lines.emplace_back(patch.substr(s, n - s)); s = n + 1; }
      if (s < patch.size()) lines.emplace_back(patch.substr(s)); }

    int hunk_no = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.rfind("@@", 0) != 0) continue;
        Hunk h; h.index = ++hunk_no;
        { auto minus = ln.find('-');
          if (minus != std::string::npos)
              h.old_start = std::strtol(ln.c_str() + minus + 1, nullptr, 10); }
        for (++i; i < lines.size(); ++i) {
            const std::string& b = lines[i];
            if (b.rfind("@@", 0) == 0) { --i; break; }
            if (b.rfind("\\ No newline", 0) == 0) continue;
            if (b.empty()) { h.before += "\n"; h.after += "\n"; continue; }
            char mk = b[0];
            std::string body = b.substr(1);
            if      (mk == ' ') { h.before += body; h.before += "\n";
                                  h.after  += body; h.after  += "\n"; }
            else if (mk == '-') { h.before += body; h.before += "\n"; }
            else if (mk == '+') { h.after  += body; h.after  += "\n"; }
            else { h.before += b; h.before += "\n"; h.after += b; h.after += "\n"; }
        }
        if (h.before.empty() && h.after.empty()) continue;
        hunks.push_back(std::move(h));
    }
    return hunks;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 256u * 1024u) size = 256u * 1024u;
    std::string_view patch{reinterpret_cast<const char*>(data), size};
    std::string err;
    auto hunks = parse_hunks(patch, err);
    // Touch every field so the optimizer can't elide the parse.
    std::size_t acc = hunks.size();
    for (const auto& h : hunks)
        acc += h.before.size() + h.after.size()
             + static_cast<std::size_t>(h.old_start) + static_cast<std::size_t>(h.index);
    (void)acc;
    return 0;
}
