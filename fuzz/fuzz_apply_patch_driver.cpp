// fuzz_apply_patch_driver.cpp — GCC-compatible standalone driver.
//
// Feeds parse_hunks (via LLVMFuzzerTestOneInput) a deterministic stream of
// adversarial unified-diff-ish inputs aimed at the header/marker/index paths:
// truncated `@@` headers, `@@ -` with no number, giant/negative line numbers,
// bare marker-less lines, the "\ No newline" sentinel, embedded NULs, and long
// runs of hunks. libFuzzer is Clang-only; this gives the same crash coverage
// under GCC + ASan/UBSan.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

void feed(const std::string& s) {
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void run_edge_cases() {
    feed("");
    feed("@@");                                   // header, nothing else
    feed("@@ -");                                 // strtol on the string end
    feed("@@ -\n");                               // header, empty body
    feed("@@ -999999999999999999999999,1 +1,1 @@\n context\n"); // strtol overflow
    feed("@@ --1,1 +1,1 @@\n-x\n+y\n");           // negative-ish
    feed("@@ -1,1 +1,1 @@");                       // header, no trailing newline
    feed(std::string("@@ -1 +1 @@\n") + std::string("\0\0\0", 3) + "\n"); // NUL body
    feed("@@ -1 +1 @@\n\\ No newline at end of file\n"); // sentinel only
    feed("@@ -1 +1 @@\nbare line no marker\n");    // marker-less body
    feed("not a patch at all\njust text\n");       // no @@ anywhere
    feed("@@x");                                   // @@ prefix, junk
    // Many empty hunks + many real hunks back to back.
    std::string many;
    for (int i = 0; i < 5000; ++i) many += "@@ -" + std::to_string(i) + " +" + std::to_string(i) + " @@\n";
    feed(many);
    std::string big;
    for (int i = 0; i < 3000; ++i) big += "@@ -1 +1 @@\n-old" + std::to_string(i) + "\n+new" + std::to_string(i) + "\n context\n";
    feed(big);
}

void run_case(std::mt19937_64& rng) {
    // Palette of bytes that stress the parser: header chars, markers, newline,
    // sentinel fragments, digits, NUL, high bytes.
    static const char* frags[] = {
        "@@ -", "@@", " +", ",", " @@", "\n", " ", "+", "-", "\\ No newline",
        "x", "0", "9", "999999999999", "\0", "\xff", "context", "@@x",
    };
    static const size_t NF = sizeof(frags) / sizeof(frags[0]);
    std::uniform_int_distribution<size_t> nfrag(0, 400);
    std::uniform_int_distribution<size_t> pick(0, NF - 1);
    std::string s;
    const size_t n = nfrag(rng);
    for (size_t i = 0; i < n; ++i) {
        const char* f = frags[pick(rng)];
        // frags with an embedded NUL need explicit length.
        if (f[0] == '\0') s.push_back('\0');
        else s += f;
    }
    feed(s);
}

} // namespace

int main(int argc, char** argv) {
    run_edge_cases();
    unsigned long iters = 500000;
    if (argc > 1) iters = std::strtoul(argv[1], nullptr, 10);
    std::mt19937_64 rng(0xA9C1DE);
    for (unsigned long i = 0; i < iters; ++i) run_case(rng);
    std::printf("fuzz_apply_patch: %lu iterations + edge cases, no crash.\n", iters);
    return 0;
}
