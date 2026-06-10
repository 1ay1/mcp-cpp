cmake_minimum_required(VERSION 3.20)
project(mcp_cpp LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MCP_BUILD_TESTS    "Build mcp-cpp tests"    ON)
option(MCP_BUILD_EXAMPLES "Build mcp-cpp examples" ON)

include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Mark third-party headers SYSTEM so their warnings stay out of our build.
if(TARGET nlohmann_json)
    get_target_property(_nj_iface nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    if(_nj_iface)
        set_target_properties(nlohmann_json PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nj_iface}")
    endif()
endif()

# ── The library: header-only INTERFACE target ──────────────────────────────
add_library(mcp INTERFACE)
add_library(mcp::mcp ALIAS mcp)
target_include_directories(mcp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(mcp INTERFACE nlohmann_json::nlohmann_json)
target_compile_features(mcp INTERFACE cxx_std_23)

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
