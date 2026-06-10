add_executable(mcp_server_example server_example.cpp)
target_link_libraries(mcp_server_example PRIVATE mcp::mcp)

add_executable(mcp_client_example client_example.cpp)
target_link_libraries(mcp_client_example PRIVATE mcp::mcp)
