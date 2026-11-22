cmake_minimum_required(VERSION 3.20)
project(erax LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ERAX_BUILD_TESTS "Build the test suite" ON)
option(ERAX_COVERAGE "Instrument with gcov and add the 'coverage' report target" OFF)

if(ERAX_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "ERAX_COVERAGE uses gcov: configure with GCC or Clang, or measure with llvm-cov by hand")
    endif()
    find_program(ERAX_GCOV NAMES gcov REQUIRED)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
    endif()
    add_compile_options(--coverage)
    add_link_options(--coverage)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

add_library(erax
    src/frontend/tokenizer/Tokenizer.cpp
    src/frontend/parser/SemType.cpp
    src/frontend/parser/Value.cpp
    src/frontend/parser/ConstantRange.cpp
    src/frontend/parser/ParseContext.cpp
    src/frontend/parser/ParseContext.ownership.cpp
    src/frontend/parser/ExpressionParser.cpp
    src/frontend/parser/ExpressionParser.records.cpp
    src/frontend/parser/ProgramParser.cpp
    src/frontend/parser/ProgramParser.records.cpp
    src/frontend/parser/ProgramParser.effects.cpp
    src/middleend/jir/Jir.cpp
    src/middleend/jir/Builder.cpp
    src/middleend/jir/Fold.cpp
    src/middleend/jir/Validator.cpp
    src/primitives/Primitives.cpp
    src/interp/Interpreter.cpp
)
target_include_directories(erax PUBLIC src)

add_executable(eraxc main.cpp)
target_link_libraries(eraxc PRIVATE erax)

# `stdlib/std.erx` embedded in the driver (docs/std.md): compiling never depends
# on files next to the binary. The re-run property makes an edit to the library
# regenerate the header on the next build.
file(READ "${CMAKE_SOURCE_DIR}/stdlib/std.erx" ERAX_STD_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/stdlib/std.erx")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/StdSource.hpp"
    "#pragma once\n"
    "\n"
    "#include <string_view>\n"
    "\n"
    "namespace erax {\n"
    "\n"
    "  /// stdlib/std.erx as it stands in the source tree, embedded at configure\n"
    "  /// time so compiling never depends on files next to the binary\n"
    "  /// (docs/std.md). The driver tokenizes this before the user's file, which\n"
    "  /// is what makes std's declarations part of every program.\n"
    "  inline constexpr std::string_view std_source = R\"ERAX(${ERAX_STD_SOURCE})ERAX\";\n"
    "\n"
    "}  // namespace erax\n")
target_include_directories(eraxc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# `stdlib/std.test.erx`, the std self-test, embedded the same way: the test
# that checks the suite runs the checked-in copy and only that copy.
file(READ "${CMAKE_SOURCE_DIR}/stdlib/std.test.erx" ERAX_STD_TEST_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/stdlib/std.test.erx")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/StdTestSource.hpp"
    "#pragma once\n"
    "\n"
    "#include <string_view>\n"
    "\n"
    "namespace erax {\n"
    "\n"
    "  /// stdlib/std.test.erx as it stands in the source tree, embedded so the\n"
    "  /// interpreter test runs the checked-in copy (docs/std.md).\n"
    "  inline constexpr std::string_view std_test_source = R\"ERAX(${ERAX_STD_TEST_SOURCE})ERAX\";\n"
    "\n"
    "}  // namespace erax\n")

if(ERAX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # The std tests parse the same embedded copy the driver imports.
    target_include_directories(erax_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

if(ERAX_COVERAGE AND ERAX_BUILD_TESTS)
    # `cmake --build build-cov --target coverage` runs the suite and prints the
    # per-source table, so a coverage run is one command and always the same one.
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND}
                -DERAX_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DERAX_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DERAX_GCOV=${ERAX_GCOV}
                -DERAX_TEST_EXE=$<TARGET_FILE:erax_tests>
                -P ${CMAKE_SOURCE_DIR}/cmake/coverage.cmake
        DEPENDS erax_tests
        USES_TERMINAL
        COMMENT "Coverage: running the suite and reporting gcov")
endif()
