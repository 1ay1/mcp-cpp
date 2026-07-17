add_executable(mcp_server_example server_example.cpp)
target_link_libraries(mcp_server_example PRIVATE mcp::mcp)

# client_example forks the server and drives it over stdio pipes via
# fork/execl + ext/stdio_filebuf.h (libstdc++ extension) — all POSIX-only,
# no MSVC equivalent. Skip it on Windows so the release build's Windows
# leg isn't broken by an example that can never compile there.
if(NOT WIN32)
    add_executable(mcp_client_example client_example.cpp)
    target_link_libraries(mcp_client_example PRIVATE mcp::mcp)
endif()
