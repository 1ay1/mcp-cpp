// The single mcp-cpp test binary. Every migrated test is a doctest TEST_CASE
// auto-registered here, linking mcp::mcp + mcp::tools once instead of ~18
// separate executables.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
