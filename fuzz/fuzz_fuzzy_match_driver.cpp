// fuzz_fuzzy_match_driver.cpp — GCC-compatible standalone fuzz driver.
//
// libFuzzer needs Clang; this driver gives the same crash coverage on a stock
// GCC + ASan/UBSan toolchain by feeding fuzzy_find() a large, deterministic
// stream of ADVERSARIAL (file, needle) pairs aimed at the index-arithmetic and
// UTF-8 boundary paths (traceback bounds, banded-DP edges, smart-quote
// normalization at buffer ends, indent splicing). It calls the SAME
// LLVMFuzzerTestOneInput() entry the libFuzzer harness exports, so the two
// share one body.
//
// Build (GCC): compiled by fuzz/CMakeLists.txt when MCP_BUILD_FUZZERS=ON and
// the compiler is NOT clang. Run: ./fuzz_fuzzy_match_driver [iterations]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// Provided by fuzz_fuzzy_match.cpp.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

// A palette biased toward bytes that stress the locator: newlines (line
// splitting), the multi-byte lead/continuation bytes of the smart-quote
// characters the normalizer rewrites (U+2018/2019/201C/201D/2013/2014/00A0),
// spaces/tabs (indent detection), and ASCII the fuzzy_eq path compares.
const unsigned char kPalette[] = {
    '\n', '\n', ' ', ' ', '\t', 'a', 'b', 'c', '{', '}', '(', ')', ';',
    // UTF-8 lead bytes + continuations for the smart chars + stray/truncated
    // multibyte sequences to hit boundary checks (i+2 < size, etc.).
    0xE2, 0x80, 0x98, 0xE2, 0x80, 0x99, 0xE2, 0x80, 0x9C, 0xE2, 0x80, 0x9D,
    0xE2, 0x80, 0x93, 0xE2, 0x80, 0x94, 0xC2, 0xA0,
    0xE2, 0x80,               // truncated (no continuation) — boundary bait
    0xFF, 0xC0, 0x80,         // ill-formed UTF-8
    'x', 'y', 'z', '0', '9',
};

void run_case(std::mt19937_64& rng) {
    std::uniform_int_distribution<size_t> len_dist(0, 4096);
    const size_t n = len_dist(rng);
    std::vector<uint8_t> buf;
    buf.reserve(n + 2);
    // First two bytes drive the harness's file/needle split.
    buf.push_back(static_cast<uint8_t>(rng()));
    buf.push_back(static_cast<uint8_t>(rng()));
    std::uniform_int_distribution<size_t> pick(0, sizeof(kPalette) - 1);
    for (size_t i = 0; i < n; ++i) buf.push_back(kPalette[pick(rng)]);
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
}

// A few hand-picked shapes that historically break line locators.
void run_edge_cases() {
    auto feed = [](const std::string& s) {
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    feed("");                                   // empty
    feed(std::string(1, '\0'));                 // single split byte, no body
    feed(std::string("\x00\x00", 2));           // split, empty body
    feed(std::string("\xff\xff", 2) + "no newline needle == file");
    feed(std::string("\x00\x10", 2) + std::string(64, '\n')); // all-newlines
    feed(std::string("\x00\x05", 2) + "\xe2\x80");            // truncated smart-quote at end
    feed(std::string("\x00\x02", 2) + "  \tindented\n\tmix"); // indent paths
    feed(std::string("\x7f\xff", 2) + std::string(2000, 'a')); // one huge line
    std::string many_lines;
    for (int i = 0; i < 500; ++i) many_lines += "line" + std::to_string(i) + "\n";
    feed(std::string("\x40\x00", 2) + many_lines);           // deep DP band
}

} // namespace

int main(int argc, char** argv) {
    run_edge_cases();

    unsigned long iters = 200000;
    if (argc > 1) iters = std::strtoul(argv[1], nullptr, 10);

    std::mt19937_64 rng(0xC0FFEEULL);   // fixed seed → reproducible
    for (unsigned long i = 0; i < iters; ++i) run_case(rng);

    std::printf("fuzz_fuzzy_match: %lu iterations + edge cases, no crash.\n", iters);
    return 0;
}
