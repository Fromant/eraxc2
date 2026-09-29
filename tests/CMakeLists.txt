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
    Parser.test.cpp
    Parser.literals.test.cpp
    Parser.globals.test.cpp
    Parser.errors.test.cpp
)
target_link_libraries(erax_tests PRIVATE erax Catch2::Catch2WithMain)

add_test(NAME erax_tests COMMAND erax_tests)