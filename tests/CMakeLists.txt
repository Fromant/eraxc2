include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(erax_tests
    Tokenizer.test.cpp
    Jir.test.cpp
    Validator.test.cpp
    Parser.test.cpp
    Parser.literals.test.cpp
    Parser.globals.test.cpp
    Parser.namespaces.test.cpp
    Parser.errors.test.cpp
    Interpreter.test.cpp
    Primitives.test.cpp
    Std.test.cpp
    Ownership.test.cpp
    Records.test.cpp
    Records.extra.test.cpp
    Collections.test.cpp
    References.test.cpp
)
target_link_libraries(erax_tests PRIVATE erax Catch2::Catch2WithMain)
# The interpreter tests run the shipped examples, so they need to find them.
target_compile_definitions(erax_tests PRIVATE ERAX_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")

add_test(NAME erax_tests COMMAND erax_tests)
add_test(NAME erax_modules
    COMMAND "${CMAKE_COMMAND}" "-DERAXC=$<TARGET_FILE:eraxc>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/modules"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Modules.test.cmake")
