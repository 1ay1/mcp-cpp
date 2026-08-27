// fuzz_fuzzy_match.cpp — libFuzzer harness for the edit/apply_patch line locator.
//
// mcp::tools::util::fuzzy_find() is what the `edit` and `apply_patch` tools use
// to locate `old_text` inside a file when it doesn't byte-match exactly. It
// runs on TWO fully model-controlled blobs (the file contents and the needle),
// does edit-distance line alignment, banded dynamic programming, UTF-8
// smart-quote normalization, and byte-offset splicing / indent adjustment — a
// dense mesh of index arithmetic on untrusted input. A crash or OOB read here
// is reachable from a single tool call, so it's worth fuzzing under ASan/UBSan.
//
// The fuzzer input is split into (file, needle) by a length prefix so both
// halves vary independently; every fuzzy_find overload (incl. new_text indent
// fix-up and a line hint) is exercised.
//
// Build (Clang):
//   CC=clang CXX=clang++ cmake -B fuzzbuild -DMCP_BUILD_FUZZERS=ON
//   cmake --build fuzzbuild -j
//   ./fuzzbuild/fuzz/fuzz_fuzzy_match -max_total_time=60

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <mcp/tools/util/fuzzy_match.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap total input: beyond this the O(Q*B) DP + banding is exercising the
    // allocator/cap logic, not new locator paths. fuzzy_find has its own
    // MAX_DP_CELLS guard, but keep the fuzzer's own memory bounded too.
    if (size > 128u * 1024u) size = 128u * 1024u;
    if (size < 2) return 0;

    // First 2 bytes: split point for file vs needle, so both halves flex
    // independently across mutations (a fixed midpoint would couple them).
    const size_t total = size - 2;
    const size_t split = total == 0 ? 0
                       : (static_cast<size_t>(data[0]) << 8 | data[1]) % (total + 1);

    const char* body = reinterpret_cast<const char*>(data) + 2;
    std::string_view file{body, split};
    std::string_view needle{body + split, total - split};

    // Exercise every public overload; each internally hits scan_lines,
    // candidate_bands, run_banded_dp, the exact-match fast path, traceback,
    // and (for the new_text form) detect_indent_delta + apply_indent_delta.
    auto m1 = mcp::tools::util::fuzzy_find(file, needle);
    (void)m1;

    // Feed a slice of the file back as new_text so indent adjustment runs on
    // real (structured) bytes rather than empty input.
    std::string_view new_text = file.substr(0, file.size() / 2);
    auto m2 = mcp::tools::util::fuzzy_find(file, needle, new_text);
    (void)m2;

    // A line hint derived from the input drives the tie-break / tolerance path.
    const std::uint32_t hint =
        needle.empty() ? std::numeric_limits<std::uint32_t>::max()
                       : static_cast<std::uint32_t>(
                             static_cast<unsigned char>(needle.front()) * 4u);
    auto m3 = mcp::tools::util::fuzzy_find(file, needle, new_text, hint);
    (void)m3;

    return 0;
}
