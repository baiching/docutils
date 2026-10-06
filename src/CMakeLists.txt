# The query + edit layer is a static library so the CLI and the test suite link
# the same objects instead of compiling the sources twice.

# Installed libraries goes here

include(FetchContent)

# 1. Declare the tree-sitter dependency
FetchContent_Declare(
    tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter
    GIT_TAG        v0.27.0  # Replace with the specific stable version you need
)

# 2. Force Tree-sitter to build as a static library
# This ensures MSVC generates a .lib file directly without expecting a DLL export map
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Build static libraries")

# 3. Download and make the targets available
FetchContent_MakeAvailable(tree-sitter)

add_library(docutils_lib STATIC
    "latex/ltx_utils.cpp"
    "latex/ltx_query.cpp"
    "latex/ltx_edit.cpp"
    "latex/ltx_document.cpp"
    "latex/ltx_cli.cpp"
)

# Public: consumers (the CLI and the tests) include "latex/..." relative to
# this directory, and inherit the tree-sitter include path through the link.
target_include_directories(docutils_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(docutils_lib PUBLIC tree-sitter tree-sitter-latex)

# Add source to this project's executable.
add_executable (docutils "docutils.cpp" "docutils.h")

target_link_libraries(docutils PRIVATE docutils_lib)
