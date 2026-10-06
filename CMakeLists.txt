# CMakeList.txt : CMake project for docutils, include source and define
# project specific logic here.
#
# 3.16 is the practical floor: the project already relies on
# FetchContent_MakeAvailable (3.14+), and the test suite configures cleanly from
# 3.16 up.
cmake_minimum_required (VERSION 3.16)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("docutils")

# Set once, here, so the layer, the CLI and GoogleTest all agree.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party)
add_subdirectory(src)

# Tests build by default so `ctest` works out of the box. Configure with
# -DDOCUTILS_BUILD_TESTS=OFF to skip fetching GoogleTest.
option(DOCUTILS_BUILD_TESTS "Build the docutils test suite" ON)

if (DOCUTILS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
