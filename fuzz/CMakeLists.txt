# fuzz/CMakeLists.txt — fuzz harnesses for mcp::tools.
#
# Two build modes, auto-selected by compiler:
#
#   • Clang  → libFuzzer executable (coverage-guided) with fuzzer+asan+ubsan.
#             ./fuzz_<name> -max_total_time=60
#   • GCC    → a standalone driver (fuzz_<name>_driver.cpp) that feeds the SAME
#             LLVMFuzzerTestOneInput body a deterministic adversarial stream,
#             built with GCC's asan+ubsan. libFuzzer is Clang-only, but GCC
#             still catches the OOB/UB this target cares about.
#
# The code under test is compiled DIRECTLY into the harness (not linked from
# the pre-built mcp_tools lib) so it is fully sanitizer-instrumented.
#
# Enabled via -DMCP_BUILD_FUZZERS=ON from the top-level build.

set(FUZZ_fuzzy_match_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/util/fuzzy_match.cpp")
set(FUZZ_apply_patch_SRC "")   # parser is self-contained in the harness
set(MCP_FUZZ_TARGETS fuzzy_match apply_patch)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MCP_FUZZ_SANITIZE "fuzzer,address,undefined")
    set(_fuzz_extra_src "")            # libFuzzer supplies main()
else()
    set(MCP_FUZZ_SANITIZE "address,undefined")
    set(_fuzz_driver_suffix "_driver") # our own main() drives the body
endif()

foreach(name ${MCP_FUZZ_TARGETS})
    set(_srcs fuzz_${name}.cpp ${FUZZ_${name}_SRC})
    if(DEFINED _fuzz_driver_suffix)
        list(APPEND _srcs fuzz_${name}${_fuzz_driver_suffix}.cpp)
    endif()

    add_executable(fuzz_${name} ${_srcs})
    target_include_directories(fuzz_${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    target_compile_options(fuzz_${name} PRIVATE
        -g -O1 -fno-omit-frame-pointer
        -fsanitize=${MCP_FUZZ_SANITIZE})
    target_link_options(fuzz_${name} PRIVATE
        -fsanitize=${MCP_FUZZ_SANITIZE})
    set_target_properties(fuzz_${name} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        CXX_SCAN_FOR_MODULES OFF)
endforeach()
