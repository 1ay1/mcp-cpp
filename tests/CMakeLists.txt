add_executable(mcp_codec_test codec_test.cpp)
target_link_libraries(mcp_codec_test PRIVATE mcp::mcp)
add_test(NAME mcp_codec_test COMMAND mcp_codec_test)

add_executable(mcp_loopback_test loopback_test.cpp)
target_link_libraries(mcp_loopback_test PRIVATE mcp::mcp)
add_test(NAME mcp_loopback_test COMMAND mcp_loopback_test)

add_executable(mcp_stateless_test stateless_test.cpp)
target_link_libraries(mcp_stateless_test PRIVATE mcp::mcp)
add_test(NAME mcp_stateless_test COMMAND mcp_stateless_test)

add_executable(mcp_server_stateless_test server_stateless_test.cpp)
target_link_libraries(mcp_server_stateless_test PRIVATE mcp::mcp)
add_test(NAME mcp_server_stateless_test COMMAND mcp_server_stateless_test)

add_executable(mcp_auth_test auth_test.cpp)
target_link_libraries(mcp_auth_test PRIVATE mcp::mcp)
add_test(NAME mcp_auth_test COMMAND mcp_auth_test)

add_executable(mcp_protocol_test protocol_test.cpp)
target_link_libraries(mcp_protocol_test PRIVATE mcp::mcp)
add_test(NAME mcp_protocol_test COMMAND mcp_protocol_test)

add_executable(mcp_cap_test cap_test.cpp)
target_link_libraries(mcp_cap_test PRIVATE mcp::mcp)
add_test(NAME mcp_cap_test COMMAND mcp_cap_test)

add_executable(mcp_scheduler_test scheduler_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(mcp_scheduler_test PRIVATE mcp::mcp Threads::Threads)
add_test(NAME mcp_scheduler_test COMMAND mcp_scheduler_test)

if(MCP_BUILD_TOOLS)
    add_executable(mcp_toolset_test toolset_test.cpp)
    target_link_libraries(mcp_toolset_test PRIVATE mcp::tools)
    add_test(NAME mcp_toolset_test COMMAND mcp_toolset_test)

    add_executable(mcp_fs_tools_test fs_tools_test.cpp)
    target_link_libraries(mcp_fs_tools_test PRIVATE mcp::tools)
    add_test(NAME mcp_fs_tools_test COMMAND mcp_fs_tools_test)

    add_executable(mcp_search_tools_test search_tools_test.cpp)
    target_link_libraries(mcp_search_tools_test PRIVATE mcp::tools)
    add_test(NAME mcp_search_tools_test COMMAND mcp_search_tools_test)

    add_executable(mcp_web_tools_test web_tools_test.cpp)
    target_link_libraries(mcp_web_tools_test PRIVATE mcp::tools)
    add_test(NAME mcp_web_tools_test COMMAND mcp_web_tools_test)

    add_executable(mcp_arg_reader_test arg_reader_test.cpp)
    target_link_libraries(mcp_arg_reader_test PRIVATE mcp::tools)
    add_test(NAME mcp_arg_reader_test COMMAND mcp_arg_reader_test)

    add_executable(mcp_terminal_strip_test terminal_strip_test.cpp)
    target_link_libraries(mcp_terminal_strip_test PRIVATE mcp::tools)
    add_test(NAME mcp_terminal_strip_test COMMAND mcp_terminal_strip_test)

    add_executable(mcp_bash_validate_test bash_validate_test.cpp)
    target_link_libraries(mcp_bash_validate_test PRIVATE mcp::tools)
    add_test(NAME mcp_bash_validate_test COMMAND mcp_bash_validate_test)

    add_executable(mcp_fuzzy_match_test fuzzy_match_test.cpp)
    target_link_libraries(mcp_fuzzy_match_test PRIVATE mcp::tools)
    add_test(NAME mcp_fuzzy_match_test COMMAND mcp_fuzzy_match_test)

    # /dev/tty detach check is POSIX-only (no /dev/tty on Windows).
    if(NOT WIN32)
        add_executable(mcp_subprocess_tty_test subprocess_tty_test.cpp)
        target_link_libraries(mcp_subprocess_tty_test PRIVATE mcp::tools)
        add_test(NAME mcp_subprocess_tty_test COMMAND mcp_subprocess_tty_test)

        # process_start/poll/stop e2e uses sh -c + exit codes (POSIX shell).
        add_executable(mcp_process_tools_test process_tools_test.cpp)
        find_package(Threads REQUIRED)
        target_link_libraries(mcp_process_tools_test PRIVATE mcp::tools Threads::Threads)
        add_test(NAME mcp_process_tools_test COMMAND mcp_process_tools_test)
    endif()
endif()
