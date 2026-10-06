#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ParserTest.hpp"

using namespace erax::frontend;
using namespace erax::middleend;
using namespace erax::test;

/// Globals, assignments, and the statement forms that build the CFG. A global is
/// the one cell a command cannot compute into, so it takes the long way round.

TEST_CASE("a global is read, written, and written in place")
{
    // Reading a global is an operand, not a cell copy.
    const Program read = parse_ok("let g: i32 = 1; fn main(): i32 { return g; }");
    const Command ret = commands_of(read, 0).back();
    REQUIRE(is_ret(ret));
    REQUIRE(ret.args.size() == 1);
    CHECK(ret.args[0].kind == OperandKind::Global);
    CHECK(ret.args[0].value == 0);

    // Assigning one writes the cell directly.
    const Program written = parse_ok("let g: i32 = 1; fn main(): i32 { g = 2; return g; }");
    const std::vector<Command> writes = commands_of(written, 0);
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].op == Op::Mov);
    CHECK(writes[0].args[0].kind == OperandKind::Global);
    CHECK(writes[0].args[1].kind == OperandKind::Instant);
    CHECK(writes[0].args[1].value == 2);

    // A compound assignment has no command that computes into a global, so it
    // goes through a frame cell and writes the global back.
    const Program compound = parse_ok("let g: i32 = 1; fn main(): i32 { g += 2; return g; }");
    const std::vector<Command> steps = commands_of(compound, 0);
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].op == Op::Add);
    CHECK(steps[0].args[0].kind == OperandKind::VReg);
    CHECK(steps[0].args[1].kind == OperandKind::Global);
    CHECK(steps[1].op == Op::Mov);
    CHECK(steps[1].args[0].kind == OperandKind::Global);
    CHECK(steps[1].args[1].kind == OperandKind::VReg);
    // The same trick for every compound form, and for a `char` global.
    for (const std::string_view op : { "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=" }) {
        const Program program = parse_ok("let g: i32 = 4; fn main(): i32 { g " + std::string { op } + " 2; return g; }");
        REQUIRE(commands_of(program, 0).size() == 3);
        CHECK(commands_of(program, 0)[0].args[1].kind == OperandKind::Global);
    }
    parse_ok("let g: char = 'A'; fn main(): i32 { g += 1; return 0; }");
    // A compound assignment on a local computes into the local's own cell.
    const Program local = parse_ok("fn main(): i32 { let x: i32 = 1; x += 2; return x; }");
    REQUIRE(commands_of(local, 0).size() == 3);
    CHECK(commands_of(local, 0)[1].op == Op::Add);
    CHECK(commands_of(local, 0)[1].args[0].kind == OperandKind::VReg);
    CHECK(commands_of(local, 0)[1].args[1].kind == OperandKind::VReg);
    CHECK(commands_of(local, 0)[1].args[0].value == commands_of(local, 0)[1].args[1].value);
}

TEST_CASE("an assignment target is a name of the right type")
{
    // A name that is not declared is not an lvalue.
    CHECK(parse_error("fn main(): i32 { nope = 1; return 0; }") == "undeclared identifier 'nope'");
    // The value has the type of the target, exactly.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x = true; return 0; }") == "cannot assign bool to i32");
    CHECK(parse_error("let g: i32 = 1; fn main(): i32 { g = 'A'; return 0; }") == "cannot assign char to i32");
    // A compound assignment needs an integer to do arithmetic on.
    CHECK(parse_error("fn main(): i32 { let b: bool = true; b += 1; return 0; }") == "'+=' is not defined for bool");
    // A compound assignment takes the target's type too.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x += true; return 0; }") != "");
    // A name with no assignment operator after it is an expression statement, so
    // the statement is what says it wanted a `;`.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1; x 2; return 0; }") == "expected ';'");
    // In a `for` step, where nothing but an assignment is allowed, it is the
    // assignment that says so.
    CHECK(parse_error("fn main(): i32 { for (let i: i32 = 0; i < 2; i) { } return 0; }") == "expected an assignment operator");
    CHECK(parse_error("fn main(): i32 { for (let i: i32 = 0; i < 2; i + 1) { } return 0; }") == "expected an assignment operator");
}

TEST_CASE("a declaration states a name, a type, and a value")
{
    // Each part is required, and the error says which one is missing.
    CHECK(parse_error("fn main(): i32 { let x i32 = 1; return 0; }") == "expected ':'");
    CHECK(parse_error("fn main(): i32 { let x: i32 1; return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1 return 0; }") == "expected ';'");
    // A type is a type name and nothing else.
    CHECK(parse_error("fn main(): i32 { let x: 1 = 1; return 0; }") == "expected a type");
    CHECK(parse_error("fn main(): i32 { let 1: i32 = 1; return 0; }") == "expected an identifier");
    CHECK(parse_error("fn main(): i32 { let x: nope = 1; return 0; }") == "expected a type");
    // A name is a name.
    CHECK(parse_error("fn main(): i32 { let if: i32 = 1; return 0; }") == "'if' is a reserved word");
    // A global is declared the same way, and only once.
    CHECK(parse_error("let g i32 = 1; fn main(): i32 { return 0; }") == "expected ':'");
    CHECK(parse_error("let g: i32 1; fn main(): i32 { return 0; }") == "expected '='");
    CHECK(parse_error("let g: i32 = 1 fn main(): i32 { return 0; }") == "expected ';'");
    CHECK(parse_error("let g: nope = 1; fn main(): i32 { return 0; }") == "expected a type");
    CHECK(parse_error("let 1: i32 = 1; fn main(): i32 { return 0; }") == "expected an identifier");
    CHECK(parse_error("let g: i32 = 1; let g: i32 = 2; fn main(): i32 { return 0; }") == "'g' is already declared");
    // A function's parameter list follows the same rules.
    CHECK(parse_error("fn f(a i32): i32 { return a; } fn main(): i32 { return 0; }") == "expected ':'");
    CHECK(parse_error("fn f(a: i32,): i32 { return a; } fn main(): i32 { return 0; }") == "expected an identifier");
    CHECK(parse_error("fn f(a: ): i32 { return 0; } fn main(): i32 { return 0; }") == "expected a type");
    // A return type is a colon and a type; a body is a brace.
    CHECK(parse_error("fn f() i32 { return 0; } fn main(): i32 { return 0; }") == "expected '{'");
    CHECK(parse_error("fn f(): 1 { return 0; } fn main(): i32 { return 0; }") == "expected a type");
    CHECK(parse_error("fn f() : { return 0; } fn main(): i32 { return 0; }") == "expected a type");
    // A function is declared once, and `main` like any other -- so a second `main` is
    // a duplicate name, not a separate rule.
    CHECK(parse_error("fn f(): i32 { return 0; } fn f(): i32 { return 1; } fn main(): i32 { return 0; }") == "'f' is already declared");
    CHECK(parse_error("fn main(): i32 { return 0; } fn main(): i32 { return 1; }") == "'main' is already declared");
    // A function and a global share one namespace.
    CHECK(parse_error("fn f() { } let f: i32 = 1; fn main(): i32 { return 0; }") != "");
    CHECK(parse_error("let g: i32 = 1; fn g() { } fn main(): i32 { return 0; }") != "");
}

TEST_CASE("a top-level declaration is a global or a function")
{
    CHECK(parse_error("nonsense") == "expected a global or a function declaration");
    CHECK(parse_error("fn main(): i32 { return 0; } )") == "expected a global or a function declaration");
    CHECK(parse_error("fn main(): i32 { return 0; } ]") == "expected a global or a function declaration");
    CHECK(parse_error("fn main(): i32 { return 0; } let y: i32 = 1 }") == "expected ';'");
    // A `fn` with no name is not a declaration.
    CHECK(parse_error("fn") == "expected an identifier");
    CHECK(parse_error("fn 1(): i32 { return 0; } fn main(): i32 { return 0; }") == "expected an identifier");
    // A program with no entry point is rejected.
    CHECK(parse_error("fn f(): i32 { return 0; }") == "the program must declare 'fn main(): i32'");
    // An unterminated body is reported where it started.
    CHECK(parse_error("fn main(): i32 { return 0;") == "unterminated function body");
    CHECK(parse_error("fn main(): i32 { { return 0; }") == "unterminated function body");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1;") == "unterminated function body");
    CHECK(parse_error("fn main(): i32 { if (true) { return 0; } ") == "unterminated function body");
    CHECK(parse_error("fn main(): i32 { { let x: i32 = 1; return 0; }") == "unterminated function body");
}

TEST_CASE("a condition is a bool, in every construct that takes one")
{
    const char* programs[] = {
        "fn main(): i32 { let i: i32 = 0; while (i) { return 0; } return 0; }",
        "fn main(): i32 { let i: i32 = 0; do { return 0; } while (i); return 0; }",
        "fn main(): i32 { for (let i: i32 = 0; i; i = i + 1) { return 0; } return 0; }",
    };
    for (const char* source : programs) {
        CHECK(parse_error(source) == "condition must be 'bool', not i32");
    }
    // A literal condition is caught one step earlier, where it is read in the type of
    // the position it is in -- and a `bool` is not one a literal can fill.
    CHECK(parse_error("fn main(): i32 { if (1) { return 0; } return 0; }") == "expected a value of type bool, not an integer literal");
    // A `char` is not a `bool`, even though it is a byte.
    CHECK(parse_error("fn main(): i32 { let c: char = 'A'; if (c) { return 0; } return 0; }") == "condition must be 'bool', not char");
    // A comparison is a bool, so it is a condition.
    parse_ok("fn main(): i32 { if (1 < 2) { return 0; } return 0; }");
    // A `char` comparison is a bool too, and compares as bytes.
    parse_ok("fn main(): i32 { let c: char = 'A'; if (c < 'z') { return 0; } return 0; }");
    // A call that returns a bool is a condition.
    parse_ok("fn f(): bool { return true; } fn main(): i32 { if (f()) { return 0; } return 0; }");
    CHECK(parse_error("fn f(): i32 { return 1; } fn main(): i32 { if (f()) { return 0; } return 0; }") == "condition must be 'bool', not i32");
}

TEST_CASE("a loop's parts are each required")
{
    // A `while` needs its keyword, parens, test, and body.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; i = i + 1 while (i > 0) { } return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while i > 0) { } return 0; }") == "expected '('");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while (i > 0 { } return 0; }") == "expected ')'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while (i > 0) i = i + 1; return 0; }") == "expected '{'");
    // A `for` needs all three parts between parens and a body.
    CHECK(parse_error("fn main(): i32 { for (let i: i32 = 0; i < 2) { } return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { for (let i: i32 = 0; i < 2; i = i + 1) return 0; }") == "expected '{'");
    CHECK(parse_error("fn main(): i32 { for (let i: i32 = 0 i < 2; i = i + 1) { } return 0; }") == "expected ';'");
    // The init is a declaration or an assignment, and nothing else.
    CHECK(parse_error("fn main(): i32 { for (1; i32 < 2; i = i + 1) { } return 0; }") == "expected a declaration or an assignment in the 'for' init");
    CHECK(parse_error("fn main(): i32 { for (let i: i32; i < 2; i = i + 1) { } return 0; }") == "'i' is read before it is assigned");
    // A `do` needs its `while` and test.
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; do { i = i - 1; } i > 0; return 0; }") == "expected 'while'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; do { i = i - 1; } return 0; }") == "expected 'while'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; do { i = i - 1; } while; return 0; }") == "expected '('");
}

TEST_CASE("a break or continue ends its statement")
{
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while (i > 0) { break } return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { let i: i32 = 0; while (i > 0) { continue } return 0; }") == "expected ';'");
    // A loop that a `return` already closed is still parsed.
    const Program program = parse_ok("fn main(): i32 { let i: i32 = 0; while (i > 0) { return 1; } return 0; }");
    CHECK(every_block_ends(body_of(program, 0)));
    // A `break` after a `return` in the same body is unreachable, and dropped.
    const Program dropped = parse_ok("fn main(): i32 { let i: i32 = 0; while (i > 0) { break; return 1; } return 0; }");
    CHECK(every_block_ends(body_of(dropped, 0)));
}

TEST_CASE("a void function returns nothing, and says so")
{
    // An explicit `return;` is a return that emits the block's `Ret`.
    const Program program = parse_ok("fn f() { return; } fn main(): i32 { f(); return 0; }");
    const std::vector<Command> commands = commands_of(program, 0);
    REQUIRE(commands.size() == 1);
    CHECK(is_ret(commands[0]));
    CHECK(commands[0].args.empty());
    // A non-void function cannot fall out of a `return` with no value.
    CHECK(parse_error("fn f(): i32 { return; } fn main(): i32 { return 0; }") == "expected a value of type i32 to return");
    // ... and the value it does return has to be the return type.
    CHECK(parse_error("fn f(): i32 { return true; } fn main(): i32 { return 0; }") == "expected a value of type i32, not bool");
    // A missing semicolon is still a missing semicolon, and a `return` in a void
    // function with one is still a value.
    CHECK(parse_error("fn f() { return } fn main(): i32 { return 0; }") == "'f' returns nothing, so it cannot return a value");
    CHECK(parse_error("fn f(): i32 { return 1 } fn f2(): i32 { return 0; } fn main(): i32 { return 0; }") == "expected ';'");
    // A void call in the middle of a function is a statement of its own.
    const Program mixed = parse_ok("fn f(x: i32) { let y: i32 = x; } fn main(): i32 { f(1); return 0; }");
    CHECK(ops_of(mixed, 1).front() == Op::Call);
}

TEST_CASE("an else if is an else holding an if")
{
    const Program program = parse_ok("fn f(b: bool): i32 { if (b) { return 1; } else if (!b) { return 2; }"
                                     " else { return 3; } } fn main(): i32 { return f(true); }");
    // Three arms, so three returns, and every block still ends.
    CHECK(count_of(ops_of(program, 0), Op::Ret) == 3);
    CHECK(every_block_ends(body_of(program, 0)));
    // The arms are reached in order, so the first return sits behind the two tests.
    CHECK(first_block_with(program, 0, Op::Ret) == 2);
    // An `else if` chain that ends in an `else` still closes off.
    const Program chain = parse_ok("fn f(n: i32): i32 { if (n == 0) { return 1; } else if (n == 1) { return 2; }"
                                   " else if (n == 2) { return 3; } else { return 4; } } fn main(): i32 { return f(1); }");
    CHECK(count_of(ops_of(chain, 0), Op::Ret) == 4);
    CHECK(every_block_ends(body_of(chain, 0)));
    // An `if` with no `else` leaves a fallthrough edge.
    const Program plain = parse_ok("fn f(n: i32): i32 { if (n == 0) { return 1; } return 2; } fn main(): i32 { return f(1); }");
    CHECK(every_block_ends(body_of(plain, 0)));
    // An `else` with no `if` is not a statement.
    CHECK(parse_error("fn main(): i32 { else { return 0; } return 0; }") != "");
}

TEST_CASE("a folded test picks the edge the value says")
{
    // `while (1 > 2)` runs zero times, so its body is dropped rather than kept, and
    // the loop's test still has a block of its own to fold its one edge in.
    const Program loop = parse_ok("fn main(): i32 { let i: i32 = 0; while (1 > 2) { i = i + 1; } return i; }");
    CHECK(body_of(loop, 0).size() == 3);
    for (const BasicBlock& block : body_of(loop, 0).blocks) {
        for (const Command& command : block.commands)
            CHECK(command.op != Op::Add);
    }
    // A folded `do` test does not run the body a second time.
    const Program again = parse_ok("fn main(): i32 { let i: i32 = 0; do { i = i + 1; } while (1 > 2);"
                                   " return i; }");
    CHECK(every_block_ends(body_of(again, 0)));
    // A folded `for` test runs the init, and nothing else: the test is false the
    // first time, so the body and the step are both gone.
    const Program counted = parse_ok("fn main(): i32 { let i: i32 = 0; for (i = 1; 1 > 2; i = i + 1) { i = 9; }"
                                     " return i; }");
    CHECK(every_block_ends(body_of(counted, 0)));
    CHECK(count_of(ops_of(counted, 0), Op::Mov) == 2);
    // Every comparison folds, and a folded test is one edge.
    for (const char* test : { "1 <= 2", "1 > 2", "1 != 1", "1 == 1", "1 >= 2" }) {
        const Program program = parse_ok(std::string { "fn main(): i32 { let x: i32 = 1; if (" } + test + ") { x = 3; } else { x = 4; } return x; }");
        for (const BasicBlock& block : body_of(program, 0).blocks)
            CHECK(block.edge_count() <= 1);
    }
    // A folded `if` in a function decides which arm a later read sees.
    const Program intersection = parse_ok("fn f(): i32 { let x: i32; if (1 == 1) { x = 3; } else { x = 4; }"
                                          " return x; } fn main(): i32 { return f(); }");
    CHECK(commands_of(intersection, 0)[0].args[1].value == 3);
}
