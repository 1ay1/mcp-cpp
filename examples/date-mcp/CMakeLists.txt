cmake_minimum_required(VERSION 3.24)
project(date_mcp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Two ways this builds:
#   • In-tree (from mcp-cpp's own build): mcp::mcp already exists — just link.
#   • Standalone (copied out of the repo): fetch mcp-cpp with FetchContent.
if(NOT TARGET mcp::mcp)
    include(FetchContent)
    FetchContent_Declare(mcp
        GIT_REPOSITORY https://github.com/1ay1/mcp-cpp.git
        GIT_TAG        master)
    FetchContent_MakeAvailable(mcp)
endif()

add_executable(date_server date_server.cpp)
target_link_libraries(date_server PRIVATE mcp::mcp)
