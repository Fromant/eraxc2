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
    src/frontend/parser/ExpressionParser.cpp
    src/frontend/parser/ProgramParser.cpp
    src/middleend/jir/Jir.cpp
    src/middleend/jir/Builder.cpp
    src/middleend/jir/Fold.cpp
)
target_include_directories(erax PUBLIC src)

add_executable(eraxc main.cpp)
target_link_libraries(eraxc PRIVATE erax)

if(ERAX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
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
