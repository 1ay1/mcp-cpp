// agtest — doctest compatibility shim for mcp-cpp's test suite.
// Remaps both assert() and a legacy CHECK(cond)/CHECK(cond,msg) onto doctest,
// wrapping the predicate so `assert(a && b)` / `CHECK(a && b)` don't trip
// doctest's expression decomposer. Include INSTEAD of <doctest/doctest.h>;
// main() comes from test_main.cpp. A test migrates by dropping <cassert> / its
// local CHECK macro, including this, and wrapping main() in a TEST_CASE.
#ifndef MCP_TESTS_AGTEST_HPP
#define MCP_TESTS_AGTEST_HPP

#include <doctest/doctest.h>

#include <string>

#ifdef assert
#  undef assert
#endif
#define assert(cond) DOCTEST_CHECK((cond))

#ifdef CHECK
#  undef CHECK
#endif
#define AGTEST_CHECK_2(cond, msg) DOCTEST_CHECK_MESSAGE((cond), msg)
#define AGTEST_CHECK_1(cond)      DOCTEST_CHECK((cond))
#define AGTEST_CHECK_PICK(_1, _2, NAME, ...) NAME
#define CHECK(...) \
    AGTEST_CHECK_PICK(__VA_ARGS__, AGTEST_CHECK_2, AGTEST_CHECK_1)(__VA_ARGS__)

// Some tests assert via a `void check(bool[, msg])` helper. Provide it so those
// migrate by deleting the local helper + wrapping main() in a TEST_CASE.
namespace mcptest {
inline void check(bool ok) { DOCTEST_CHECK(ok); }
inline void check(bool ok, const char* what) { DOCTEST_CHECK_MESSAGE(ok, what); }
inline void check(bool ok, const std::string& what) { DOCTEST_CHECK_MESSAGE(ok, what); }
} // namespace mcptest
using mcptest::check;

#endif // MCP_TESTS_AGTEST_HPP
