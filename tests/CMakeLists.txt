# Single mcp-cpp test binary. Every migrated test is a doctest TEST_CASE
# auto-registered into mcp_tests, which links mcp::mcp + mcp::tools once
# instead of ~18 separate executables. doctest_discover_tests keeps per-case
# ctest entries (ctest -j parallelism + per-case reporting preserved).
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
set(DOCTEST_WITH_TESTS OFF CACHE INTERNAL "")
set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

add_executable(mcp_tests
    test_main.cpp
    codec_test.cpp
    loopback_test.cpp
    stateless_test.cpp
    server_stateless_test.cpp
    auth_test.cpp
    protocol_test.cpp
    cap_test.cpp
    scheduler_test.cpp
    toolset_test.cpp
    fs_tools_test.cpp
    apply_patch_test.cpp
    search_tools_test.cpp
    textproc_test.cpp
    data_test.cpp
    structural_test.cpp
    repomap_test.cpp
    web_tools_test.cpp
    arg_reader_test.cpp
    terminal_strip_test.cpp
    bash_validate_test.cpp
    fuzzy_match_test.cpp
)
target_link_libraries(mcp_tests PRIVATE mcp::mcp mcp::tools doctest::doctest Threads::Threads)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(mcp_tests)

# Kept standalone: subprocess_tty forks + drives a real PTY; process_tools
# spawns `sh -c` children. Their process isolation is load-bearing, so they
# must not share a binary with the in-process unit tests.
if(NOT WIN32)
    add_executable(mcp_subprocess_tty_test subprocess_tty_test.cpp)
    target_link_libraries(mcp_subprocess_tty_test PRIVATE mcp::tools)
    add_test(NAME mcp_subprocess_tty_test COMMAND mcp_subprocess_tty_test)

    add_executable(mcp_process_tools_test process_tools_test.cpp)
    target_link_libraries(mcp_process_tools_test PRIVATE mcp::tools Threads::Threads)
    add_test(NAME mcp_process_tools_test COMMAND mcp_process_tools_test)
endif()
