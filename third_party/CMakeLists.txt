# third_party/CMakeLists.txt

add_library(tree-sitter-latex STATIC
    tree_sitter_latex/src/parser.c
    tree_sitter_latex/src/scanner.c
)

target_include_directories(tree-sitter-latex PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tree_sitter_latex/src
)
