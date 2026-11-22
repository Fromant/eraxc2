#include "ParserTest.hpp"

// A namespace (`docs/core/language.md#namespaces`) is a scope for the top-level
// names, and names are reached qualified: `n.f()` calls the `f` in `n`, `n.g`
// reads its global, and a bare name searches the current namespace outward. The
// rules below are the resolution, clash, and shape ones — what parses, what each
// name means, and what a same-named thing may not do.

using namespace erax::test;

TEST_CASE("a namespace holds functions and globals, reached by a qualified name") {
  parse_ok(R"(
namespace n {
  fn f(x: i32): i32 { return x + 1; }
  let g: i32 = 1;
  fn h(): i32 { return g * 2; }
}
fn main(): i32 { return n.h() + n.g + n.f(1); }
)");
}

TEST_CASE("a namespace-qualified global is a constant expression operand") {
  const Program program = parse_ok(R"(
namespace n { let g: i32 = 1; }
let h: i32 = n.g + 1;
fn main(): i32 { return h + n.g; }
)");
  // `n.g + 1` folds at declaration time, the way an unqualified global's value does.
  CHECK(program.globals[1].init.blocks[0].commands[0].args[1].value == 2);
}

TEST_CASE("a qualified name is an assignment target") {
  parse_ok(R"(
namespace n { let g: i32 = 1; }
fn main(): i32 { n.g = 5; n.g += 3; return n.g; }
)");
  parse_ok(R"(
namespace n { let g: i32 = 0; }
fn main(): i32 { for (n.g = 0; n.g < 10; n.g = n.g + 1) { } return n.g; }
)");
}

TEST_CASE("namespaces nest, and the first segment searches outward") {
  parse_ok(R"(
namespace a {
  namespace b {
    fn f(): i32 { return 7; }
    let g: i32 = 3;
  }
}
fn main(): i32 { return a.b.f() + a.b.g; }
)");
  // A sibling nested namespace is reached unqualified from inside another.
  parse_ok(R"(
namespace n { fn f(): i32 { return 1; } }
namespace m { fn g(): i32 { return n.f(); } }
fn main(): i32 { return m.g(); }
)");
}

TEST_CASE("a namespace's members shadow an enclosing namespace's") {
  // The `f` `g` calls are n's, and the trailing `f()` is the root's.
  parse_ok(R"(
fn f(): i32 { return 1; }
namespace n {
  fn f(): i32 { return 2; }
  fn g(): i32 { return f(); }
}
fn main(): i32 { return n.g() + f(); }
)");
}

TEST_CASE("a namespace body sees its unqualified names and its ancestors'") {
  parse_ok(R"(
namespace a {
  fn f(): i32 { return 1; }
  namespace b {
    fn g(): i32 { return f(); }
  }
}
fn main(): i32 { return a.b.g(); }
)");
}

TEST_CASE("a namespace carries overloads and templates like any scope") {
  parse_ok(R"(
namespace n {
  fn f(x: i32): i32 { return x; }
  fn f(x: u8): u8 { return x; }
  fn id<T>(x: T): T { return x; }
  fn use_u8(): u8 { return id(1u8); }
}
fn main(): i32 { return n.f(1) + (n.f(1u8) as i32) + (n.use_u8() as i32); }
)");
}

TEST_CASE("the pre-pass and item pass number namespaces' items in the same order") {
  // Declarations interleave the root and two namespaces; both passes recurse in
  // the same source order, so every function's id satisfies the pass agreement.
  parse_ok(R"(
namespace n { let a: i32 = 1; }
let b: i32 = 2;
namespace m {
  let c: i32 = 3;
  fn f(): i32 { return c; }
}
fn main(): i32 { return n.a + b + m.f(); }
)");
}

TEST_CASE("a function in a namespace is only a callable, and a namespace is not a value") {
  CHECK(parse_error(R"(
namespace n { fn f(): i32 { return 0; } }
fn main(): i32 { let x: i32 = n.f; x = x + 1; return 0; }
)") == "'n.f' is a function and can only be called");
  CHECK(parse_error(R"(
namespace n { }
fn main(): i32 { let x: i32 = n; return 0; }
)") == "'n' is a namespace, not a value");
}

TEST_CASE("a name belongs to one kind of thing in a namespace") {
  CHECK(parse_error(R"(
namespace n { }
namespace n { }
fn main(): i32 { return 0; }
)") == "'n' is already declared");
  CHECK(parse_error(R"(
let g: i32 = 1;
namespace g { }
fn main(): i32 { return 0; }
)") == "'g' is already declared");
  CHECK(parse_error(R"(
fn f(): i32 { return 0; }
namespace f { }
fn main(): i32 { return 0; }
)") == "'f' is already declared");
  CHECK(parse_error(R"(
namespace n { let x: i32 = 1; fn x(y: i32): i32 { return y; } }
fn main(): i32 { return 0; }
)") == "'x' is already declared");
  // Root and inner namespaces may reuse a name across the boundary, though.
  parse_ok(R"(
namespace n { fn f(): i32 { return 1; } }
fn f(): i32 { return 2; }
fn main(): i32 { return f(); }
)");
}

TEST_CASE("main is only at the root") {
  CHECK(parse_error(R"(
namespace n { fn main(): i32 { return 0; } }
)") == "the entry point must be 'fn main(): i32'");
  CHECK(parse_error(R"(
namespace n { }
)") == "the program must declare 'fn main(): i32'");
}

TEST_CASE("a qualified name that does not reach a namespace's member errors") {
  CHECK(parse_error(R"(
namespace a { }
fn main(): i32 { return a.b; }
)") == "undeclared identifier 'a.b'");
  CHECK(parse_error(R"(
namespace a { }
fn main(): i32 { return a.b.c; }
)") == "unknown namespace 'a.b'");
  // The first segment resolves outward, so a local never takes the qualified path.
  CHECK(parse_error(R"(
fn main(): i32 { let a: i32 = 1; return a.b; }
)") == "member access needs a structure, not i32");
}