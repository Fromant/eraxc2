#include "ParserTest.hpp"
#include "interp/Interpreter.hpp"

using namespace erax::test;

namespace {
  erax::interp::Outcome execute_owned(std::string_view source) {
    const auto program = parse_ok(source);
    auto outcome = erax::interp::run(program);
    if (!outcome) {
      INFO(erax::interp::to_string(outcome.error()));
    }
    REQUIRE(outcome.has_value());
    return *outcome;
  }
}

TEST_CASE("bindings infer local types and keep const separate from mut") {
  CHECK(execute_owned("fn main(): i32 { const n = 41; let answer = n; answer += 1; return answer; }").code == 42);
  CHECK(execute_owned("fn main(): i32 { const mut n = 41; return n; }").code == 41);
  CHECK(execute_owned("fn main(): i32 { let mut n: i32; n = 7; return n; }").code == 7);
  CHECK(execute_owned("const n: i32 = 7; fn main(): i32 { return n; }").code == 7);
  CHECK(parse_error("fn main(): i32 { const n = 1; n = 2; return n; }") == "cannot assign to a const binding");
  CHECK(parse_error("fn main(): i32 { const mut n = 1; n = 2; return n; }") == "cannot assign to a const binding");
  CHECK(parse_error("const n: i32 = 1; fn main(): i32 { n = 2; return n; }") == "cannot assign to a const binding");
  CHECK(parse_error("fn main(): i32 { const n: i32; return 0; }") == "a const declaration needs an initializer");
  CHECK(parse_error("fn f() {} fn main(): i32 { const n = f(); return 0; }") == "a variable cannot have type void");
}

TEST_CASE("move consumes a local and let can be initialized again") {
  CHECK(execute_owned("fn main(): i32 { const a = 7; const b = move a; return b; }").code == 7);
  CHECK(execute_owned("fn main(): i32 { let a = 7; const b = move a; a = 9; return a + b; }").code == 16);
  CHECK(parse_error("fn main(): i32 { const a = 7; const b = move a; return a; }") == "'a' is used after it was moved");
  CHECK(parse_error("fn main(): i32 { let a = 7; a = move a; return 0; }") == "cannot move a value into itself");
  CHECK(parse_error("fn main(): i32 { const a = 7; const b = move a; a = 9; return b; }") ==
        "cannot assign to a const binding");
  CHECK(parse_error("fn main(): i32 { const a = move 7; return a; }") == "move needs a local variable or parameter");
  CHECK(execute_owned("fn take(a: i32): i32 { return move a; } fn main(): i32 { const a = 7; return take(move a); }")
            .code == 7);
  CHECK(parse_error("fn f(a: i32, b: i32) {} fn main(): i32 { let a = 7; f(move a, a); return 0; }") ==
        "'a' is used after it was moved");
}

TEST_CASE("move states merge only the paths that reach a join") {
  CHECK(execute_owned("fn f(flag: bool): i32 { let a = 7; if (flag) { return move a; } else { return a; } } "
                      "fn main(): i32 { return f(false); }")
            .code == 7);
  CHECK(execute_owned("fn f(flag: bool): i32 { let a = 7; if (flag) { const b = move a; return b; } return a; } "
                      "fn main(): i32 { return f(false); }")
            .code == 7);
  CHECK(parse_error("fn f(flag: bool): i32 { let a = 7; if (flag) { move a; } return a; } "
                    "fn main(): i32 { return f(false); }") == "'a' is used after it was moved");
  CHECK(execute_owned("fn main(): i32 { let a = 7; if (false) { move a; } return a; }").code == 7);
  CHECK(execute_owned("fn main(): i32 { let a = 7; while (false) { move a; } return a; }").code == 7);
  CHECK(execute_owned("fn main(): i32 { let a = 7; for (; false;) { move a; } return a; }").code == 7);
  CHECK(execute_owned("fn f(flag: bool): i32 { let a = 7; if (flag) { move a; } else { move a; } "
                      "a = 9; return a; } fn main(): i32 { return f(true); }")
            .code == 9);
}

TEST_CASE("a move cannot read the previous iteration's consumed value") {
  CHECK(parse_error("fn main(): i32 { let a = 7; while (true) { move a; } return 0; }") ==
        "'a' may be uninitialized or moved on a control-flow path");
  CHECK(parse_error("fn main(): i32 { let a = 7; do { move a; } while (true); return 0; }") ==
        "'a' may be uninitialized or moved on a control-flow path");
  CHECK(parse_error("fn main(): i32 { let a = 7; for (let i = 0; i < 2; i += 1) { move a; } return 0; }") ==
        "'a' may be uninitialized or moved on a control-flow path");
  CHECK(parse_error("fn main(): i32 { let a = 7; while (true) { move a; continue; a = 8; } return 0; }") ==
        "'a' may be uninitialized or moved on a control-flow path");
  CHECK(execute_owned("fn main(): i32 { let a = 7; while (true) { const b = move a; return b; } }").code == 7);
  CHECK(execute_owned("fn main(): i32 { let a = 7; do { move a; } while (false); a = 9; return a; }").code == 9);
  CHECK(execute_owned("fn main(): i32 { let a: i32; for (let i = 0; i < 2; i = move a) { a = i + 1; } return 7; }")
            .code == 7);
  CHECK(
      execute_owned("fn main(): i32 { let a = 7; for (let i = 0; i < 2; i += 1) { move a; a = 8; } return a; }").code ==
      8);
}

TEST_CASE("assignment chains store from right to left and have no expression value") {
  CHECK(execute_owned("fn main(): i32 { let a = 0; let b = 0; let c = 0; a = b = c = 7; return a + b + c; }").code ==
        21);
  CHECK(execute_owned("let g: i32 = 0; namespace n { let g: i32 = 0; } "
                      "fn main(): i32 { let a = 0; a = n.g = g = 7; return a + n.g + g; }")
            .code == 21);
  CHECK(execute_owned("fn main(): i32 { let a = 1; let b = 2; let p = &b; a = *p = 7; return a + b; }").code == 14);
  CHECK(execute_owned("fn main(): i32 { let a = 1; let b = 2; let p = &b; *p = a = 7; return a + b; }").code == 14);
  CHECK(parse_error("fn main(): i32 { let a = 0; let b = 0; a = b += 1; return 0; }") == "only '=' may be chained");
  CHECK(parse_error("fn main(): i32 { let a = 0; const b = 0; a = b = 1; return a; }") ==
        "cannot assign to a const binding");
  CHECK(parse_error("fn main(): i32 { let a = 0; const b = (a = 1); return a; }") != "");
}

TEST_CASE("dependent pointer signatures deduce recursively and substitute their results") {
  CHECK(execute_owned("fn read<T>(p: ptr T): T { return *p; } "
                      "fn main(): i32 { let a = 7; return read(&a); }")
            .code == 7);
  CHECK(execute_owned("fn identity<T>(p: ptr T): ptr T { return p; } "
                      "fn main(): i32 { let a = 7; let p = identity(&a); return *p; }")
            .code == 7);
  CHECK(execute_owned("fn nested<T>(p: ptr ptr T): T { let q = *p; return *q; } "
                      "fn main(): i32 { let a = 7; let p = &a; return nested(&p); }")
            .code == 7);
  CHECK(execute_owned("fn echo<T extends Clonable & Movable>(value: T): T { return move value; } "
                      "fn main(): i32 { return echo(7); }")
            .code == 7);
  CHECK(parse_error("fn same<T>(p: ptr T, value: T) {} "
                    "fn main(): i32 { let a = 1; same(&a, true); return 0; }") ==
        "cannot deduce 'T' of 'same': the arguments give i32 and bool");
}

TEST_CASE("reallocated blocks are zeroed and freed cells cannot be accessed") {
  CHECK(execute_owned("fn main(): i32 { let p = primitive alloc(8); *p = 77; primitive free(p); "
                      "let q = primitive alloc(8); return *q as i32; }")
            .code == 0);
  for (const auto statement : {"return *p as i32;", "*p = 42; return 0;"}) {
    const auto program =
        parse_ok(std::string("fn main(): i32 { let p = primitive alloc(8); primitive free(p); ") + statement + " }");
    const auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(std::get<erax::interp::Trapped>(outcome.error()).trap == erax::interp::Trap::InvalidPointer);
  }
  CHECK(execute_owned("fn main(): i32 { let a = primitive alloc(8); let b = primitive alloc(8); "
                      "*b = 42; primitive free(a); return *b as i32; }")
            .code == 42);
}
