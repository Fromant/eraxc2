#include "interp/Interpreter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "middleend/jir/Jir.hpp"

using erax::frontend::ProgramParser;
using erax::frontend::Tokenizer;
using erax::interp::Broken;
using erax::interp::Cell;
using erax::interp::Limits;
using erax::interp::Outcome;
using erax::interp::Rejected;
using erax::interp::Trap;
using erax::interp::Trapped;
using erax::middleend::Branch;
using erax::middleend::Command;
using erax::middleend::Condition;
using erax::middleend::Function;
using erax::middleend::Global;
using erax::middleend::Op;
using erax::middleend::Operand;
using erax::middleend::OperandKind;
using erax::middleend::Program;
using erax::middleend::Subject;
using erax::middleend::Type;

namespace {

/// Lowers a source and runs it, the way the driver does, so a test reads like a
/// program and asks what it did rather than how it was built.
std::expected<Outcome, erax::interp::Failure> execute(std::string_view source, const Limits& limits = {})
{
    auto tokens = Tokenizer {}.tokenize(source, "test.erx");
    REQUIRE(tokens.has_value());
    auto program = ProgramParser { *tokens }.parse();
    REQUIRE(program.has_value());
    return erax::interp::run(*program, limits);
}

/// An `i32` cell read as the number it holds, which is what an exit code is: the
/// entrypoint returns `i32` by contract.
std::int64_t as_i32(Cell cell) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(cell));
}

/// The exit code of a program that finishes, so a test reads it as the source
/// writes it. A test that is about the cell itself asks for `raw_code_of`.
std::int64_t code_of(std::string_view source, const Limits& limits = {})
{
    auto outcome = execute(source, limits);
    REQUIRE(outcome.has_value());
    return as_i32(outcome->code);
}

/// The exit code as the raw cell it is, for a test about the bits rather than the
/// number: an `i32` cell is zero-extended, so a negative code has no sign above it.
Cell raw_code_of(std::string_view source, const Limits& limits = {})
{
    auto outcome = execute(source, limits);
    REQUIRE(outcome.has_value());
    return outcome->code;
}

/// The final global state, one cell per global in declaration order.
std::vector<Cell> globals_of(std::string_view source, const Limits& limits = {})
{
    auto outcome = execute(source, limits);
    REQUIRE(outcome.has_value());
    return outcome->globals;
}

/// A program lowered but not run, for a test that breaks one rule of the IR on
/// purpose: the parser never emits an invalid program, and reaching the machine's
/// own refusals is the only way there.
Program lower(std::string_view source)
{
    auto tokens = Tokenizer {}.tokenize(source, "test.erx");
    REQUIRE(tokens.has_value());
    auto program = ProgramParser { *tokens }.parse();
    REQUIRE(program.has_value());
    return *program;
}

std::string read_file(const std::string& path)
{
    std::ifstream file { path, std::ios::binary };
    REQUIRE(file.good());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string example_path(std::string_view name)
{
    return std::string { ERAX_EXAMPLES_DIR } + "/" + std::string { name };
}

/// The smallest function there is: one block that returns `value` and does nothing
/// else. A program written by hand is mostly this plus the one thing it is about.
Function entry_returning(Operand value)
{
    Function entry;
    entry.return_type = Type::I32;
    entry.frame_size = 1;
    entry.cfg.blocks.resize(1);
    entry.cfg.blocks[0].commands.push_back(Command { Op::Ret, { value } });
    return entry;
}

/// Runs a shipped example and hands back what it did. The examples are read as
/// files so a location in a failure names the file it came from.
std::expected<Outcome, erax::interp::Failure> run_example(std::string_view name)
{
    const std::string path = example_path(name);
    auto tokens = Tokenizer {}.tokenize(read_file(path), path);
    REQUIRE(tokens.has_value());
    auto program = ProgramParser { *tokens }.parse();
    REQUIRE(program.has_value());
    return erax::interp::run(*program);
}

Cell example_code(std::string_view name)
{
    auto outcome = run_example(name);
    REQUIRE(outcome.has_value());
    return outcome->code;
}

/// Whether a file lowers at all, for an example that is expected not to: the
/// interpreter is never asked about a program that did not make it into IR.
bool lowers(std::string_view name)
{
    const std::string path = example_path(name);
    auto tokens = Tokenizer {}.tokenize(read_file(path), path);
    if (!tokens)
        return false;
    return ProgramParser { *tokens }.parse().has_value();
}

} // namespace

TEST_CASE("the entrypoint's return is the program's exit code")
{
    CHECK(code_of("fn main(): i32 { return 42; }") == 42);
    CHECK(code_of("fn main(): i32 { return 0; }") == 0);
    CHECK(code_of("fn main(): i32 { return -1; }") == -1);
    CHECK(code_of("fn main(): i32 { let x: i32 = 5; return x + x; }") == 10);
    // The code is a raw cell, so a negative exit code is zero-extended bits with
    // nothing above them: the machine never holds a value in a host type.
    CHECK(raw_code_of("fn main(): i32 { return -1; }") == 0xffffffffull);
    CHECK(raw_code_of("fn main(): i32 { return 42; }") == 42ull);
}

TEST_CASE("a program's whole result is its globals and its exit code")
{
    const auto outcome = execute("let counter: i32 = 2; fn main(): i32 { counter = counter * 3; return counter; }");
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome { { 6 }, 6 });
    // A program that declares no globals leaves the list empty: the exit code is
    // the entrypoint's return, not a cell of its own.
    CHECK(execute("fn main(): i32 { return 1; }")->globals.empty());
}

TEST_CASE("print writes a line of values to the program's output")
{
    // Values are separated by single spaces and a line is ended with one newline.
    CHECK(execute("fn main(): i32 { print(1, 2, 3); return 0; }")->output == "1 2 3\n");
    // Zero arguments still writes the newline.
    CHECK(execute("fn main(): i32 { print(); return 0; }")->output == "\n");
    // A value prints as the decimal its type reads: signed values sign-extend,
    // unsigned values zero-extend, and `char`/`bool` are the byte they lower to.
    CHECK(execute("fn main(): i32 { print(-1); return 0; }")->output == "-1\n");
    CHECK(execute("fn main(): i32 { print(255u8); return 0; }")->output == "255\n");
    CHECK(execute("fn main(): i32 { let x: i64 = 300; print(x); return 0; }")->output == "300\n");
    CHECK(execute("let g: i64 = -7; fn main(): i32 { print(g); return 0; }")->output == "-7\n");
    CHECK(execute("fn main(): i32 { print(true, 'x'); return 0; }")->output == "1 120\n");
    // An expression is folded and printed the same way a return value is.
    CHECK(execute("fn main(): i32 { print(10 - 2 * 3); return 0; }")->output == "4\n");
    // Everything accumulates: each call appends its own line.
    CHECK(execute("fn main(): i32 { print(1); print(); print(2); return 0; }")->output == "1\n\n2\n");
    // Printing changes nothing else about the run: the result is still the globals
    // and the exit code.
    const auto outcome = execute("let n: i32 = 4; fn main(): i32 { print(n, n * n); return n; }");
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome { { 4 }, 4, "4 16\n" });
}

TEST_CASE("a global is zeroed, then written in declaration order")
{
    CHECK(globals_of("fn main(): i32 { return 0; }").empty());
    const std::vector<Cell> globals = globals_of("let a: i32 = 1; let b: i32 = 2; let c: i32 = a + b; fn main(): i32 { return c; }");
    CHECK(globals == std::vector<Cell> { 1, 2, 3 });
    // An initializer runs in order, so a later one sees what an earlier wrote.
    CHECK(globals_of("let a: i32 = 1; let b: i32 = a * 10; fn main(): i32 { return b; }") == std::vector<Cell> { 1, 10 });
    // A global outlives the frame that wrote it, and no frame of its own holds it.
    CHECK(globals_of("let n: i32 = 0; fn bump() { n = n + 1; } fn main(): i32 { bump(); bump(); return n; }") == std::vector<Cell> { 2 });
    // A global with no initializer is zeroed by the machine rather than run, which
    // only a program built by hand can say: the parser always writes one.
    Program program;
    program.globals.push_back(Global { Type::I32, {} });
    program.functions.push_back(entry_returning(Operand::global(Type::I32, 0)));
    const auto outcome = erax::interp::run(program);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome { { 0 }, 0 });
}

TEST_CASE("a call puts its value in the caller's frame")
{
    CHECK(code_of("fn f(n: i32): i32 { return n; } fn main(): i32 { return f(3); }") == 3);
    CHECK(code_of("fn f(n: i32): i32 { return n + 1; } fn main(): i32 { return f(3); }") == 4);
    // Arguments are copies, so writing a parameter does not touch the caller's.
    CHECK(code_of("fn f(n: i32): i32 { n = n + 1; return n; } fn main(): i32 { let x: i32 = 1;"
                  " let y: i32 = f(x); return x + y; }")
        == 3);
    // A call's result is a cell of its own, so nested calls do not share a slot.
    CHECK(code_of("fn f(n: i32): i32 { return n + 1; } fn g(n: i32): i32 { return n * 2; }"
                  " fn main(): i32 { return f(g(4)); }")
        == 9);
    // A void callee leaves nothing behind and stops the statement there.
    CHECK(code_of("fn f(x: i32) { } fn main(): i32 { f(1); return 5; }") == 5);
}

TEST_CASE("recursion is another frame of the same shape")
{
    CHECK(code_of("fn fib(n: i32): i32 { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }"
                  " fn main(): i32 { return fib(10); }")
        == 55);
    CHECK(code_of("fn down(n: i32): i32 { if (n == 0) { return 0; } return down(n - 1) + 1; }"
                  " fn main(): i32 { return down(100); }")
        == 100);
    // A frame's cells are its own: `n` is `$0` in every one of them.
    CHECK(code_of("fn count(n: i32): i32 { if (n == 0) { return 0; } let m: i32 = n;"
                  " return count(n - 1) + (m - m); } fn main(): i32 { return count(50); }")
        == 0);
}

TEST_CASE("a loop runs its commands until the test stops holding")
{
    // A `while` re-tests its own header and runs nothing else again, which is why
    // the increment survives: the initializer is not in the loop's block.
    CHECK(code_of("fn main(): i32 { let i: i32 = 0; while (i < 3) { i = i + 1; } return i; }") == 3);
    CHECK(code_of("fn main(): i32 { let i: i32 = 0; while (i > 3) { i = i + 1; } return i; }") == 0);
    CHECK(code_of("fn main(): i32 { let s: i32 = 0; for (let i: i32 = 0; i < 10; i = i + 1) { s = s + i; }"
                  " return s; }")
        == 45);
    CHECK(code_of("fn main(): i32 { let i: i32 = 0; do { i = i + 1; } while (i < 3); return i; }") == 3);
    // `break` leaves and `continue` goes back to the test, both inside the frame.
    CHECK(code_of("fn main(): i32 { let i: i32 = 0; while (true) { i = i + 1; if (i > 3) { break; } }"
                  " return i; }")
        == 4);
    CHECK(code_of("fn main(): i32 { let i: i32 = 0; while (i < 5) { i = i + 1; if (i == 3) { continue; }"
                  " i = i + 10; } return i; }")
        == 11);
    // A nested loop is its own frame slice and its own test.
    CHECK(code_of("fn main(): i32 { let total: i32 = 0;"
                  " for (let i: i32 = 0; i < 3; i = i + 1) { for (let j: i32 = 0; j < 2; j = j + 1) {"
                  " total = total + 1; } } return total; }")
        == 6);
    // An `if` inside a loop picks its arm per pass, not once at parse time.
    CHECK(code_of("fn main(): i32 { let n: i32 = 0;"
                  " for (let i: i32 = 0; i < 4; i = i + 1) { if (i % 2 == 0) { n = n + i; } }"
                  " return n; }")
        == 2);
}

TEST_CASE("arithmetic is the IR's arithmetic")
{
    CHECK(code_of("fn main(): i32 { let a: i32 = 7; let b: i32 = 2; return a / b; }") == 3);
    // Truncation is toward zero, signed and unsigned alike.
    CHECK(code_of("fn main(): i32 { let a: i32 = -7; let b: i32 = 2; return a / b; }") == -3);
    CHECK(code_of("fn main(): i32 { let a: i32 = -7; let b: i32 = 2; return a % b; }") == -1);
    CHECK(code_of("fn main(): i32 { let a: u32 = 7; let b: u32 = 2; if (a / b == 3) { return 1; }"
                  " return 0; }")
        == 1);
    // Signedness is in the op's name, so the same bits order two ways.
    CHECK(code_of("fn main(): i32 { let a: i32 = 1; let b: i32 = -1; if (a < b) { return 10; } return 20; }") == 20);
    CHECK(code_of("fn main(): i32 { let a: u32 = 1; let b: u32 = 4294967295; if (a < b) { return 10; }"
                  " return 20; }")
        == 10);
    // Overflow wraps, so a sum past the type's maximum is the negative end.
    CHECK(code_of("fn main(): i32 { let a: i32 = 2147483647; return a + 1; }") == -2147483648ll);
    // Shift counts are masked to the operand width: 33 & 31 is 1.
    CHECK(code_of("fn main(): i32 { let a: i32 = 1; let n: i32 = 33; return a << n; }") == 2);
    CHECK(code_of("fn main(): i32 { let a: i32 = -16; let n: i32 = 34; return a >> n; }") == -4);
    // Bitwise ops land in their own slots: 12 & 10 = 8, 12 | 10 = 14, 12 ^ 10 = 6.
    CHECK(code_of("fn main(): i32 { let a: i32 = 12; let b: i32 = 10; let c: i32 = 0;"
                  " let d: i32 = 0; let e: i32 = 0; c = a & b; d = a | b; e = a ^ b;"
                  " return c * 100 + d * 10 + e; }")
        == 946);
    CHECK(code_of("fn main(): i32 { let a: i32 = 5; let flag: bool = true; if (!flag) { return 0; }"
                  " return -a; }")
        == -5);
    // A `u64` cell holds a value no signed type has, and wraps like any other: the
    // sum is only out of range at run time, so nothing folds it away.
    CHECK(globals_of("let g: u64 = 0u64; fn main(): i32 { let a: u64 = 18446744073709551615u64;"
                     " let b: u64 = 1u64; g = a + b; return 0; }")
        == std::vector<Cell> { 0 });
    CHECK(globals_of("let g: u64 = 18446744073709551615u64; fn main(): i32 { return 0; }") == std::vector<Cell> { 18446744073709551615ull });
}

TEST_CASE("a division by zero is a trap, not a value")
{
    auto outcome = execute("fn main(): i32 { let z: i32 = 0; return 7 / z; }");
    REQUIRE_FALSE(outcome.has_value());
    const Trapped& trapped = std::get<Trapped>(outcome.error());
    CHECK(trapped.trap == Trap::DivisionByZero);
    CHECK(trapped.site.subject == Subject::Function);
    CHECK(trapped.site.index == 0);
    CHECK(trapped.site.depth == 1);
    CHECK(trapped.stack == std::vector<std::size_t> { 0 });
    // The report names the command and the call stack, which is the only place a
    // nameless IR can say where it was.
    const std::string report = erax::interp::to_string(outcome.error());
    CHECK(report.find("division by zero") != std::string::npos);
    CHECK(report.find("function 0") != std::string::npos);
    // Modulo dies the same way.
    auto modulo = execute("fn main(): i32 { let z: i32 = 0; return 7 % z; }");
    REQUIRE_FALSE(modulo.has_value());
    CHECK(std::get<Trapped>(modulo.error()).trap == Trap::DivisionByZero);
}

TEST_CASE("a signed division with no answer dies too")
{
    // `i32min / -1` is 2147483648, which no `i32` holds: there is no bit pattern
    // to return, so the machine stops rather than wrapping.
    auto outcome = execute("fn main(): i32 { let a: i32 = -2147483648; let b: i32 = -1; return a / b; }");
    REQUIRE_FALSE(outcome.has_value());
    CHECK(std::get<Trapped>(outcome.error()).trap == Trap::SignedOverflow);
    auto modulo = execute("fn main(): i32 { let a: i64 = -9223372036854775807i64 - 1i64; let b: i64 = -1i64;"
                          " let c: i64 = a % b; return 0; }");
    REQUIRE_FALSE(modulo.has_value());
    CHECK(std::get<Trapped>(modulo.error()).trap == Trap::SignedOverflow);
}

TEST_CASE("a trap in a callee carries the call stack it happened on")
{
    auto outcome = execute("fn inner(z: i32): i32 { return 1 / z; }"
                           " fn outer(z: i32): i32 { return inner(z); }"
                           " fn main(): i32 { return outer(0); }");
    REQUIRE_FALSE(outcome.has_value());
    const Trapped& trapped = std::get<Trapped>(outcome.error());
    CHECK(trapped.trap == Trap::DivisionByZero);
    // The innermost frame is where it died; the rest is how it got there.
    CHECK(trapped.stack == std::vector<std::size_t> { 2, 1, 0 });
    CHECK(trapped.site.depth == 3);
    CHECK(erax::interp::to_string(outcome.error()).find("function 1") != std::string::npos);
}

TEST_CASE("a machine that runs out of room says so instead of growing")
{
    const std::string deep = "fn down(n: i32): i32 { if (n == 0) { return 0; } return down(n - 1); }"
                             " fn main(): i32 { return down(100); }";
    CHECK(code_of(deep) == 0);
    // The limits count the entrypoint's own frame, so this stops at the fourth.
    auto outcome = execute(deep, Limits { 4, 1 << 20 });
    REQUIRE_FALSE(outcome.has_value());
    const Trapped& trapped = std::get<Trapped>(outcome.error());
    CHECK(trapped.trap == Trap::StackExhausted);
    CHECK(trapped.site.depth == 4);
    CHECK(trapped.stack.size() == 4);
    // Cells are the other half of the same limit. Every frame here is one cell, so
    // one cell of room is the entrypoint and nothing more.
    auto narrow = execute("fn f(n: i32): i32 { return n; } fn main(): i32 { return f(1); }", Limits { 4096, 1 });
    REQUIRE_FALSE(narrow.has_value());
    CHECK(std::get<Trapped>(narrow.error()).trap == Trap::StackExhausted);
    // A frame wider than the whole cell array can never fit, which is a different
    // answer from running out of room as frames pile up.
    auto wide = execute("fn main(): i32 { return 1; }", Limits { 4096, 0 });
    REQUIRE_FALSE(wide.has_value());
    CHECK(std::get<Trapped>(wide.error()).trap == Trap::FrameTooLarge);
    // The entrypoint itself counts, so a limit of one still runs a straight line.
    CHECK(execute("fn main(): i32 { return 1; }", Limits { 1, 4 })->code == 1);
}

TEST_CASE("a global initializer is a frame of its own")
{
    // An initializer has no frame in the IR, so the machine gives it the cells its
    // vregs name and runs it before the entrypoint exists.
    const Program program = lower("let a: i32 = 1; let b: i32 = a + 2; fn main(): i32 { return b; }");
    REQUIRE(program.globals.size() == 2);
    const auto outcome = erax::interp::run(program);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome { { 1, 3 }, 3 });
    // The parser folds every initializer it can, so only IR written by hand has an
    // initializer that needs cells of its own: here `$0` is the global's own value,
    // computed in a frame rather than read from the table.
    Program hand;
    hand.globals.push_back(Global { Type::I32, {} });
    Global& counted = hand.globals.back();
    counted.init.blocks.resize(1);
    counted.init.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 20) } });
    counted.init.blocks[0].commands.push_back(
        Command { Op::Add, { Operand::vreg(Type::I32, 1), Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 0) } });
    counted.init.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::global(Type::I32, 0), Operand::vreg(Type::I32, 1) } });
    counted.init.blocks[0].commands.push_back(Command { Op::Ret, {} });
    hand.functions.push_back(entry_returning(Operand::global(Type::I32, 0)));
    const auto computed = erax::interp::run(hand);
    REQUIRE(computed.has_value());
    CHECK(*computed == Outcome { { 40 }, 40 });
    // The initializer's cells are counted against the same limit as every other
    // frame, so one that does not fit stops the run before the entrypoint exists.
    auto narrow = erax::interp::run(hand, Limits { 4096, 1 });
    REQUIRE_FALSE(narrow.has_value());
    const Trapped& trapped = std::get<Trapped>(narrow.error());
    CHECK(trapped.trap == Trap::FrameTooLarge);
    CHECK(trapped.site.subject == Subject::GlobalInit);
    CHECK(trapped.site.depth == 1);
}

TEST_CASE("a malformed program is refused before it runs")
{
    // The machine reads every operand through an index, so it asks the validator
    // first: an operand outside its frame is a message, never a wild read.
    Program program = lower("fn main(): i32 { let x: i32 = 1; return x; }");
    auto& ret = program.functions[0].cfg.blocks[0].commands.back();
    REQUIRE(ret.op == Op::Ret);
    REQUIRE(ret.args[0].kind == OperandKind::VReg);
    ret.args[0].value = 9;
    auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Rejected>(outcome.error()));
    // Two rules see it: the cell is outside the frame, and nothing ever wrote it.
    std::vector<std::string> texts;
    for (const erax::middleend::ValidationError& error : std::get<Rejected>(outcome.error()).errors) {
        texts.push_back(error.message);
    }
    REQUIRE(texts.size() == 2);
    CHECK(texts[0].find("names $9 in a frame of 1 cells") != std::string::npos);
    CHECK(texts[1].find("reads $9, which nothing has written") != std::string::npos);
    // The report counts them and names the first, so a caller with ten errors does
    // not have to print ten.
    const std::string report = erax::interp::to_string(outcome.error());
    CHECK(report.find("2 errors, the first:") != std::string::npos);
    CHECK(report.find("$9") != std::string::npos);
    // Every rule the validator has is out of reach of the machine, so the driver
    // never gets a `Broken` for a program it accepted.
    CHECK(!std::holds_alternative<Broken>(outcome.error()));
}

TEST_CASE("the shipped examples run")
{
    // The examples are the corpus the driver lowers by hand; each one's result is
    // what its comment says it does.
    CHECK(example_code("entry.erx") == 0);
    CHECK(example_code("fib.erx") == 55);
    CHECK(example_code("folded_test.erx") == 4);
    CHECK(example_code("globals.erx") == 2);
    CHECK(example_code("print.erx") == 0);
    CHECK(example_code("sum_to_ten.erx") == 45);
    CHECK(example_code("types.erx") == 0);
    // `print.erx` is the one that writes output, so its bytes matter as much as
    // its exit code; `globals.erx` is the one whose final global state matters.
    const auto printed = run_example("print.erx");
    REQUIRE(printed.has_value());
    CHECK(printed->output == "1 2 3\n-42 255\n120 1\n\n");
    const auto globals = run_example("globals.erx");
    REQUIRE(globals.has_value());
    CHECK(globals->globals == std::vector<Cell> { 2 });
    // `helloworld.erx` is aspirational: `string` is not in v1, so it never
    // lowers and the interpreter is never asked about it.
    CHECK_FALSE(lowers("helloworld.erx"));
}

TEST_CASE("a global initializer can branch toward its own value")
{
    // An initializer is straight-line when the parser writes it, but its CFG may
    // still have edges, and the machine counts the cells those edges read: the
    // branch test's vregs are part of what the initializer's frame has to hold.
    Program program;
    program.globals.push_back(Global { Type::I32, {} });
    Global& branching = program.globals.back();
    branching.init.blocks.resize(2);
    branching.init.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 5) } });
    branching.init.blocks[0].next = 1;
    branching.init.blocks[0].branch = Branch {
        { Op::CmpNe, Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 0) }, 1
    };
    branching.init.blocks[1].commands.push_back(
        Command { Op::Mov, { Operand::global(Type::I32, 0), Operand::vreg(Type::I32, 0) } });
    branching.init.blocks[1].commands.push_back(Command { Op::Ret, {} });
    program.functions.push_back(entry_returning(Operand::global(Type::I32, 0)));
    const auto outcome = erax::interp::run(program);
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome { { 5 }, 5 });
}

TEST_CASE("a block with a branch but no fallthrough dies when the test does not hold")
{
    // The validator accepts a two-edge block with the branch missing its
    // fallthrough, so the machine's own walk has to say what an untaken branch
    // means there: a message, not an index into nothing.
    Program program;
    Function entry;
    entry.return_type = Type::I32;
    entry.frame_size = 1;
    entry.cfg.blocks.resize(2);
    entry.cfg.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 0) } });
    entry.cfg.blocks[0].branch = Branch {
        { Op::CmpNe, Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 0) }, 1
    };
    entry.cfg.blocks[1].commands.push_back(Command { Op::Ret, { Operand::vreg(Type::I32, 0) } });
    program.functions.push_back(entry);
    const auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Broken>(outcome.error()));
    CHECK(std::get<Broken>(outcome.error()).what == "a block with a branch has no fallthrough edge");
}

TEST_CASE("reading a global the program does not declare is a message, not a wild read")
{
    // The validator checks frames, calls, and every vreg, but not global ids, so
    // a run can reach the machine's own reading of an index: it reports rather
    // than indexing past the end of the global array.
    Program program;
    Function entry;
    entry.return_type = Type::I32;
    entry.frame_size = 2;
    entry.cfg.blocks.resize(1);
    entry.cfg.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1) } });
    entry.cfg.blocks[0].commands.push_back(Command { Op::Add,
                                                     { Operand::vreg(Type::I32, 1), Operand::global(Type::I32, 42),
                                                       Operand::global(Type::I32, 42) } });
    entry.cfg.blocks[0].commands.push_back(Command { Op::Ret, { Operand::vreg(Type::I32, 1) } });
    program.functions.push_back(entry);
    auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Broken>(outcome.error()));
    CHECK(std::get<Broken>(outcome.error()).what == "an operand names a global that does not exist");

    // The same read through a unary op dies the same way.
    Function unary;
    unary.return_type = Type::I32;
    unary.frame_size = 1;
    unary.cfg.blocks.resize(1);
    unary.cfg.blocks[0].commands.push_back(
        Command { Op::Not, { Operand::vreg(Type::I32, 0), Operand::global(Type::I32, 42) } });
    unary.cfg.blocks[0].commands.push_back(Command { Op::Ret, { Operand::vreg(Type::I32, 0) } });
    Program again;
    again.functions.push_back(unary);
    const auto second = erax::interp::run(again);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(std::holds_alternative<Broken>(second.error()));
    CHECK(std::get<Broken>(second.error()).what == "an operand names a global that does not exist");
}

TEST_CASE("writing a global the program does not declare is a message, not a wild write")
{
    Program program;
    program.functions.push_back(entry_returning(Operand::instant(Type::I32, 0)));
    auto& block = program.functions[0].cfg.blocks[0];
    block.commands.insert(block.commands.begin(),
                          Command { Op::Mov, { Operand::global(Type::I32, 7), Operand::instant(Type::I32, 1) } });
    const auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Broken>(outcome.error()));
    CHECK(std::get<Broken>(outcome.error()).what == "a write names a global that does not exist");
}

TEST_CASE("an unreadable operand dies wherever the machine reads it")
{
    // The validator never checks global ids, so an out-of-range global can sit in
    // any operand slot; every one of the machine's readers reports it the same
    // way rather than indexing past the end of the global array.
    const auto broken = [](Program program) {
        const auto outcome = erax::interp::run(program);
        REQUIRE_FALSE(outcome.has_value());
        REQUIRE(std::holds_alternative<Broken>(outcome.error()));
        return std::get<Broken>(outcome.error()).what;
    };

    // A `RET` reading the missing global.
    Program returned;
    Function ret_entry;
    ret_entry.return_type = Type::I32;
    ret_entry.frame_size = 1;
    ret_entry.cfg.blocks.resize(1);
    ret_entry.cfg.blocks[0].commands.push_back(Command { Op::Ret, { Operand::global(Type::I32, 42) } });
    returned.functions.push_back(ret_entry);
    CHECK(broken(std::move(returned)) == "an operand names a global that does not exist");

    // A call handing the missing global to a callee.
    Program called;
    Function callee;
    callee.params.push_back(Type::I32);
    callee.return_type = Type::I32;
    callee.frame_size = 1;
    callee.cfg.blocks.resize(1);
    callee.cfg.blocks[0].commands.push_back(Command { Op::Ret, { Operand::vreg(Type::I32, 0) } });
    Function caller;
    caller.return_type = Type::I32;
    caller.frame_size = 1;
    caller.cfg.blocks.resize(1);
    caller.cfg.blocks[0].commands.push_back(Command { Op::Call,
                                                      { Operand::vreg(Type::I32, 0), Operand::function(Type::I32, 0),
                                                        Operand::global(Type::I32, 42) } });
    caller.cfg.blocks[0].commands.push_back(Command { Op::Ret, { Operand::vreg(Type::I32, 0) } });
    called.functions.push_back(callee);
    called.functions.push_back(caller);
    called.entrypoint = 1;
    CHECK(broken(std::move(called)) == "an operand names a global that does not exist");

    // A branch test on the missing global.
    Program branched;
    Function branch_entry;
    branch_entry.return_type = Type::I32;
    branch_entry.frame_size = 1;
    branch_entry.cfg.blocks.resize(2);
    branch_entry.cfg.blocks[0].next = 1;
    branch_entry.cfg.blocks[0].branch = Branch {
        { Op::CmpNe, Operand::global(Type::I32, 42), Operand::global(Type::I32, 42) }, 1
    };
    branch_entry.cfg.blocks[1].commands.push_back(Command { Op::Ret, { Operand::instant(Type::I32, 0) } });
    branched.functions.push_back(branch_entry);
    CHECK(broken(std::move(branched)) == "an operand names a global that does not exist");
}

TEST_CASE("a global initializer can die before the entrypoint exists")
{
    // An initializer runs in a frame of its own, so an initializer that traps is
    // the whole program dying there: the entrypoint is never pushed.
    Program program;
    program.globals.push_back(Global { Type::I32, {} });
    Global& doomed = program.globals.back();
    doomed.init.blocks.resize(1);
    doomed.init.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 7) } });
    doomed.init.blocks[0].commands.push_back(
        Command { Op::Mov, { Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 0) } });
    doomed.init.blocks[0].commands.push_back(Command { Op::Sdiv, { Operand::vreg(Type::I32, 2),
                                                                   Operand::vreg(Type::I32, 0),
                                                                   Operand::vreg(Type::I32, 1) } });
    doomed.init.blocks[0].commands.push_back(Command { Op::Ret, {} });
    program.functions.push_back(entry_returning(Operand::instant(Type::I32, 0)));
    const auto outcome = erax::interp::run(program);
    REQUIRE_FALSE(outcome.has_value());
    const Trapped& trapped = std::get<Trapped>(outcome.error());
    CHECK(trapped.trap == Trap::DivisionByZero);
    CHECK(trapped.site.subject == Subject::GlobalInit);
    CHECK(trapped.site.index == 0);
    CHECK(trapped.site.depth == 1);
}

TEST_CASE("every trap has a name")
{
    CHECK(erax::interp::to_string(Trap::DivisionByZero) == "division by zero");
    CHECK(erax::interp::to_string(Trap::SignedOverflow) == "signed division overflowed");
    CHECK(erax::interp::to_string(Trap::StackExhausted) == "the stack outgrew the machine");
    CHECK(erax::interp::to_string(Trap::FrameTooLarge) == "a frame is larger than the machine allows");
}

TEST_CASE("a failure reads as a message, and an empty rejection says so")
{
    CHECK(erax::interp::to_string(erax::interp::Failure { erax::interp::Broken { "nope" } }) == "nope");
    const erax::interp::Failure empty = erax::interp::Rejected {};
    CHECK(erax::interp::to_string(empty) == "0 errors, the first: no reason given");
}