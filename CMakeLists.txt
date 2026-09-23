cmake_minimum_required(VERSION 3.20...3.31)
project(mcp_cpp
    VERSION     0.1.0
    DESCRIPTION "Model Context Protocol — modern C++ implementation + built-in tool set"
    LANGUAGES   CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build tests/examples by default ONLY when mcp-cpp is the top-level project
# (built standalone). When embedded via add_subdirectory (e.g. by agentty),
# they default OFF so the parent doesn't pull mcp-cpp's test binaries into its
# own build/ctest — each repo runs its own tests. (PROJECT_IS_TOP_LEVEL is
# CMake >=3.21; derive it for the 3.20 floor.)
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    else()
        set(PROJECT_IS_TOP_LEVEL OFF)
    endif()
endif()
option(MCP_BUILD_TESTS    "Build mcp-cpp tests"    ${PROJECT_IS_TOP_LEVEL})
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
    # tree-sitter runtime + bash grammar (vendored, MIT; see third_party/README)
    # for shellx's shell-command parsing. Plain C. Always optimised: the
    # generated parser.c is a 9.6 MB table that nobody steps through, and
    # -O0 makes every agent shell call parse ~10x slower in Debug builds.
    enable_language(C)
    add_library(mcp_tree_sitter STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/src/lib.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-bash/src/parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-bash/src/scanner.c)
    target_include_directories(mcp_tree_sitter
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/src
                ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-bash/src)
    set_target_properties(mcp_tree_sitter PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    target_compile_options(mcp_tree_sitter PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-O2 -w>
        $<$<C_COMPILER_ID:MSVC>:/O2 /w>)
    # _POSIX_C_SOURCE / _DEFAULT_SOURCE: the runtime uses fdopen/strdup.
    target_compile_definitions(mcp_tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)

    file(GLOB MCP_TOOLS_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/util/*.cpp)
    add_library(mcp_tools STATIC ${MCP_TOOLS_SOURCES})
    add_library(mcp::tools ALIAS mcp_tools)
    target_include_directories(mcp_tools
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tools)
    # Static archive: consumers must link tree-sitter too, so propagate it
    # (link-only; its headers stay private to shellx.cpp).
    target_link_libraries(mcp_tools PUBLIC mcp::mcp $<LINK_ONLY:mcp_tree_sitter>)
    target_include_directories(mcp_tools PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/include)
    target_compile_features(mcp_tools PUBLIC cxx_std_23)
    # MSVC defaults the execution charset to the system code page (CP1252 on
    # most Windows installs), NOT UTF-8. Narrow string literals containing
    # non-ASCII characters — em dashes, curly quotes, box-drawing — then get
    # encoded as single CP1252 bytes (e.g. U+2014 "—" → 0x97), which is invalid
    # UTF-8. Those literals live in tool descriptions that a host serializes
    # onto the JSON wire, where the encoder rejects them (json type_error.316).
    # /utf-8 forces both the source and execution charsets to UTF-8.
    target_compile_options(mcp_tools PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endif()

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# libFuzzer harnesses (Clang only). Off by default; opt in with
#   CC=clang CXX=clang++ cmake -B fuzzbuild -DMCP_BUILD_FUZZERS=ON
option(MCP_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only)" OFF)
if(MCP_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
