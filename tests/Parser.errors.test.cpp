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
  // A dotted name is a namespace-qualified reference, so a dot that does not lead
  // to a declared namespace is the unknown-namespace error; one that leads to a
  // namespace without the member is undeclared.
  CHECK(parse_error("fn main(): i32 { return a.b(); }") == "unknown namespace 'a'");
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1; return a.b; }") == "member access needs a structure, not i32");
  CHECK(parse_error("namespace a { } fn main(): i32 { return a.b(); }") == "call to undeclared function 'a.b'");
  CHECK(parse_error("namespace a { } fn main(): i32 { return a.b.c; }") == "unknown namespace 'a.b'");
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
  CHECK(parse_error("let g: i32 = 1; let g: i32 = 2; fn main(): i32 { return 0; }") == "'g' is already declared");
  CHECK(parse_error("fn f(): i32 { return 0; } fn f(): i32 { return 0; } fn main(): i32 { return 0; }") ==
        "'f' is already declared");
  // `main` is no exception, so two entrypoints are a duplicate and not a feature.
  CHECK(parse_error("fn main(): i32 { return 0; } fn main(): i32 { return 0; }") == "'main' is already declared");
  // A new parameter list under one name is an overload set, not a repeat: it
  // declares. Only the same parameters twice are the same declaration twice.
  parse_ok("fn f(x: i32): i32 { return x; } fn f(x: bool): i32 { return 1; } fn main(): i32 { return f(1); }");
  parse_ok("fn f(): i32 { return 1; } fn f(x: i32): i32 { return x; } fn main(): i32 { return f(); }");
  CHECK(parse_error("fn f(x: i32): i32 { return x; } fn f(x: i32): i32 { return x; } fn main(): i32 { return 0; }") ==
        "'f' is already declared");
  // The return type is not part of a function's identity, so it never separates two.
  CHECK(parse_error("fn f(): i32 { return 1; } fn f(): bool { return true; } fn main(): i32 { return 0; }") ==
        "'f' is already declared");
  // A global shares the flat namespace with every member of a set, either order.
  CHECK(parse_error("fn f(x: i32): i32 { return x; } let f: i32 = 1; fn main(): i32 { return 0; }") ==
        "'f' is already declared");
  CHECK(parse_error("let f: i32 = 1; fn f(x: i32): i32 { return x; } fn main(): i32 { return 0; }") ==
        "'f' is already declared");
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
  CHECK(parse_error("fn main(): i32 { return nope(1, 2, 3); }") == "call to undeclared function 'nope'");
  // A name that is a local is not a function, so the same message says it is not one.
  CHECK(parse_error("fn main(): i32 { let f: i32 = 0; return f(); }") == "value 'f' is not callable");
  // A function that is declared is callable, so this is about the signature instead.
  CHECK(parse_error("fn f(): i32 { return 0; } fn main(): i32 { return f(1); }") != "call to undeclared function 'f'");
  // An overload set with no candidate for these arguments says which ones it saw,
  // in the order they were written.
  CHECK(parse_error("fn f(x: i32): i32 { return x; } fn f(x: bool): i32 { return 1; }"
                    " fn main(): i32 { return f(true, 1); }") ==
        "no overload of 'f' matches the arguments (bool, i32)");
  CHECK(parse_error("fn f(x: i32): i32 { return x; } fn f(a: i32, b: i32): i32 { return a; }"
                    " fn main(): i32 { return f(); }") == "no overload of 'f' matches the arguments ()");
  CHECK(parse_error("fn f(x: i32): i32 { return x; } fn f(x: i64): i32 { return 1; }"
                    " fn main(): i32 { let b: bool = true; return f(b); }") ==
        "no overload of 'f' matches the arguments (bool)");
}

TEST_CASE("a for header is init; test; step, and each part says what it wanted") {
  // The three parts are separated, so a missing `;` is caught at the next part
  // rather than being read as part of the one before it.
  CHECK(parse_error("fn main(): i32 { for (let = 0; i < 1; i = i + 1) { } return 0; }") == "expected an identifier");
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 1 i < 2; i = i + 1) { } return 0; }") == "expected ';'");
  // The step is an assignment, not an expression, so a bare expression there is an
  // error rather than a write the loop only sometimes does.
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 0; i < 2; i + 1) { } return 0; }") ==
        "expected an assignment operator");
  // ... and a missing `)` is caught after it.
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (i = 0; i < 2; i = i + 1 { } return 0; }") == "expected ')'");
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for i = 0; i < 2; i = i + 1) { } return 0; }") == "expected '('");
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
  // A call that returns nothing is not an operand. The error now comes from the
  // operator checking for void operands.
  CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = f() + 1; return 0; }") ==
        "'+' cannot be applied to a call that returns nothing");
  CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = 1; if (f() > 1) { return 1; } return 0; }") ==
        "'>' cannot be applied to a call that returns nothing");
  // A unary operator names the operator and the void it was given, which is the more
  // useful pair of the two, so the two wordings differ.
  CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = -f(); return 0; }") == "'-' is not defined for void");
  CHECK(parse_error("fn f() { } fn main(): i32 { let x: bool = !f(); return 0; }") ==
        "'!' is defined on 'bool', but got void");
  // A void value on its own is fine, which is what a statement of `f();` is.
  parse_ok("fn f() { } fn main(): i32 { f(); return 0; }");
}

TEST_CASE("a bool is not an integer, whichever way it is used") {
  // A bool in an integer position is a type mismatch, and the message names both.
  CHECK(parse_error("fn main(): i32 { let x: i32 = true; return 0; }") == "expected a value of type i32, not bool");
  CHECK(parse_error("fn main(): i32 { let x: u8 = false; return 0; }") == "expected a value of type u8, not bool");
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
  CHECK(any_error("fn main(): i32 { let c: char = '\\q'; return 0; }") == "invalid escape sequence '\\q'");
  CHECK(any_error("fn main(): i32 { let c: char = '\\'; return 0; }") != "");
  CHECK(any_error("fn main(): i32 { let s: string = \"a\\qb\"; return 0; }") != "");
  // The named ones are the C set, and they are the only ones.
  parse_ok("fn main(): i32 { let a: char = '\\n'; let b: char = '\\t'; let c: char = '\\r';"
           " let d: char = '\\0'; let e: char = '\\\\'; let f: char = '\\''; return 0; }");
}

TEST_CASE("the cursor names what it wanted to see") {
  // Each expectation says what the token is, by its own name.
  CHECK(parse_error("fn main(): i32 { let x; return 0; }") == "expected ':' or '=' after the variable name");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1 x = 2; return 0; }") == "expected ';'");
  CHECK(parse_error("fn main(): i32 { return 0; } ]") == "expected a global, a function, or a namespace declaration");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return 0; } )") ==
        "expected a global, a function, or a namespace declaration");
  // A keyword expectation names the keyword.
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; do { i = i - 1; } x; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let i: i32 = 0; for (let i: i32 = 0; i < 1; i = i + 1) break; return 0; }") !=
        "");
  // An operator expectation quotes the operator.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x ? 1; }") == "unknown operator '?'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x ?? 1; }") ==
        "unknown operator '?"
        "?'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x <<= 1; }") != "");
  // A maximal-munch run is one operator, so `!=` is one and `!==` is not.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let b: bool = x !== 1; return 0; }") == "unknown operator '!=='");
  parse_ok("fn main(): i32 { let x: i32 = 1; let b: bool = x != 1; return 0; }");
}

TEST_CASE("a string and a float are not types yet") {
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5; return 0; }") == "float is not supported yet");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5 + 1; return 0; }") == "float is not supported yet");
  CHECK(parse_error("fn main(): i32 { let x: string = \"hi\"; return 0; }") == "string is not supported yet");
  CHECK(parse_error("fn main(): i32 { let x: i32 = \"hi\"; return 0; }") == "string literals require std.string");
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
  CHECK(parse_error_location("fn main(): i32 {\n  let x: i32 = 1;\n  return y;\n}") == "test.erx:3:10");
  CHECK(parse_error_location("fn main(): i32 { let x: i8 = 128; return 0; }") == "test.erx:1:30");
  // A rule about an operator points at the operator.
  CHECK(parse_error_location("fn main(): i32 { let x: i32 = 1 ++ 2; return 0; }") == "test.erx:1:33");
  // ... and a rule about a keyword, at the keyword.
  CHECK(parse_error_location("fn main(): i32 { break; return 0; }") == "test.erx:1:18");
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
  for (const char* word :
       {"let", "fn", "if",  "else", "while", "do",  "for", "break", "continue", "return", "true",      "false",
        "i8",  "u8", "i16", "u16",  "i32",   "u32", "i64", "u64",   "bool",     "char",   "primitive", "as"}) {
    const std::string source = "fn main(): i32 { let " + std::string {word} + ": i32 = 1; return 0; }";
    CHECK(parse_error(source) == "'" + std::string {word} + "' is a reserved word");
  }
  // A keyword in a call position is still a reserved word.
  CHECK(parse_error("fn main(): i32 { return for(1); }") == "'for' is a reserved word");
}

TEST_CASE("a cast takes a type and a value that has bits") {
  // The type comes right after `as`, and anything else is said at the `as`.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 5 as; return 0; }") == "expected a type after 'as'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 5 as nope; return 0; }") == "expected a type after 'as'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 5 as string; return 0; }") == "string is not supported yet");
  // Nothing comes out of a void call: there are no bits to convert.
  CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = f() as i32; return 0; }") ==
        "cannot cast a value of type void");
  // The cast is the conversion, so the result is the target's type and the
  // position still has to match it — no second conversion on the way in.
  CHECK(parse_error("fn main(): i32 { let x: u64 = 5 as u8; return 0; }") != "");
  // `as` names a type after itself and nothing else: not a name, not a type.
  CHECK(parse_error("fn main(): i32 { let as: i32 = 1; return 0; }") == "'as' is a reserved word");
  CHECK(parse_error("fn main(): i32 { let x: as = 1; return 0; }") == "expected a type");
}

TEST_CASE("a pointer names a cell, a deref needs a pointer, and a store writes through") {
  // A deref whose operand is not a pointer is a type error.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return *x; }") == "'*' needs a pointer, but got i32");
  // `&` takes the address of a variable or global's cell: nothing else has one,
  // so a constant, a deref (a scratch vreg), and an intermediate all die.
  CHECK(parse_error("fn main(): i32 { let p: ptr i32 = &42; return 0; }") == "cannot take the address of a constant");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &(x + 1); return 0; }") ==
        "cannot take the address of this expression");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &(*(&x)); return 0; }") ==
        "cannot take the address of this expression");
  // A store's right side and a store target's type must agree; the compound
  // operators are the integer ones, so a pointer pointee cannot compound.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &x; *p = false; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let x: bool = true; let p: ptr bool = &x; *p += 1; return 0; }") != "");
  // `ptr` is contextual: a type in type position, an ordinary name elsewhere.
  parse_ok("fn main(): i32 { let ptr: i32 = 1; let p: ptr i32 = &ptr; return *p; }");
}

TEST_CASE("a signature may name concrete and dependent pointers") {
  parse_ok("fn f(p: ptr i32): i32 { return 0; } fn main(): i32 { return 0; }");
  parse_ok("fn g<T>(p: ptr T) { } fn main(): i32 { return 0; }");
  // `string` is named as itself rather than as a missing type.
  CHECK(parse_error("fn f(p: string) { } fn main(): i32 { return 0; }") == "string is not supported yet");
}

TEST_CASE("the constant-only positions say what a constant is") {
  // A global's type says how it starts, so a call is not one.
  CHECK(parse_error("fn f(): i32 { return 0; } let g: i32 = f(); fn main(): i32 { return 0; }") ==
        "a constant expression may not call 'f'");
  CHECK(parse_error("let g: ptr u8 = primitive alloc(8); fn main(): i32 { return 0; }") ==
        "a constant expression may not call a primitive");
  // A deref reads memory, which a constant expression cannot do.
  CHECK(parse_error("let g: i32 = *(0 as ptr i32); fn main(): i32 { return 0; }") ==
        "this must be a constant expression");
}

TEST_CASE("a unary operator names the type it was handed") {
  // The non-literal negation: the `-` in front of a number is read with the sign.
  CHECK(parse_error("fn main(): i32 { let y: u8 = 2; let x: u8 = -y; return 0; }") == "'-' is not defined for u8");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let b: bool = !x; return 0; }") ==
        "'!' is defined on 'bool', but got i32");
  CHECK(parse_error("fn main(): i32 { let b: bool = true; let x: i32 = ~b; return 0; }") ==
        "'~' is not defined for bool");
  // The most negative value has no negation, and a shift that drops the top bit
  // has no value either.
  CHECK(parse_error("fn main(): i32 { let x: i32 = -(-2147483647 - 1); return 0; }") ==
        "this constant does not fit i32");
  CHECK(parse_error("fn main(): i32 { let x: i64 = -1i64 << 1i64; return 0; }") == "this constant does not fit i64");
}

TEST_CASE("a literal suffix that names nothing is quoted back") {
  CHECK(parse_error("fn main(): i32 { let x: i32 = 64ul; return 0; }") == "'ul' is not a type");
}

TEST_CASE("a primitive is one identifier") {
  CHECK(parse_error("fn main(): i32 { let p: ptr u8 = primitive .alloc(8); return 0; }") ==
        "'.' is not part of a name in v1");
  CHECK(parse_error("fn main(): i32 { let p: ptr u8 = primitive alloc.now(8); return 0; }") ==
        "'.' is not part of a name in v1");
  CHECK(parse_error("fn main(): i32 { return std.; }") == "expected an identifier");
}

TEST_CASE("a store names a pointer and its pointee's type") {
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; *x = 5; return 0; }") ==
        "cannot assign through a value of type i32, which is not a pointer");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &x; let y: i64 = 2; *p = y; return 0; }") ==
        "cannot assign i64 through a pointer to i32");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &x; let y: i64 = 2; *p += y; return 0; }") ==
        "cannot assign i64 through a pointer to i32");
  // A store that never says how to store is still a missing semicolon.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let p: ptr i32 = &x; *p = 1 return 0; }") == "expected ';'");
}

TEST_CASE("an assignment target is a name that exists") {
  CHECK(parse_error("namespace a { } fn main(): i32 { a.b = 1; return 0; }") == "undeclared identifier 'a.b'");
  CHECK(parse_error("fn main(): i32 { z.y = 1; return 0; }") == "unknown namespace 'z'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x = ; return 0; }") == "expected an expression");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x += ; return 0; }") == "expected an expression");
}

TEST_CASE("a global declaration names its type and its start") {
  CHECK(parse_error("let = 1; fn main(): i32 { return 0; }") == "expected an identifier");
  CHECK(parse_error("let x 1; fn main(): i32 { return 0; }") == "expected ':'");
  CHECK(parse_error("let x: 1; fn main(): i32 { return 0; }") == "expected a type");
  CHECK(parse_error("let x: i32 1; fn main(): i32 { return 0; }") == "expected '='");
}

TEST_CASE("a namespace body is named, opened, and closed") {
  CHECK(parse_error("namespace { } fn main(): i32 { return 0; }") == "expected an identifier");
  CHECK(parse_error("namespace a fn main(): i32 { return 0; }") == "expected '{'");
  CHECK(parse_error("namespace a { let g: i32 = 1;") == "unterminated namespace body");
}

TEST_CASE("a body that never closes is named") {
  // The declaration pass skips each body to find where the next declaration
  // starts, so an unclosed body is caught there before the body is ever parsed.
  CHECK(parse_error("fn main(): i32 {") == "unterminated function body");
  CHECK(parse_error("fn main(): i32 { return 0; ") == "unterminated function body");
  CHECK(parse_error("fn f<T>(x: T) {") == "unterminated function body");
}

TEST_CASE("a loop body and its header are parsed like any block") {
  CHECK(parse_error("fn main(): i32 { if (true) { let; } return 0; }") == "expected an identifier");
  CHECK(parse_error("fn main(): i32 { if (false) { } else if (true) { let; } return 0; }") == "expected an identifier");
  CHECK(parse_error("fn main(): i32 { if (false) { } else { let; } return 0; }") == "expected an identifier");
  CHECK(parse_error("fn main(): i32 { do { let; } while (false); return 0; }") == "expected an identifier");
  CHECK(parse_error("fn main(): i32 { do { } while (false) return 0; }") == "expected ';'");
  CHECK(parse_error("fn main(): i32 { for (1 + 2; ; ) { } return 0; }") ==
        "expected a declaration or an assignment in the 'for' init");
  // A `for` init and a `do` test may be the forms the loops allow.
  parse_ok("fn main(): i32 { let x: i32 = 0; let p: ptr i32 = &x; for (*p = 0; *p < 3; *p += 1) { } return 0; }");
  parse_ok("fn main(): i32 { do { } while (true); return 0; }");
}

TEST_CASE("a void function returns with nothing, a value function with a value") {
  parse_ok("fn f() { return; } fn main(): i32 { f(); return 0; }");
}
