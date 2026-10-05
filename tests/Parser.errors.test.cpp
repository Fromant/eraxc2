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

TEST_CASE("a name is bound once, and the message says which kind") {
    // A duplicate local, a duplicate parameter and a duplicate global all say the same
    // thing, because they are all the same mistake: one name, bound twice.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let x: i32 = 2; return 0; }") ==
          "'x' is already declared in this scope");
    CHECK(parse_error("fn f(x: i32, x: i32): i32 { return x; } fn main(): i32 { return 0; }") ==
          "'x' is already declared in this scope");
    // A global and a function are at the top of a program rather than in a scope, so
    // they are 'already declared' and not 'in this scope' -- there is no scope to name.
    CHECK(parse_error("let g: i32 = 1; let g: i32 = 2; fn main(): i32 { return 0; }") ==
          "'g' is already declared");
    CHECK(parse_error("fn f(): i32 { return 0; } fn f(): i32 { return 0; } fn main(): i32 { return 0; }") ==
          "'f' is already declared");
    // `main` is no exception, so two entrypoints are a duplicate and not a feature.
    CHECK(parse_error("fn main(): i32 { return 0; } fn main(): i32 { return 0; }") ==
          "'main' is already declared");
    // Shadowing an *outer* local is allowed, so the message only ever fires for the
    // scope the name was introduced in.
    parse_ok("fn main(): i32 { let x: i32 = 1; if (true) { let x: i32 = 2; } return x; }");
}

TEST_CASE("a call names a function that exists, or says which one it wanted") {
    // A call to a name nothing is declared as is a name error, not a type error, so it
    // is reported at the call and names the callee.
    CHECK(parse_error("fn main(): i32 { return nope(1); }") == "call to undeclared function 'nope'");
    CHECK(parse_error("fn main(): i32 { return nope(); }") == "call to undeclared function 'nope'");
    // ... and it fires whether or not there are arguments to get wrong first.
    CHECK(parse_error("fn main(): i32 { return nope(1, 2, 3); }") ==
          "call to undeclared function 'nope'");
    // A name that is a local is not a function, so the same message says it is not one.
    CHECK(parse_error("fn main(): i32 { let f: i32 = 0; return f(); }") ==
          "call to undeclared function 'f'");
    // A function that is declared is callable, so this is about the signature instead.
    CHECK(parse_error("fn f(): i32 { return 0; } fn main(): i32 { return f(1); }") !=
          "call to undeclared function 'f'");
}

TEST_CASE("a for header is init; test; step, and each part says what it wanted") {
    // The three parts are separated, so a missing `;` is caught at the next part
    // rather than being read as part of the one before it.
    CHECK(parse_error("fn main(): i32 { for (let = 0; i < 1; i = i + 1) { } return 0; }") ==
          "expected an identifier");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 1 i < 2; i = i + 1) { } return 0; }") ==
          "expected ';'");
    // The step is an assignment, not an expression, so a bare expression there is an
    // error rather than a write the loop only sometimes does.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 0; i < 2; i + 1) { } return 0; }") ==
          "expected an assignment operator");
    // ... and a missing `)` is caught after it.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 0; i < 2; i = i + 1 { } return 0; }") ==
          "expected ')'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for i = 0; i < 2; i = i + 1) { } return 0; }") ==
          "expected '('");
    // A body that is never closed is a missing `}`, and it is reported at the function
    // rather than at the loop, because the loop's own `}` never arrives.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 0; i < 2; i = i + 1) { return 0; }") ==
          "unterminated function body");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while (i < 2) { i = i + 1; return 0; }") ==
          "unterminated function body");
    // A `for` with no test is an infinite loop, so it is legal and has no exit edge of
    // its own: only a `break` gets out.
    const Program forever =
        parse_ok("fn main(): i32 { let i: i32 = 0; for (i = 0;;i = i + 1) { if (i > 3) { break; } } return i; }");
    CHECK(ops_of(forever, 0).size() > 4);
    // The empty init is legal too, and then the first `;` is the one that ends it.
    parse_ok("fn main(): i32 { let i: i32 = 0; for (; i < 2; i = i + 1) { } return i; }");
}

TEST_CASE("a void call has no value, and the arithmetic says so") {
    // A call that returns nothing is not an operand. The next operand is read in the
    // type of the one before it, so `void` is what the position asks for and the
    // literal is what it does not get.
    CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = f() + 1; return 0; }") ==
          "expected a value of type void, not an integer literal");
    CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = 1; if (f() > 1) { return 1; } return 0; }") ==
          "expected a value of type void, not an integer literal");
    // A unary operator names the operator and the void it was given, which is the more
    // useful pair of the two, so the two wordings differ.
    CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = -f(); return 0; }") ==
          "'-' is not defined for void");
    CHECK(parse_error("fn f() { } fn main(): i32 { let x: bool = !f(); return 0; }") ==
          "'!' is defined on 'bool', but got void");
    // A void value on its own is fine, which is what a statement of `f();` is.
    parse_ok("fn f() { } fn main(): i32 { f(); return 0; }");
}

TEST_CASE("a bool is not an integer, whichever way it is used") {
    // A bool in an integer position is a type mismatch, and the message names both.
    CHECK(parse_error("fn main(): i32 { let x: i32 = true; return 0; }") ==
          "expected a value of type i32, not bool");
    CHECK(parse_error("fn main(): i32 { let x: u8 = false; return 0; }") ==
          "expected a value of type u8, not bool");
    // As an operand of a bitwise operator it is named as the operand's type, not as a
    // mismatch with the result, because `|` is not a bool operator to begin with.
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: i8 = a | b; return 0; }") ==
          "'|' is not defined for bool");
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: i8 = a & b; return 0; }") ==
          "'&' is not defined for bool");
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: i8 = a ^ b; return 0; }") ==
          "'^' is not defined for bool");
    // An integer in a bool position is the same error from the other side.
    CHECK(parse_error("fn main(): i32 { let x: bool = 1; return 0; }") != "");
    CHECK(parse_error("fn main(): i32 { let x: i8 = 1; let y: bool = x; return 0; }") != "");
}

TEST_CASE("an escape sequence is one of the ones the language names") {
    // An escape is a lexical rule, so it is rejected before the parser sees the
    // literal: `any_error` is the helper for a source the tokenizer refuses.
    CHECK(any_error("fn main(): i32 { let c: char = '\\q'; return 0; }") ==
          "invalid escape sequence '\\q'");
    CHECK(any_error("fn main(): i32 { let c: char = '\\'; return 0; }") != "");
    CHECK(any_error("fn main(): i32 { let s: string = \"a\\qb\"; return 0; }") != "");
    // The named ones are the C set, and they are the only ones.
    parse_ok("fn main(): i32 { let a: char = '\\n'; let b: char = '\\t'; let c: char = '\\r';"
             " let d: char = '\\0'; let e: char = '\\\\'; let f: char = '\\''; return 0; }");
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
