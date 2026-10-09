#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ParserTest.hpp"

using namespace erax::frontend;
using namespace erax::middleend;
using namespace erax::test;

TEST_CASE("a program lowers to globals, functions, and an entrypoint") {
  const Program program = parse_ok("let g: i32 = 1; fn main(): i32 { return g; }");
  REQUIRE(program.globals.size() == 1);
  CHECK(program.globals[0].type == Type::I32);
  REQUIRE(program.functions.size() == 1);
  CHECK(program.functions[0].return_type == Type::I32);
  CHECK(program.entrypoint == 0);

  // A global's init is a one-block CFG that writes the value and returns.
  const Cfg& init = program.globals[0].init;
  REQUIRE(init.blocks.size() == 1);
  REQUIRE(init.blocks[0].commands.size() == 2);
  CHECK(init.blocks[0].commands[0].op == Op::Mov);
  CHECK(init.blocks[0].commands[0].args[0].kind == OperandKind::Global);
  CHECK(init.blocks[0].commands[0].args[1].value == 1);
  CHECK(is_ret(init.blocks[0].commands[1]));
}

TEST_CASE("params are the first cells of the frame") {
  const Program program = parse_ok("fn f(a: i32, b: u8): bool { return a == 1; } fn main(): i32 { return 0; }");
  const Function& f = program.functions[0];
  CHECK(f.params == std::vector<Type> {Type::I32, Type::U8});
  CHECK(f.return_type == Type::I8);
  CHECK(f.frame_size >= 2);

  // `$0` and `$1` are the params; the comparison gets a cell of its own.
  const Cfg& cfg = body_of(program, 0);
  REQUIRE(cfg.blocks.size() == 1);
  const Command& compare = cfg.blocks[0].commands[0];
  REQUIRE(compare.op == Op::CmpEq);
  CHECK(compare.args[1].value == 0);
  CHECK(compare.args[2].value == 1);
}

TEST_CASE("a source type lowers to its machine type") {
  const Program program =
      parse_ok("let a: u8 = 1; let b: char = 'x'; let c: bool = true; let d: u64 = 1; fn main(): i32 { return 0; }");
  REQUIRE(program.globals.size() == 4);
  CHECK(program.globals[0].type == Type::U8);
  CHECK(program.globals[1].type == Type::U8);
  CHECK(program.globals[2].type == Type::I8);
  CHECK(program.globals[3].type == Type::U64);
  CHECK(program.globals[2].init.blocks[0].commands[0].args[1].value == 1);
  CHECK(program.globals[3].init.blocks[0].commands[0].args[1].value == 1);
}

TEST_CASE("an integer literal takes the type of the position it fills") {
  // Contextual: the declared type decides, and the value has to fit it.
  parse_ok("fn f(x: i8): i8 { let y: i8 = 127; return y; } fn main(): i32 { f(1); return 0; }");
  CHECK(parse_error("fn f(x: i8): i8 { let y: i8 = 128; return y; } fn main(): i32 { f(1); return 0; }") ==
        "'128' does not fit i8");
  // A negative value is not a u8, even though its bits would fit.
  CHECK(parse_error("fn main(): i32 { let x: u8 = -1; return 0; }") == "'-' is not defined for u8");
  // With no type to take, a literal is an i32.
  parse_ok("fn main(): i32 { 1 + 2; return 0; }");
  CHECK(parse_error("fn main(): i32 { let x: bool = 1; return 0; }") != "");
  // A `char` literal is a `char`, and it is not an integer to add to one.
  parse_ok("fn main(): i32 { let c: char = 'A'; return 0; }");
  CHECK(parse_error("fn main(): i32 { let c: char = 'A'; let x: i32 = c + 1; return x; }") != "");
  // A call's result is the callee's return type, with no widening on the way out.
  CHECK(parse_error("fn f(): i8 { return 1; } fn main(): i32 { return f(); }") ==
        "expected a value of type i32, not i8");
}

TEST_CASE("binary operands must have the same type") {
  parse_ok("fn main(): i32 { let a: i32 = 1; let b: i32 = 2; let c: i32 = a + b; return c; }");
  const std::string message =
      parse_error("fn main(): i32 { let a: i32 = 1; let b: u32 = 2; let c: i32 = a + b; return c; }");
  CHECK(message == "'+' needs two operands of the same type, but got i32 and u32");
  // A comparison is a `bool`, which is not an integer.
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1; let b: i32 = a < 2; return 0; }") != "");
  // ... and neither is it an `i32`.
  parse_ok("fn main(): i32 { let a: i32 = 1; let b: bool = a < 2; return 0; }");
  // A logical operator is on `bool` too.
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1; let b: bool = a && a; return 0; }") != "");
  parse_ok("fn main(): i32 { let a: bool = true; let b: bool = !a; return 0; }");
  // `!` has no integer form; `~` and `-` are on integers only.
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1; let b: i32 = ~!a; return b; }") != "");
  CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = -a; return 0; }") != "");
}

TEST_CASE("precedence follows the operator table") {
  // `*` binds tighter than `+`, and both tighter than the comparison, so this
  // is `1 + (4 * 2)` compared against 8, not `5` compared against `2`.
  const Program program = parse_ok("fn main(): i32 { let a: i32 = 1; let b: i32 = 4; let c: i32 = 2;"
                                   " let x: bool = a + b * c < 8; return 0; }");
  const Cfg& cfg = body_of(program, 0);
  std::vector<Op> ops;
  for (const Command& command : cfg.blocks[0].commands)
    ops.push_back(command.op);
  // Mul before Add before CmpLt, and the `&&` level binds looser than either.
  const auto index_of = [&ops](Op op) {
    return std::find(ops.begin(), ops.end(), op) - ops.begin();
  };
  REQUIRE(index_of(Op::Mul) < index_of(Op::Add));
  REQUIRE(index_of(Op::Add) < index_of(Op::CmpLt));

  // A literal-only initializer folds, so nothing but the write is emitted.
  const Program folded = parse_ok("fn main(): i32 { let x: i32 = 1 + 4 * 2; return x; }");
  const Cfg& straight = body_of(folded, 0);
  REQUIRE(straight.blocks[0].commands.size() == 2);
  CHECK(straight.blocks[0].commands[0].op == Op::Mov);
  CHECK(straight.blocks[0].commands[0].args[1].value == 9);
}

TEST_CASE("logical not is a test against zero") {
  // `!x` is `x == 0`, so it lowers to the comparison that says so: a `CMP_NE`
  // would make `!` a no-op, since a `bool` is 0 or 1 either way.
  const Program program = parse_ok("fn main(): i32 { let flag: bool = true; let other: bool = !flag; return 0; }");
  const auto commands = commands_of(program, 0);
  const auto not_at = std::find_if(commands.begin(), commands.end(), [](const Command& command) {
    return command.op == Op::CmpEq;
  });
  REQUIRE(not_at != commands.end());
  // The flag is the left operand and the zero it is tested against is an
  // instant, not a cell.
  CHECK(not_at->args[2].kind == OperandKind::Instant);
  CHECK(not_at->args[2].value == 0);
  CHECK(not_at->args[1].kind == OperandKind::VReg);
  // A `!` on a literal folds, so `!true` is `false` before any IR exists: the
  // only command left is the write.
  const Program folded = parse_ok("fn main(): i32 { let flag: bool = !true; return 0; }");
  const auto straight = commands_of(folded, 0);
  REQUIRE(straight.size() == 2);
  CHECK(straight[0].op == Op::Mov);
  CHECK(straight[0].args[1].kind == OperandKind::Instant);
  CHECK(straight[0].args[1].value == 0);
  CHECK(straight[1].op == Op::Ret);
}

TEST_CASE("a cast converts a value's type, wrapping into the target") {
  // A constant folds through the same wrap the machine applies, so `12888 as i8`
  // is an instant of 88 (12888 = 0x3258) and no CAST reaches the IR.
  const Program folded = parse_ok("fn main(): i32 { let x: i8 = 12888 as i8; return 0; }");
  const auto straight = commands_of(folded, 0);
  REQUIRE(straight[0].op == Op::Mov);
  CHECK(straight[0].args[1].type == Type::I8);
  CHECK(straight[0].args[1].value == 88);

  // A runtime operand keeps the conversion: CAST reads the source in its own
  // type and lands the low bits in the target's.
  const Program live = parse_ok("fn main(): i32 { let a: i32 = 300; let b: i8 = a as i8; return 0; }");
  CHECK(count_of(ops_of(live, 0), Op::Cast) == 1);

  // A cast to bool is the nonzero compare: bool is an i8 in the IR, and a CAST
  // into i8 could not tell `as bool` from `as i8`.
  const Program flag = parse_ok("fn main(): i32 { let a: i32 = 1; let b: bool = a as bool; return 0; }");
  CHECK(has_op(ops_of(flag, 0), Op::CmpNe));
  CHECK(!has_op(ops_of(flag, 0), Op::Cast));

  // bool and i8 are one cell in the IR, so `as i8` on a bool only rewrites the
  // source type: there is nothing to convert.
  const Program retype = parse_ok("fn main(): i32 { let b: bool = true; let x: i8 = b as i8; return 0; }");
  CHECK(!has_op(ops_of(retype, 0), Op::Cast));

  // `as` binds tighter than the binary operators: the operand of `+` is the
  // cast, so `x + 1 as i32` is `x + (1 as i32)` — an i8 plus an i32, which the
  // same-type rule rejects — while `(x + 1) as i32` says it out loud and is fine.
  CHECK(parse_error("fn main(): i32 { let x: i8 = 1; let y: i32 = x + 1 as i32; return 0; }") != "");
  parse_ok("fn main(): i32 { let x: i8 = 1; let y: i32 = (x + 1) as i32; return 0; }");
  // ... and looser than unary, so `-5 as u8` is `(-5) as u8` — which parses —
  // and not `-(5 as u8)`, which has no `-`.
  parse_ok("fn main(): i32 { let x: u8 = -5 as u8; return 0; }");
}

TEST_CASE("a call names its callee and checks every argument") {
  const std::string source = "fn f(a: i32, b: i32): i32 { return a; } fn main(): i32 { return f(1, 2); }";
  const Program program = parse_ok(source);
  const Cfg& cfg = body_of(program, 1);
  const Command call = cfg.blocks[0].commands[0];
  REQUIRE(call.op == Op::Call);
  // `args[0]` is the result slot, `args[1]` the callee, then one per argument.
  REQUIRE(call.args.size() == 4);
  CHECK(call.args[0].kind == OperandKind::VReg);
  CHECK(call.args[1].kind == OperandKind::Fn);
  CHECK(call.args[1].value == 0);
  CHECK(call.args[2].value == 1);
  CHECK(call.args[3].value == 2);

  // Both directions of a wrong count are the same rule, so they read the same.
  const std::string too_many = "'f' takes 1 arguments, but 2 were given";
  const std::string too_few = "'f' takes 2 arguments, but 1 were given";
  CHECK(parse_error("fn f(a: i32): i32 { return a; } fn main(): i32 { return f(1, 2); }") == too_many);
  CHECK(parse_error("fn f(a: i32, b: i32): i32 { return a; } fn main(): i32 { return f(1); }") == too_few);
  // An argument that is a literal still has to fit the parameter's type.
  CHECK(parse_error("fn f(a: u8): i32 { return 0; } fn main(): i32 { return f(300); }") != "");
  parse_ok("fn f(a: u8): i32 { return 0; } fn main(): i32 { return f(200); }");
  CHECK(parse_error("fn main(): i32 { return nope(1); }") == "call to undeclared function 'nope'");
}

TEST_CASE("a void call discards its result") {
  const Program program = parse_ok("fn log(x: i32) { let y: i32 = x; } fn main(): i32 { log(1); return 0; }");
  const Command call = body_of(program, 1).blocks[0].commands[0];
  REQUIRE(call.op == Op::Call);
  // The result slot is the `EMPTY` sentinel, and it is the only place one is
  // legal.
  CHECK(call.args[0].is_empty());
  CHECK(call.args[0].type == Type::Void);
  CHECK(program.functions[0].return_type == Type::Void);
  // A void function that falls off the end returns nothing.
  const Command last = body_of(program, 0).blocks[0].commands.back();
  CHECK(is_ret(last));
  CHECK(last.args.empty());
}

TEST_CASE("a call picks the declaration its argument types match") {
  // Declaration order is the id order, so `f(i32)` is function 0, `f(i64)` is
  // function 1, and `main` — the third item — is function 2. The callee operand
  // in `main`'s CALL says which one the arguments picked.
  const auto callee_of = [](const Program& program, std::size_t id) {
    const auto commands = commands_of(program, id);
    const auto call = std::find_if(commands.begin(), commands.end(), [](const Command& command) {
      return command.op == Op::Call;
    });
    REQUIRE(call != commands.end());
    return call->args[1].value;
  };

  // A lone literal argument is an i32 before any candidate is chosen, so the
  // i32 overload takes it...
  const Program literal =
      parse_ok("fn f(x: i32): i32 { return 1; } fn f(x: i64): i32 { return 2; } fn main(): i32 { return f(5); }");
  CHECK(callee_of(literal, 2) == 0);
  // ... a typed local moves the match ...
  const Program typed = parse_ok("fn f(x: i32): i32 { return 1; } fn f(x: i64): i32 { return 2; }"
                                 " fn main(): i32 { let n: i64 = 5; return f(n); }");
  CHECK(callee_of(typed, 2) == 1);
  // ... and so does a suffix, which is part of the argument's type.
  const Program suffixed = parse_ok("fn f(x: i32): i32 { return 1; } fn f(x: i64): i32 { return 2; }"
                                    " fn main(): i32 { return f(5i64); }");
  CHECK(callee_of(suffixed, 2) == 1);
  // Zero arguments are a signature like any other.
  const Program zero = parse_ok("fn f(): i32 { return 1; } fn f(x: i32): i32 { return 2; }"
                                " fn main(): i32 { return f(); }");
  CHECK(callee_of(zero, 2) == 0);
}

TEST_CASE("a call deduces a template's type arguments") {
  // Each distinct type set is one instantiation, a real function of its own
  // that the CALL names. Source functions keep ids `0..N-1`; instantiations
  // follow in the order the calls first asked for them.
  const Program program = parse_ok("fn twice<T extends Arithmetic>(x: T): T { return x + x; }"
                                   " fn main(): i32 { let a: i32 = twice(21); let b: i32 = twice(2); return a + b; }");
  REQUIRE(program.functions.size() == 2);  // main, plus the one i32 instance
  CHECK(program.functions[1].params == std::vector<Type> {Type::I32});
  CHECK(program.functions[1].return_type == Type::I32);
  const auto calls = commands_of(program, 0);
  const auto call = std::find_if(calls.begin(), calls.end(), [](const Command& command) {
    return command.op == Op::Call;
  });
  REQUIRE(call != calls.end());
  CHECK(call->args[1].value == 1);

  // A different type set is a different function, in request order — main's
  // first CALL goes to the i32 instance, its second to the u8 one.
  const Program two =
      parse_ok("fn twice<T extends Arithmetic>(x: T): T { return x + x; }"
               " fn main(): i32 { let a: i32 = twice(21); let b: u8 = twice(3u8); return a + b as i32; }");
  REQUIRE(two.functions.size() == 3);
  CHECK(two.functions[1].params == std::vector<Type> {Type::I32});
  CHECK(two.functions[2].params == std::vector<Type> {Type::U8});
  std::vector<std::uint64_t> callees;
  for (const Command& command : commands_of(two, 0)) {
    if (command.op == Op::Call)
      callees.push_back(command.args[1].value);
  }
  REQUIRE(callees.size() == 2);
  CHECK(callees[0] == 1);
  CHECK(callees[1] == 2);
}

TEST_CASE("a template may call itself, as the one instantiation") {
  // `fib(n - 1)` and `fib(n - 2)` deduce the same type as the body's own
  // parameter, so the recursion is an ordinary call to the same function.
  const Program program = parse_ok("fn fib<T extends Ordered & Arithmetic>(n: T): T {"
                                   " if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }"
                                   " fn main(): i32 { return fib(6); }");
  REQUIRE(program.functions.size() == 2);
  CHECK(program.functions[1].params == std::vector<Type> {Type::I32});
  CHECK(program.functions[1].return_type == Type::I32);
}

TEST_CASE("a template body reads, writes, and casts the deduced type") {
  const Program program = parse_ok("fn wrap<T extends Arithmetic>(x: T): T { let y: T = x + x; return y; }"
                                   " fn main(): i32 { return wrap(21); }");
  REQUIRE(program.functions.size() == 2);
  CHECK(program.functions[1].return_type == Type::I32);
  CHECK(has_op(ops_of(program, 1), Op::Add));

  // `as T` and `as` to a concrete type in the same body both resolve: the two
  // calls below deduce u32 and i32, each narrowed through the same `as u8`.
  const Program casted = parse_ok("fn low<T extends Ordered & Arithmetic>(x: T): u8 { let y: u8 = x as u8; return y; }"
                                  " fn main(): i32 { return (low(300u32) + low(7)) as i32; }");
  REQUIRE(casted.functions.size() == 3);
  CHECK(casted.functions[1].params == std::vector<Type> {Type::U32});
  CHECK(casted.functions[2].params == std::vector<Type> {Type::I32});
}

TEST_CASE("an exact concrete declaration beats a template of the same name") {
  const Program program = parse_ok("fn pick<T extends Ordered & Arithmetic>(x: T): i32 { return 1; }"
                                   " fn pick(x: char): i32 { return 2; }"
                                   " fn main(): i32 { let a: i32 = pick('x'); let b: i32 = pick(5); return a + b; }");
  // Concrete pick(char) is function 0, main 1, the i32 instantiation 2.
  REQUIRE(program.functions.size() == 3);
  std::vector<std::uint64_t> callees;
  for (const Command& command : commands_of(program, 1)) {
    if (command.op == Op::Call)
      callees.push_back(command.args[1].value);
  }
  REQUIRE(callees.size() == 2);
  CHECK(callees[0] == 0);  // 'x' resolves concretely
  CHECK(callees[1] == 2);  // 5 deduces the template
}

TEST_CASE("a template call checks its bounds after deduction") {
  // abs(12u8): T deduces to u8, which negation is not defined on.
  CHECK(parse_error("fn abs<T extends Ordered & Negatable>(x: T): T { return x; }"
                    " fn main(): i32 { abs(12u8); return 0; }") ==
        "u8 does not satisfy Negatable (required by 'T' of 'abs')");
  // min(1, 2u64): the two arguments bind T twice, disagreeing.
  CHECK(parse_error("fn min<T extends Ordered>(a: T, b: T): T { return a; }"
                    " fn main(): i32 { min(1, 2u64); return 0; }") ==
        "cannot deduce 'T' of 'min': the arguments give i32 and u64");
  // A template's argument count is exact, like a concrete call's.
  CHECK(parse_error("fn f<T extends Arithmetic>(a: T): T { return a; }"
                    " fn main(): i32 { f(1, 2); return 0; }") == "'f' takes 1 arguments, but 2 were given");
  // A single concrete parameter is exact too, with the ordinary argument error.
  CHECK(parse_error("fn f<T extends Ordered & Arithmetic>(x: i32, y: T): T { return y; }"
                    " fn main(): i32 { f(true, 1); return 0; }") == "argument 1 of 'f' is i32, not bool");
  CHECK(parse_error("fn f<T extends Ordered & Arithmetic>(x: i32, y: T): T { return y; }"
                    " fn main(): i32 { f(1i64, 2); return 0; }") == "argument 1 of 'f' is i32, not i64");
}

TEST_CASE("a template's declaration rules are enforced") {
  // The entry point names one concrete `fn main(): i32`.
  CHECK(parse_error("fn main<T>(x: T) { }") == "the entry point must be 'fn main(): i32'");
  // One template per name.
  CHECK(parse_error("fn f<T>(x: T) { } fn f<U>(x: U) { } fn main(): i32 { return 0; }") == "'f' is already declared");
  // A type parameter is declared once, with bounds that exist.
  CHECK(parse_error("fn f<T, T>(a: T) { } fn main(): i32 { return 0; }") == "type parameter 'T' is declared twice");
  CHECK(parse_error("fn f<T extends Foo>(a: T) { } fn main(): i32 { return 0; }") == "'Foo' is not a capability");
  // Deduction needs a foothold: every type parameter must appear in a
  // parameter's type.
  CHECK(parse_error("fn make<T>(x: i32): i32 { return x; } fn main(): i32 { return 0; }") ==
        "type parameter 'T' of 'make' is not in the parameters");
  // A template is a name plus a type list, not anything the expression
  // grammar accepts: explicit `f<u8>(…)` is not v1.
  CHECK(parse_error("fn f<T extends Ordered & Arithmetic>(x: T): T { return x; }"
                    " fn main(): i32 { f<u8>(1); return 0; }") != "");
}

TEST_CASE("a template's type list only closes on well-formed bounds") {
  // The grammar around `extends` and `>` rejects each broken shape with its own
  // message, before any deduction is involved.
  CHECK(parse_error("fn f<T extends & Ordered>(x: T): T { return x; } fn main(): i32 { return 0; }") ==
        "expected a capability after 'extends'");
  CHECK(parse_error("fn f<T extends Ordered & Ordered>(x: T): T { return x; } fn main(): i32 { return 0; }") ==
        "capability 'Ordered' is listed twice");
  CHECK(parse_error("fn f< & Ordered>(x: i32): i32 { return x; } fn main(): i32 { return 0; }") ==
        "expected an identifier");
  // Without a comma or `>`, the next token cannot open the parameter list.
  CHECK(parse_error("fn f<T extends Ordered(x: T): T { return x; } fn main(): i32 { return 0; }") == "expected '>'");
  // The template's type list does not excuse the parameter list `)`.
  CHECK(parse_error("fn f<T extends Ordered>(x: T { return x; } fn main(): i32 { return 0; }") == "expected ')'");
}

TEST_CASE("a template body is checked once per instantiation, not before") {
  // An empty body's missing return is an instantiation error, not a
  // declaration error: the declaration parses, and the call's deduction names
  // it before the body is checked.
  CHECK(parse_error("fn f<T extends Ordered>(x: T): T { } fn main(): i32 { f(3); return 0; }") ==
        "control reaches the end of 'f', which returns i32");
  // What is valid in the abstract can fail against a concrete type — `x - 1i32`
  // narrows the literal to a `u8` that does not fit — and the error surfaces
  // when the instantiation is lowered, in a later loop of the same pass.
  CHECK(parse_error(
            "fn sub<T extends Arithmetic>(x: T): T { return x - 1i32; } fn main(): i32 { sub(3u8); return 0; }") ==
        "'1i32' does not fit u8");
}

TEST_CASE("primitive is a call to the catalog of the same name") {
  // One statement lowers to exactly one `PRIM print`: an `EMPTY` result slot,
  // then the printed values in order. The id is the catalog's (`primitive.md`).
  const Program program = parse_ok("fn main(): i32 { primitive print(1, 2, 3); return 0; }");
  const std::vector<Command> commands = commands_of(program, 0);
  REQUIRE(commands.size() == 2);
  const Command print = commands[0];
  REQUIRE(print.op == Op::Primitive);
  CHECK(print.prim == 0);
  REQUIRE(print.args.size() == 4);
  CHECK(print.args[0].is_empty());
  CHECK(print.args[1].kind == OperandKind::Instant);
  CHECK(print.args[1].value == 1);
  CHECK(print.args[3].value == 3);
  CHECK(is_ret(commands[1]));

  // No arguments is fine: `primitive print()` is just the newline.
  parse_ok("fn main(): i32 { primitive print(); return 0; }");

  // Any value type prints, and nothing expects a type back out of it.
  parse_ok("fn g(): u8 { return 1; } fn main(): i32 { primitive print(g(), true, 'x'); return 0; }");
  parse_ok("let g: i32 = 5; fn main(): i32 { primitive print(g); return 0; }");
}

TEST_CASE("print is an ordinary name, now that the primitive keyword owns printing") {
  // The name-rule is gone with the builtin: a function, a parameter, a global,
  // and a local may all be called print.
  parse_ok("fn print(): i32 { return 0; } fn main(): i32 { return print(); }");
  parse_ok("fn f(print: i32): i32 { return print; } fn main(): i32 { return f(1); }");
  parse_ok("let print: i32 = 1; fn main(): i32 { return print; }");
  parse_ok("fn main(): i32 { let print: i32 = 1; return print; }");
  // A bare print is not a call of anything: it is now just an undeclared name.
  CHECK(parse_error("fn main(): i32 { print(1); return 0; }") == "call to undeclared function 'print'");
}

TEST_CASE("primitive rejects a call nothing can make") {
  // A call that returns nothing has no value to pass, a constant expression
  // cannot call anything, and an unknown name is not a primitive.
  CHECK(parse_error("fn f() { } fn main(): i32 { primitive print(f()); return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive print(1; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let x: i32 = primitive print(1); return x; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive nope(1); return 0; }") == "no primitive named 'nope'");
  // The fixed parameters are exact: exit takes one i32, abort none.
  CHECK(parse_error("fn main(): i32 { primitive exit(); return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive exit(1, 2); return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive abort(1); return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let b: bool = true; primitive exit(b); return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive exit(true); return 0; }") != "");
  // The keyword only ever introduces a call: a name must follow, then '('.
  CHECK(parse_error("fn main(): i32 { primitive; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { primitive print; return 0; }") != "");
  parse_ok("fn g(): bool { primitive print(true); return true; } fn main(): i32 { return 0; }");
  // exit and abort end their block, so flow stops where they are written,
  // the way `return` does: no return is needed after them, and code after
  // them is parsed but dropped as unreachable.
  parse_ok("fn main(): i32 { primitive exit(0); }");
  parse_ok("fn main(): i32 { if (true) { primitive abort(); } return 0; }");
  {
    const Program program = parse_ok("fn main(): i32 { primitive exit(0); let x: i32 = 2; }");
    const Cfg& cfg = body_of(program, 0);
    CHECK(cfg.size() == 1);
    REQUIRE(cfg.blocks[0].commands.size() == 1);
    CHECK(cfg.blocks[0].commands[0].op == Op::Primitive);
    CHECK(cfg.blocks[0].commands[0].prim == 3);
  }
}

TEST_CASE("an assignment is a statement, not an expression") {
  parse_ok("fn main(): i32 { let x: i32 = 1; x = x + 1; return x; }");
  parse_ok("fn main(): i32 { let x: i32 = 1; x += 1; return x; }");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let y: i32 = (x = 2); return y; }") != "");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x = x = 2; return x; }") != "");
  // An lvalue is a name: a literal or a call result is not one.
  CHECK(parse_error("fn main(): i32 { 1 = 2; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { f() = 2; return 0; }") == "call to undeclared function 'f'");
}

TEST_CASE("a read must follow a write on every path") {
  parse_ok("fn main(): i32 { let x: i32 = 1; x = 2; return x; }");
  CHECK(parse_error("fn main(): i32 { let x: i32; return x; }") == "'x' is read before it is assigned");
  // A test the parser cannot fold is two paths, and one of them skips the write.
  CHECK(parse_error(
            "fn f(b: bool): i32 { let x: i32; if (b) { x = 1; } return x; } fn main(): i32 { return f(true); }") ==
        "'x' is read before it is assigned");
  // Both arms are.
  parse_ok("fn f(b: bool): i32 { let x: i32; if (b) { x = 1; } else { x = 2; } return x; } fn main(): i32 { return "
           "f(true); }");
  // A test that folds to `true` leaves one path, and the write is on it.
  parse_ok("fn main(): i32 { let x: i32; if (1 < 2) { x = 1; } return x; }");
  // A loop may run zero times, so its body does not count as a write.
  CHECK(parse_error(
            "fn f(n: i32): i32 { let x: i32; while (n > 0) { x = 1; } return x; } fn main(): i32 { return f(1); }") ==
        "'x' is read before it is assigned");
  // A `break` in the same loop is a path that skips the write.
  CHECK(parse_error("fn f(n: i32): i32 { let x: i32; while (n > 0) { if (n > 1) { break; } x = 1; } return x; } fn "
                    "main(): i32 { return f(1); }") != "");
  // A parameter and a global always have a value.
  parse_ok("fn f(a: i32): i32 { return a; } fn main(): i32 { return f(1); }");
  parse_ok("let g: i32 = 1; fn main(): i32 { return g; }");
}

TEST_CASE("a for's init scope ends with the loop") {
  parse_ok("fn main(): i32 { for (let j: i32 = 0; j < 2; j = j + 1) { } return 0; }");
  CHECK(parse_error("fn main(): i32 { for (let j: i32 = 0; j < 2; j = j + 1) { } return j; }") ==
        "undeclared identifier 'j'");
  // A name in an enclosing scope is still visible inside the loop.
  parse_ok("fn main(): i32 { let j: i32 = 0; for (let k: i32 = 0; k < 2; k = k + 1) { j = k; } return j; }");
}

TEST_CASE("an inner name shadows an outer one") {
  parse_ok("fn main(): i32 { let x: i32 = 1; { let x: i32 = 2; } return x; }");
  // ... but not in the same scope.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; let x: i32 = 2; return x; }") ==
        "'x' is already declared in this scope");
}

TEST_CASE("declaration order is free for functions and globals in bodies") {
  parse_ok("fn main(): i32 { return f(1); } fn f(x: i32): i32 { return g(x); } fn g(x: i32): i32 { return x; }");
  // A global is visible in a function declared above it.
  parse_ok("fn main(): i32 { return later; } let later: i32 = 1;");
  // ... but a global initializer reads only what came before it.
  CHECK(parse_error("let a: i32 = b; let b: i32 = 1; fn main(): i32 { return a; }") ==
        "'b' is declared after this initializer");
  // A global's initializer is a constant expression.
  CHECK(parse_error("let a: i32 = f(); fn f(): i32 { return 1; } fn main(): i32 { return a; }") != "");
}

TEST_CASE("main is the entry point, by name") {
  CHECK(parse_error("fn f(): i32 { return 0; }") == "the program must declare 'fn main(): i32'");
  // The rule is on the whole signature, not just the name.
  CHECK(parse_error("fn main(x: i32): i32 { return 0; }") == "the entry point must be 'fn main(): i32'");
  CHECK(parse_error("fn main(): i8 { return 0; }") == "the entry point must be 'fn main(): i32'");
  CHECK(parse_error("fn main(): i32 { return 0; } fn main(): i32 { return 1; }") == "'main' is already declared");
  // The entrypoint is the index of `main`, not the first function.
  const Program program = parse_ok("fn f(): i32 { return 0; } fn main(): i32 { return f(); }");
  CHECK(program.entrypoint == 1);
}

TEST_CASE("a non-void function returns on every path") {
  parse_ok("fn f(b: bool): i32 { if (b) { return 1; } return 0; } fn main(): i32 { return f(true); }");
  CHECK(parse_error("fn f(): i32 { let x: i32 = 1; } fn main(): i32 { return f(); }") ==
        "control reaches the end of 'f', which returns i32");
  // One arm of an `if` the parser cannot fold is not every path.
  CHECK(parse_error("fn f(b: bool): i32 { if (b) { return 1; } } fn main(): i32 { return f(true); }") ==
        "control reaches the end of 'f', which returns i32");
  // A test that folds to `true` does return on every path.
  parse_ok("fn f(): i32 { if (1 < 2) { return 1; } } fn main(): i32 { return f(); }");
  // A void function may fall off the end, and may not return a value.
  parse_ok("fn f() { } fn main(): i32 { return 0; }");
  CHECK(parse_error("fn f(): i32 { return 1; } fn g() { return 1; } fn main(): i32 { return 0; }") ==
        "'g' returns nothing, so it cannot return a value");
}

TEST_CASE("a test that folds keeps one edge") {
  // `1 < 2` is true, so the `else` is dropped rather than kept as dead code.
  const Program program =
      parse_ok("fn main(): i32 { let x: i32 = 1; if (1 < 2) { x = 2; } else { x = 3; } return x; }");
  const Cfg& cfg = body_of(program, 0);
  for (const BasicBlock& block : cfg.blocks)
    CHECK(block.edge_count() <= 1);
  // The whole body is straight-line: entry, then, return.
  CHECK(cfg.size() == 3);
}

TEST_CASE("a loop tests in a block of its own, so its prefix runs once") {
  // The block a `while` is written in is not the block the loop comes back to: a
  // back edge into it would run `let i = 0` again on every iteration, and
  // `i = i + 1` would never end.
  const Program program = parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 3) { i = i + 1; } return i; }");
  const Cfg& cfg = body_of(program, 0);
  const std::size_t header = first_block_with(program, 0, Op::CmpLt);
  REQUIRE(header != cfg.size());
  CHECK(header != 0);
  // The entry block only sets `i` up and hands over; the test is in the header.
  REQUIRE(cfg.blocks[0].commands.size() == 1);
  CHECK(cfg.blocks[0].commands[0].op == Op::Mov);
  CHECK(cfg.blocks[0].commands[0].args[0].value == 0);
  CHECK(cfg.blocks[0].next == header);
  // The body links back to the header, not to the entry.
  const std::size_t body = first_block_with(program, 0, Op::Add);
  REQUIRE(body != cfg.size());
  CHECK(cfg.blocks[body].next == header);
  // A `while` as a whole function body has an empty block in front of the header,
  // because the header is a block of its own all the same.
  const Program bare = parse_ok("fn f(n: i32) { while (n > 0) { n = n - 1; } } fn main(): i32 { return 0; }");
  const Cfg& alone = body_of(bare, 0);
  const std::size_t alone_header = first_block_with(bare, 0, Op::CmpGt);
  const std::size_t alone_body = first_block_with(bare, 0, Op::Sub);
  REQUIRE(alone_header != alone.size());
  REQUIRE(alone_body != alone.size());
  CHECK(alone_header == 1);
  CHECK(cfg.size() == 4);
  CHECK(alone.blocks[0].commands.empty());
  CHECK(alone.blocks[0].next == alone_header);
  CHECK(alone.blocks[alone_body].next == alone_header);
}

TEST_CASE("unreachable code is dropped, not rejected") {
  // The code after a `return` is still parsed and checked, but nothing can
  // reach it, so it is not in the CFG.
  const Program program = parse_ok("fn main(): i32 { return 1; let x: i32 = 2; return x; }");
  const Cfg& cfg = body_of(program, 0);
  CHECK(cfg.size() == 1);
  REQUIRE(cfg.blocks[0].commands.size() == 1);
  CHECK(is_ret(cfg.blocks[0].commands[0]));
  CHECK(cfg.blocks[0].commands[0].args[0].value == 1);
  // A body whose arm never falls through closes the other one off too.
  const Program looped = parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 2) { return 1; } return 0; }");
  CHECK(every_block_ends(body_of(looped, 0)));
}

TEST_CASE("break and continue are loop-only") {
  parse_ok(
      "fn main(): i32 { let i: i32 = 0; while (i < 2) { i = i + 1; if (i == 1) { continue; } break; } return i; }");
  parse_ok("fn main(): i32 { for (let j: i32 = 0; j < 2; j = j + 1) { break; } return 0; }");
  parse_ok("fn f(n: i32): i32 { while (n > 0) { continue; } return n; } fn main(): i32 { return f(1); }");
  CHECK(parse_error("fn main(): i32 { break; return 0; }") == "'break' is only allowed in a loop");
  CHECK(parse_error("fn main(): i32 { continue; return 0; }") == "'continue' is only allowed in a loop");
  // A loop in another function does not open one here.
  CHECK(
      parse_error("fn f(n: i32): i32 { while (n > 0) { return 1; } return n; } fn main(): i32 { break; return 0; }") ==
      "'break' is only allowed in a loop");
  // A nested block inside a loop is still inside it.
  parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 2) { { break; } } return i; }");
}

TEST_CASE("a block is a statement and a scope") {
  parse_ok("fn main(): i32 { { let x: i32 = 1; } return 0; }");
  // The inner name is gone with the block.
  CHECK(parse_error("fn main(): i32 { { let x: i32 = 1; } return x; }") == "undeclared identifier 'x'");
}

TEST_CASE("a do body always runs, so what it assigns survives the loop") {
  parse_ok("fn f(n: i32): i32 { let x: i32; do { x = 1; } while (n > 0); return x; } fn main(): i32 { return f(1); }");
  // ... unless the only path that assigns it is one a `break` skips.
  CHECK(parse_error("fn f(n: i32): i32 { let x: i32; do { if (n > 0) { break; } x = 1; } while (n < 0); return x; } "
                    "fn main(): i32 { return f(1); }") == "'x' is read before it is assigned");
  // A `for` init always runs, so an assignment there counts.
  parse_ok("fn f(n: i32): i32 { let x: i32; for (x = 1; n > 0; n = n - 1) { } return x; } fn main(): i32 { return "
           "f(1); }");
  // ... but the body of a loop that may run zero times does not.
  CHECK(parse_error("fn f(n: i32): i32 { let x: i32; for (; n > 0; n = n - 1) { x = 1; } return x; } fn main(): i32 "
                    "{ return f(1); }") == "'x' is read before it is assigned");
}

TEST_CASE("a loop runs its step after its body") {
  // The step is written before the body but runs after it, so the block the
  // step lands in is the one the body and `continue` fall into.
  const Program program = parse_ok("fn main(): i32 { for (let j: i32 = 0; j < 2; j = j + 1) { } return 0; }");
  const Cfg& cfg = body_of(program, 0);
  bool saw_step = false;
  for (const BasicBlock& block : cfg.blocks) {
    for (const Command& command : block.commands) {
      if (command.op == Op::Add) {
        saw_step = true;
        CHECK(block.edge_count() == 1);
      }
    }
  }
  CHECK(saw_step);
  CHECK(every_block_ends(cfg));
}

TEST_CASE("an unknown operator is reported where it is written") {
  // The tokenizer keeps `++` as one maximal-munch run; the whitelist is what
  // rejects it, with a location of its own.
  const std::string message = parse_error("fn main(): i32 { let x: i32 = 1 ++ 2; return x; }");
  CHECK(message == "unknown operator '++'");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x ** 2; }") == "unknown operator '**'");
}

TEST_CASE("reserved words are not names") {
  CHECK(parse_error("fn main(): i32 { let let: i32 = 1; return 0; }") == "'let' is a reserved word");
  CHECK(parse_error("fn main(): i32 { return while; }") == "'while' is a reserved word");
  // A type name is a reserved word too, so it cannot name a variable.
  CHECK(parse_error("fn main(): i32 { let i32: i32 = 1; return 0; }") == "'i32' is a reserved word");
  // ... or a function, or a global.
  CHECK(parse_error("fn fn(): i32 { return 0; }") == "'fn' is a reserved word");
  CHECK(parse_error("let bool: i32 = 1; fn main(): i32 { return 0; }") == "'bool' is a reserved word");
  // `extends` is reserved for a type parameter's bounds.
  CHECK(parse_error("fn main(): i32 { let extends: i32 = 1; return 0; }") == "'extends' is a reserved word");
  CHECK(parse_error("fn extends(): i32 { return 0; }") == "'extends' is a reserved word");
}

TEST_CASE("float and string are not types yet") {
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5; return x; }") == "float is not supported yet");
  CHECK(parse_error("fn main(): i32 { let x: string = \"hi\"; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let x: i32 = \"hi\"; return x; }") == "string is not supported yet");
}

TEST_CASE("the tokenizer's contract still holds at the parser's edge") {
  // `::` lexes as two colons, so a namespace is a parse error rather than a
  // name the parser has to understand.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x::y; }") != "");
  // A dot is not an operator: `1..2` is not a range.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1..2; return x; }") != "");
  // `?.` composes at parse and has nothing to compose with in v1.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1; return x?.y; }") == "unknown operator '?'");
}
