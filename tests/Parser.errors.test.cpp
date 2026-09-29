#include <string>
#include <string_view>
#include <vector>

#include "ParserTest.hpp"

using namespace erax::frontend;
using namespace erax::middleend;
using namespace erax::test;

/// The remaining shape rules: what a program may not say, and where the error
/// points. Every case here is a rejection, so the message is the assertion.

TEST_CASE("a name is a local, a global, or a function") {
    CHECK(parse_error("fn main(): i32 { return nope; }") == "undeclared identifier 'nope'");
    parse_ok("fn main(): i32 { let x: i32 = 1; return x; }");
    // A function name in a call position is a call; a call result is not a name.
    CHECK(parse_error("fn f(): i32 { return 0; } fn main(): i32 { return f()(); }") != "");
    // A dot is a namespace, and v1 has none: a name is one identifier, so the error
    // is about the dot, wherever it appears after a name.
    CHECK(parse_error("fn main(): i32 { return a.b(); }") == "'.' is not part of a name in v1");
    CHECK(parse_error("fn main(): i32 { let a: i32 = 1; return a.b; }") == "'.' is not part of a name in v1");
    CHECK(parse_error("fn a.b(): i32 { return 0; } fn main(): i32 { return 0; }") == "expected '('");
    // A global's value is a read in a function and a constant in an initializer.
    const Program program = parse_ok("let g: i32 = 1; let h: i32 = g + 1; fn main(): i32 { return h; }");
    CHECK(program.globals[1].init.blocks[0].commands[0].args[1].value == 2);
    // A function is not a global, and a global is not a function.
    CHECK(parse_error("fn f(): i32 { return 0; } let f: i32 = 1; fn main(): i32 { return 0; }") != "");
    CHECK(parse_error("let g: i32 = 1; fn g(): i32 { return 0; } fn main(): i32 { return 0; }") != "");
}

TEST_CASE("a statement is a declaration, an assignment, or a control keyword") {
    // A bare name is not a statement.
    CHECK(parse_error("fn main(): i32 { x; return 0; }") != "");
    // A block is a statement and a scope, and it needs its closing brace.
    CHECK(parse_error("fn main(): i32 { { let x: i32 = 1; return 0; }") == "unterminated function body");
    parse_ok("fn main(): i32 { { { } } return 0; }");
    // A value is not a statement, but a bare expression statement is allowed where
    // it computes nothing: a literal it can fold away emits no command at all.
    const Program folded = parse_ok("fn main(): i32 { 1 + 1; return 0; }");
    CHECK(commands_of(folded, 0).size() == 1);
    CHECK(parse_error("fn main(): i32 { f(); return 0; }") == "call to undeclared function 'f'");
    // A call is a statement when the callee is a void function.
    parse_ok("fn f() { } fn main(): i32 { f(); return 0; }");
    // An expression statement on its own is allowed and emits nothing.
    const Program program = parse_ok("fn main(): i32 { let x: i32 = 1; x; return 0; }");
    CHECK(commands_of(program, 0).size() == 2);
}

TEST_CASE("a local is declared once per scope") {
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let x: i32 = 2; return 0; }") ==
          "'x' is already declared in this scope");
    // A parameter is a name in the function's own scope, so a local may shadow it.
    parse_ok("fn f(x: i32): i32 { let x: i32 = 1; return x; } fn main(): i32 { return f(2); }");
    // A loop's init is its own scope, so it may reuse a name.
    parse_ok("fn main(): i32 { let i: i32 = 0; for (let i: i32 = 0; i < 2; i = i + 1) { } return i; }");
    // ... as may a loop body.
    parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 2) { let i: i32 = 1; i = i + 1; } return i; }");
    // An inner scope sees an outer name, and shadows it.
    const Program program = parse_ok("let g: i32 = 1; fn main(): i32 { let g: i32 = 2; { let g: i32 = 3; }"
                                     " return g; }");
    CHECK(commands_of(program, 0)[0].args[1].value == 2);
}

TEST_CASE("the cursor names what it wanted to see") {
    // Each expectation says what the token is, by its own name.
    CHECK(parse_error("fn main(): i32 { let x = 1; return 0; }") == "expected ':'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1 x = 2; return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { return 0; } ]") == "expected a global or a function declaration");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return 0; } )") == "expected a global or a function declaration");
    // A keyword expectation names the keyword.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; do { i = i - 1; } x; return 0; }") != "");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (let i: i32 = 0; i < 1; i = i + 1) break; return 0; }") !=
          "");
    // An operator expectation quotes the operator.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x ? 1; }") == "unknown operator '?'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x ?? 1; }") == "unknown operator '?" "?'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x <<= 1; }") != "");
    // A maximal-munch run is one operator, so `!=` is one and `!==` is not.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let b: bool = x !== 1; return 0; }") ==
          "unknown operator '!=='");
    parse_ok("fn main(): i32 { let x: i32 = 1; let b: bool = x != 1; return 0; }");
}

TEST_CASE("a string and a float are not types yet") {
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5; return 0; }") == "float is not supported yet");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5 + 1; return 0; }") == "float is not supported yet");
    CHECK(parse_error("fn main(): i32 { let x: string = \"hi\"; return 0; }") == "string is not supported yet");
    CHECK(parse_error("fn main(): i32 { let x: i32 = \"hi\"; return 0; }") == "string is not supported yet");
    CHECK(parse_error("let g: i32 = 1.5; fn main(): i32 { return 0; }") == "float is not supported yet");
    // A `1` followed by a dot is a `1` and a dot, since a dot is not part of a number.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.; return 0; }") == "expected ';'");
    // An identifier is not a string, so a name that looks like one is a name -- and
    // so is `string`, which is not a type name in v1.
    parse_ok("fn main(): i32 { let hi: i32 = 1; return hi; }");
    parse_ok("fn main(): i32 { let string: i32 = 1; return string; }");
}

TEST_CASE("an error points at the token that caused it") {
    // The rule is one error at a place, so the location is part of the contract.
    CHECK(parse_error_location("fn main(): i32 {\n  let x: i32 = 1;\n  return y;\n}") ==
          "test.erax:3:10");
    CHECK(parse_error_location("fn main(): i32 { let x: i8 = 128; return 0; }") == "test.erax:1:30");
    // A rule about an operator points at the operator.
    CHECK(parse_error_location("fn main(): i32 { let x: i32 = 1 ++ 2; return 0; }") == "test.erax:1:33");
    // ... and a rule about a keyword, at the keyword.
    CHECK(parse_error_location("fn main(): i32 { break; return 0; }") == "test.erax:1:18");
}

TEST_CASE("a comment is not code") {
    // A line comment is the tokenizer's, so the parser sees the code after it.
    parse_ok("fn main(): i32 { let x: i32 = 1; // set x\n return x; }");
    parse_ok("// leading\nfn main(): i32 { return 0; }");
    // A block comment is a comment too, and can sit between any two tokens.
    parse_ok("fn main(): i32 { /* not code */ return /* nothing */ 0; }");
    // A block comment that runs off the end of the file is the tokenizer's error.
    CHECK(any_error("fn main(): i32 { return 0; } /* unterminated") != "");
}

TEST_CASE("every keyword is a reserved word, not a name") {
    for (const char* word : {"let", "fn", "if", "else", "while", "do", "for", "break", "continue", "return",
                             "true", "false", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "bool",
                             "char"}) {
        const std::string source = "fn main(): i32 { let " + std::string{word} + ": i32 = 1; return 0; }";
        CHECK(parse_error(source) == "'" + std::string{word} + "' is a reserved word");
    }
    // A keyword in a call position is still a reserved word.
    CHECK(parse_error("fn main(): i32 { return for(1); }") == "'for' is a reserved word");
}
