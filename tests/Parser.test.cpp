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
    CHECK(f.params == std::vector<Type>{Type::I32, Type::U8});
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
    const Program program = parse_ok("let a: u8 = 1; let b: char = 'x'; let c: bool = true; let d: u64 = 1; fn main(): i32 { return 0; }");
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
    const std::string message = parse_error("fn main(): i32 { let a: i32 = 1; let b: u32 = 2; let c: i32 = a + b; return c; }");
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
    for (const Command& command : cfg.blocks[0].commands) ops.push_back(command.op);
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
    CHECK(parse_error("fn f(b: bool): i32 { let x: i32; if (b) { x = 1; } return x; } fn main(): i32 { return f(true); }") ==
          "'x' is read before it is assigned");
    // Both arms are.
    parse_ok("fn f(b: bool): i32 { let x: i32; if (b) { x = 1; } else { x = 2; } return x; } fn main(): i32 { return f(true); }");
    // A test that folds to `true` leaves one path, and the write is on it.
    parse_ok("fn main(): i32 { let x: i32; if (1 < 2) { x = 1; } return x; }");
    // A loop may run zero times, so its body does not count as a write.
    CHECK(parse_error("fn f(n: i32): i32 { let x: i32; while (n > 0) { x = 1; } return x; } fn main(): i32 { return f(1); }") ==
          "'x' is read before it is assigned");
    // A `break` in the same loop is a path that skips the write.
    CHECK(parse_error("fn f(n: i32): i32 { let x: i32; while (n > 0) { if (n > 1) { break; } x = 1; } return x; } fn main(): i32 { return f(1); }") != "");
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
    CHECK(parse_error("fn main(): i32 { return 0; } fn main(): i32 { return 1; }") ==
          "'main' is already declared");
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
    const Program program = parse_ok("fn main(): i32 { let x: i32 = 1; if (1 < 2) { x = 2; } else { x = 3; } return x; }");
    const Cfg& cfg = body_of(program, 0);
    for (const BasicBlock& block : cfg.blocks) CHECK(block.edge_count() <= 1);
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
    const Program looped =
        parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 2) { return 1; } return 0; }");
    CHECK(every_block_ends(body_of(looped, 0)));
}

TEST_CASE("break and continue are loop-only") {
    parse_ok("fn main(): i32 { let i: i32 = 0; while (i < 2) { i = i + 1; if (i == 1) { continue; } break; } return i; }");
    parse_ok("fn main(): i32 { for (let j: i32 = 0; j < 2; j = j + 1) { break; } return 0; }");
    parse_ok("fn f(n: i32): i32 { while (n > 0) { continue; } return n; } fn main(): i32 { return f(1); }");
    CHECK(parse_error("fn main(): i32 { break; return 0; }") == "'break' is only allowed in a loop");
    CHECK(parse_error("fn main(): i32 { continue; return 0; }") == "'continue' is only allowed in a loop");
    // A loop in another function does not open one here.
    CHECK(parse_error("fn f(n: i32): i32 { while (n > 0) { return 1; } return n; } fn main(): i32 { break; return 0; }") ==
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
    CHECK(parse_error("fn f(n: i32): i32 { let x: i32; do { if (n > 0) { break; } x = 1; } while (n < 0); return x; } fn main(): i32 { return f(1); }") ==
          "'x' is read before it is assigned");
    // A `for` init always runs, so an assignment there counts.
    parse_ok("fn f(n: i32): i32 { let x: i32; for (x = 1; n > 0; n = n - 1) { } return x; } fn main(): i32 { return f(1); }");
    // ... but the body of a loop that may run zero times does not.
    CHECK(parse_error("fn f(n: i32): i32 { let x: i32; for (; n > 0; n = n - 1) { x = 1; } return x; } fn main(): i32 { return f(1); }") ==
          "'x' is read before it is assigned");
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
