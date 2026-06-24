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

# ── The batteries-included toolset: a COMPILED add-on (libmcp_tools) ────────
# The core protocol library above is header-only. The built-in agent toolset
# (filesystem / shell / search / git / web + host-injected memory / todo /
# skill / docs / subagent SHELLS) is substantial implementation code, so it
# lives in a compiled static lib a host links only if it wants the tools.
# Heavy template cost stays out of the header-only core.
option(MCP_BUILD_TOOLS "Build the batteries-included mcp::tools toolset" ON)
if(MCP_BUILD_TOOLS)
    file(GLOB MCP_TOOLS_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/util/*.cpp)
    add_library(mcp_tools STATIC ${MCP_TOOLS_SOURCES})
    add_library(mcp::tools ALIAS mcp_tools)
    target_include_directories(mcp_tools
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tools)
    target_link_libraries(mcp_tools PUBLIC mcp::mcp)
    target_compile_features(mcp_tools PUBLIC cxx_std_23)
endif()

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
