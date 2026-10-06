

# Add source to this project's executable.
add_executable (docutils "docutils.cpp" "docutils.h" "latex/ltx_utils.cpp" "latex/ltx_query.cpp" "latex/ltx_edit.cpp" "latex/ltx_document.cpp" "latex/ltx_cli.cpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET docutils PROPERTY CXX_STANDARD 20)
endif()

# Installed libraries goes here

include(FetchContent)

# 2. Declare the tree-sitter dependency
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

target_link_libraries(docutils PRIVATE tree-sitter tree-sitter-latex)