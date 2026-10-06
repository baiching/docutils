# Test suite for the LaTeX query + edit layer.
#
# GoogleTest is fetched rather than required, so a fresh clone needs no extra
# setup: configuring the project downloads it once.

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest
    GIT_TAG        v1.18.0
    GIT_SHALLOW    TRUE
)

# Match the project's runtime setting so the test binary links cleanly, and skip
# googletest's install rules.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(docutils_tests
    ltx_edit_test.cpp
    ltx_query_test.cpp
    ltx_document_test.cpp
    ltx_cli_test.cpp
)

# test_utils.hpp lives beside the test sources.
target_include_directories(docutils_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# docutils_lib carries the layer sources, the public include path and the
# tree-sitter link, so the tests exercise exactly what the CLI ships.
target_link_libraries(docutils_tests PRIVATE docutils_lib gtest_main)

# Registers each test with CTest, so both `ctest` and a plain run of the binary
# work.
include(GoogleTest)
gtest_discover_tests(docutils_tests)
