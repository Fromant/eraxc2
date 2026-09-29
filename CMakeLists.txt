cmake_minimum_required(VERSION 3.20)
project(erax LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ERAX_BUILD_TESTS "Build the test suite" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

add_library(erax
    src/frontend/tokenizer/Tokenizer.cpp
    src/frontend/parser/SemType.cpp
    src/frontend/parser/Value.cpp
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