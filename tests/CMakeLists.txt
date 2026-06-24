add_executable(mcp_codec_test codec_test.cpp)
target_link_libraries(mcp_codec_test PRIVATE mcp::mcp)
add_test(NAME mcp_codec_test COMMAND mcp_codec_test)

add_executable(mcp_loopback_test loopback_test.cpp)
target_link_libraries(mcp_loopback_test PRIVATE mcp::mcp)
add_test(NAME mcp_loopback_test COMMAND mcp_loopback_test)

add_executable(mcp_protocol_test protocol_test.cpp)
target_link_libraries(mcp_protocol_test PRIVATE mcp::mcp)
add_test(NAME mcp_protocol_test COMMAND mcp_protocol_test)

add_executable(mcp_cap_test cap_test.cpp)
target_link_libraries(mcp_cap_test PRIVATE mcp::mcp)
add_test(NAME mcp_cap_test COMMAND mcp_cap_test)

if(MCP_BUILD_TOOLS)
    add_executable(mcp_toolset_test toolset_test.cpp)
    target_link_libraries(mcp_toolset_test PRIVATE mcp::tools)
    add_test(NAME mcp_toolset_test COMMAND mcp_toolset_test)
endif()
