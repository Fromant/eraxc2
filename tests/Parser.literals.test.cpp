#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ParserTest.hpp"

using namespace erax::frontend;
using namespace erax::middleend;
using namespace erax::test;

/// Literals and the folding that happens while they are parsed. A constant
/// expression is computed here rather than at run time, so every rule about the
/// machine's arithmetic shows up as a bit pattern or as a message.

namespace {

/// The width of a type name, for reading a folded bit pattern back as a number.
std::size_t width_of(std::string_view type) {
    if (type == "i8" || type == "u8" || type == "char" || type == "bool") return 8;
    if (type == "i16" || type == "u16") return 16;
    if (type == "i32" || type == "u32") return 32;
    return 64;
}

/// The single folded initializer of `let x: T = <expression>;`, as raw bits.
std::uint64_t folded_bits(std::string_view expression, std::string_view type = "i32") {
    const Program program = parse_ok("fn main(): i32 { let x: " + std::string{type} + " = " +
                                     std::string{expression} + "; return 0; }");
    const std::vector<Command> commands = commands_of(program, 0);
    REQUIRE(commands.size() == 2);
    REQUIRE(commands[0].op == Op::Mov);
    REQUIRE(commands[0].args[1].kind == OperandKind::Instant);
    return commands[0].args[1].value;
}

/// The same bits read as a number, sign-extended from the width of the type.
std::int64_t folded_signed(std::string_view expression, std::string_view type = "i32") {
    const std::uint64_t bits = folded_bits(expression, type);
    const std::size_t width = width_of(type);
    const std::uint64_t mask = width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
    if ((bits & (std::uint64_t{1} << (width - 1))) == 0) return static_cast<std::int64_t>(bits);
    return static_cast<std::int64_t>(bits | ~mask);
}

}  // namespace

TEST_CASE("a radix literal is read in its own base") {
    // The base follows the `0`, in either case.
    CHECK(folded_bits("0xFF", "u8") == 255);
    CHECK(folded_bits("0Xff", "u8") == 255);
    CHECK(folded_bits("0b1011", "u8") == 11);
    CHECK(folded_bits("0B1011", "u8") == 11);
    CHECK(folded_bits("0o17", "u8") == 15);
    CHECK(folded_bits("0O17", "u8") == 15);
    // A hexadecimal digit above 9 is a letter, so the letter branch runs.
    CHECK(folded_bits("0xDEAD", "u32") == 0xDEAD);
    // Separators are the tokenizer's job and survive into the parser.
    CHECK(folded_bits("0xFF_FF", "u32") == 0xFFFF);
    CHECK(folded_bits("1_000_000", "u32") == 1000000);
    // Plain decimal is the no-prefix case.
    CHECK(folded_bits("1234", "u32") == 1234);
    // A prefix is only a prefix on a zero: `0` is a zero, not an octal nothing.
    CHECK(folded_bits("0", "u32") == 0);
    // More than any type can hold is not a constant at all.
    CHECK(any_error("fn main(): i32 { let x: u64 = 18446744073709551616; return 0; }") ==
          "integer literal '18446744073709551616' is too large for any type");
    // A decimal digit is out of range for a binary or octal literal.
    CHECK(any_error("fn main(): i32 { let x: u8 = 0b1012; return 0; }") != "");
}

TEST_CASE("a u64 constant is exact up to i64max and a bit pattern above it") {
    // Below `i64max` a constant is an exact value, so folding can be exact.
    CHECK(folded_bits("9223372036854775807", "u64") == 9223372036854775807ULL);
    CHECK(folded_bits("9223372036854775807", "i64") == 9223372036854775807ULL);
    // Above it, only the bit pattern is known, and it is still a valid u64.
    CHECK(folded_bits("18446744073709551615", "u64") == 18446744073709551615ULL);
    // ... and the widest type is the only one it fits.
    CHECK(parse_error("fn main(): i32 { let x: i64 = 18446744073709551615; return 0; }") ==
          "this constant does not fit i64");
    CHECK(parse_error("fn main(): i32 { let x: u32 = 4294967296; return 0; }") ==
          "this constant does not fit u32");
    // A literal is not checked against the default type it is first read as: the type
    // of the position it fills is the one that counts, even at the far end.
    CHECK(folded_bits("4294967296", "u64") == 4294967296ULL);
    CHECK(folded_bits("4294967296", "i64") == 4294967296ULL);
    // `i64min` has no literal, but it can be written as an expression of exact ones.
    CHECK(folded_signed("-9223372036854775807 - 1", "i64") == -9223372036854775807 - 1);
    CHECK(folded_signed("-2147483647 - 1", "i32") == -2147483647 - 1);
    CHECK(folded_bits("-8", "i8") == 0xF8);
}

TEST_CASE("a char literal is an escape sequence or a code point") {
    // The simple escapes, each its own case.
    CHECK(folded_bits("'\\n'", "char") == 10);
    CHECK(folded_bits("'\\t'", "char") == 9);
    CHECK(folded_bits("'\\r'", "char") == 13);
    CHECK(folded_bits("'\\0'", "char") == 0);
    CHECK(folded_bits("'\\b'", "char") == 8);
    CHECK(folded_bits("'\\f'", "char") == 12);
    CHECK(folded_bits("'\\v'", "char") == 11);
    CHECK(folded_bits("'\\\\'", "char") == 92);
    CHECK(folded_bits("'\\''", "char") == 39);
    CHECK(folded_bits("'\\\"'", "char") == 34);
    // Two hexadecimal digits, or four for a universal code point.
    CHECK(folded_bits("'\\x41'", "char") == 65);
    CHECK(folded_bits("'\\u0041'", "char") == 65);
    // A digit above 9 in a `\x` escape is a letter.
    CHECK(folded_bits("'\\xff'", "char") == 255);
    // A code point is still a 0-255 byte in v1.
    CHECK(parse_error("fn main(): i32 { let c: char = '\\u0100'; return 0; }") ==
          "character literal is not a 0-255 code point");
    // A malformed escape is the tokenizer's, so it never reaches the parser.
    CHECK(any_error("fn main(): i32 { let c: char = '\\q'; return 0; }") ==
          "invalid escape sequence '\\q'");
    // The digit count is the parser's rule, because the tokenizer keeps the escape
    // text raw and cannot know which of the two forms it is in.
    CHECK(parse_error("fn main(): i32 { let c: char = '\\x4'; return 0; }") ==
          "expected 2 hexadecimal digits after \\x");
    CHECK(parse_error("fn main(): i32 { let c: char = '\\u041'; return 0; }") ==
          "expected 4 hexadecimal digits after \\u");
    CHECK(parse_error("fn main(): i32 { let c: char = '\\x412'; return 0; }") != "");
    // A `char` is a byte, so a bare `char` literal is its own code point, and it is
    // the only type a char literal can fill: it is not an integer literal.
    CHECK(folded_bits("'A'", "char") == 65);
    CHECK(parse_error("fn main(): i32 { let c: i8 = 'A'; return 0; }") ==
          "expected a value of type i8, not char");
    CHECK(parse_error("fn main(): i32 { let c: u8 = '\\u0100'; return 0; }") != "");
}

TEST_CASE("a constant expression is exact, and its value must fit") {
    // The value has to be representable in the type it takes, which is not the same
    // thing as its bits fitting: 256 is a perfectly good 0 in eight bits.
    CHECK(folded_bits("127", "i8") == 127);
    CHECK(parse_error("fn main(): i32 { let x: i8 = 128; return 0; }") == "this constant does not fit i8");
    CHECK(parse_error("fn main(): i32 { let x: u8 = 255 + 1; return 0; }") == "this constant does not fit u8");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 2147483647 + 1; return 0; }") ==
          "this constant does not fit i32");
    // A negative value is not an unsigned one, even where its bits would fit.
    CHECK(parse_error("fn main(): i32 { let x: u8 = -1; return 0; }") == "this constant does not fit u8");
    // The whole expression is checked, not each literal in it: a literal is only a
    // placeholder until the expression it is in has a type.
    CHECK(folded_bits("100 + 27", "i8") == 127);
    CHECK(parse_error("fn main(): i32 { let x: i8 = 100 + 28; return 0; }") == "this constant does not fit i8");
    // A parenthesized expression is the expression in it, so it takes the type too.
    parse_ok("fn main(): i32 { let x: i8 = (1 + 2); return 0; }");
    CHECK(parse_error("fn main(): i32 { let x: i8 = (300 + 2); return 0; }") == "this constant does not fit i8");
    // Every integer operator folds, on the type the position gives it.
    CHECK(folded_bits("0xF0 | 0x0F", "u8") == 0xFF);
    CHECK(folded_bits("0xFF & 0x0F", "u8") == 0x0F);
    CHECK(folded_bits("0xFF ^ 0x0F", "u8") == 0xF0);
    CHECK(folded_signed("-7 / 2") == -3);
    CHECK(folded_signed("-7 % 2") == -1);
    CHECK(folded_bits("255 / 16", "u8") == 15);
    CHECK(folded_bits("255 % 16", "u8") == 15);
    CHECK(folded_signed("100 * -3") == -300);
    // Division and modulus by zero have no answer at all, typed or not.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1 / 0; return 0; }") ==
          "division by zero in a constant expression");
    CHECK(parse_error("fn main(): i32 { let x: u32 = 1 % 0; return 0; }") ==
          "division by zero in a constant expression");
    // A negation and a `~` keep the value exact, so they are range-checked like one.
    // `i32min` is a constant, so negating it overflows at parse time...
    CHECK(parse_error("fn main(): i32 { let x: i32 = -(-2147483647 - 1); return 0; }") ==
          "this constant does not fit i32");
    CHECK(folded_signed("~5") == -6);
    CHECK(parse_error("fn main(): i32 { let x: i8 = -(-127 - 1); return 0; }") == "this constant does not fit i8");
    // ... but negating a value read at runtime only wraps, which is the JIR's rule.
    parse_ok("fn main(): i32 { let x: i8 = -128; let y: i8 = -x; return 0; }");
    CHECK(parse_error("fn main(): i32 { let x: u8 = ~0; return 0; }") == "this constant does not fit u8");
    // A shift count masks to the width the fold is done in. A placeholder folds at the
    // 64-bit width a constant is written in, so the result is exact and the position's
    // own range check is what rejects it if that result does not fit.
    CHECK(folded_bits("1 << 66", "u64") == 4);
    CHECK(folded_bits("1 << 62", "u64") == 1ULL << 62);
    CHECK(parse_error("fn main(): i32 { let x: u32 = 1 << 33; return 0; }") ==
          "this constant does not fit u32");
    CHECK(folded_bits("1 << 40", "i64") == 1ULL << 40);
    CHECK(folded_signed("-8 >> 1") == -4);
    CHECK(folded_signed("-1 >> 1") == -1);
    CHECK(folded_bits("0 << 31", "i32") == 0);
    CHECK(folded_bits("7 << 0", "i32") == 7);
    // An unsigned right shift is logical, and a `char` is unsigned.
    CHECK(folded_bits("255 >> 4", "char") == 15);
    CHECK(folded_bits("255 >> 4", "u8") == 15);
    // A product that leaves the width the expression is checked in is not a constant
    // that can be used there, but the same product in a wider type is.
    CHECK(folded_bits("65536 * 65536", "i64") == 4294967296ULL);
    CHECK(parse_error("fn main(): i32 { let x: i32 = 65536 * 65536; return 0; }") != "");
}

TEST_CASE("a constant expression is one the parser can compute") {
    // A read is not one, and a call is not one.
    CHECK(parse_error("let a: i32 = x; fn main(): i32 { return 0; }") == "undeclared identifier 'x'");
    CHECK(parse_error("fn f(): i32 { return 1; } let a: i32 = f(); fn main(): i32 { return 0; }") ==
          "a constant expression may not call 'f'");
    // ... but literal arithmetic on a global's folded value is.
    const Program program = parse_ok("let a: i32 = 2; let b: i32 = a * 3; fn main(): i32 { return b; }");
    REQUIRE(program.globals[1].init.blocks[0].commands.size() == 2);
    CHECK(program.globals[1].init.blocks[0].commands[0].args[1].value == 6);
    // A read of a global in a function is an operand, not a constant.
    const Program read = parse_ok("let g: i32 = 1; fn main(): i32 { return g + 1; }");
    CHECK(commands_of(read, 0)[0].op == Op::Add);
    // A global is visible to the global above it, and to the ones below.
    parse_ok("let a: i32 = 1; let b: i32 = a + 1; let c: i32 = b + 1; fn main(): i32 { return c; }");
    // ... but not to itself, which is a read of a cell that does not exist yet.
    CHECK(parse_error("let a: i32 = a; fn main(): i32 { return 0; }") != "");
}

TEST_CASE("the arithmetic operator picks its signed form from the operand type") {
    // `/`, `%` and `>>` are signed or unsigned depending on the type, so a `char`
    // division is a logical one. Each is on a runtime value, so it is a command.
    const Program program = parse_ok("fn main(): i32 { let a: char = 255; let b: char = 16;"
                                     " let c: char = a / b; let d: char = a % b; let e: char = a >> 4;"
                                     " let f: i32 = -1; let g: i32 = f / 2; let h: i32 = f >> 1;"
                                     " let i: char = 1; let j: char = i << 1; return 0; }");
    const std::vector<Op> ops = ops_of(program, 0);
    CHECK(has_op(ops, Op::Udiv));
    CHECK(has_op(ops, Op::Umod));
    CHECK(has_op(ops, Op::Shr));
    CHECK(has_op(ops, Op::Sdiv));
    CHECK(has_op(ops, Op::Sar));
    // Shifting left is the same command either way.
    CHECK(has_op(ops, Op::Shl));
    CHECK(!has_op(ops, Op::Smod));
    CHECK(count_of(ops, Op::Mov) == 10);
}

TEST_CASE("an operator is only defined on integers") {
    // A `bool` is not an integer, so it has no arithmetic at all.
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = a + a; return 0; }") ==
          "'+' is not defined for bool");
    // ... and a `bool` is not an `i8` either, even though that is what it is.
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: i8 = a; return 0; }") != "");
    // `!` is on `bool` and nothing else, and it rejects a literal by name.
    CHECK(parse_error("fn main(): i32 { let a: i32 = 1; let b: bool = !a; return 0; }") ==
          "'!' is defined on 'bool', but got i32");
    CHECK(parse_error("fn main(): i32 { let b: bool = !1; return 0; }") ==
          "expected a value of type bool, not an integer literal");
    parse_ok("fn main(): i32 { let a: bool = true; let b: bool = !a; return 0; }");
    // A logical operator wants `bool` on both sides, and says so on the one that is not.
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = a || 1; return 0; }") != "");
    CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = 1 || a; return 0; }") != "");
    CHECK(parse_error("fn main(): i32 { let a: i32 = 1; let b: i32 = a && 1; return 0; }") != "");
    // A call that returns nothing is not an operand.
    CHECK(parse_error("fn f() { } fn main(): i32 { let a: i32 = f() + 1; return 0; }") != "");
    // A `char` is a byte and is an integer, so arithmetic on it is defined -- the
    // literal takes the `char`.
    parse_ok("fn main(): i32 { let a: char = 'A'; let b: char = a + 1; return 0; }");
    // ... but not on two different types.
    CHECK(parse_error("fn main(): i32 { let a: char = 'A'; let b: i32 = a + 1; return 0; }") != "");
    // A comparison is a `bool` whatever it compares, so the operands keep their own.
    parse_ok("fn main(): i32 { let a: char = 'A'; let b: bool = a < 128; return 0; }");
    parse_ok("fn main(): i32 { let a: i32 = 1; let b: bool = a < 2; return 0; }");
    parse_ok("fn main(): i32 { let a: u64 = 1; let b: bool = a == 1; return 0; }");
    parse_ok("fn main(): i32 { let a: i64 = 1; let b: bool = a != 2; return 0; }");
    parse_ok("fn main(): i32 { let a: i16 = 1; let b: bool = a <= 2; return 0; }");
    parse_ok("fn main(): i32 { let a: u16 = 1; let b: bool = a >= 2; return 0; }");
    parse_ok("fn main(): i32 { let a: u8 = 1; let b: bool = a > 2; return 0; }");
}

TEST_CASE("a literal takes the type of the position, and only if it fits") {
    // A lone literal is the one thing that takes the type of the position, so the
    // position is what checks it.
    CHECK(folded_bits("1", "i32") == 1);
    CHECK(parse_error("fn main(): i32 { let a: u8 = 256; return 0; }") != "");
    // A literal in a `bool` position is a type error, not a conversion, and neither
    // is a `bool` in an integer position.
    CHECK(parse_error("fn main(): i32 { let a: bool = 1; return 0; }") != "");
    CHECK(parse_error("fn main(): i32 { let a: i32 = true; return 0; }") ==
          "expected a value of type i32, not bool");
    CHECK(parse_error("fn main(): i32 { let a: char = true; return 0; }") != "");
    // An operator does not take the type of its position: its own type wins, which is
    // what keeps `1 < 2` a `bool`.
    parse_ok("fn main(): i32 { let a: bool = 1 < 2; return 0; }");
    parse_ok("fn main(): i32 { let a: i32 = 1 + 2; return a; }");
    CHECK(parse_error("fn main(): i32 { let a: i32 = 1 < 2; return 0; }") ==
          "expected a value of type i32, not bool");
    CHECK(parse_error("fn main(): i32 { let a: bool = 1 + 2; return 0; }") != "");
    // A return value is a position too, and a `bool` is not a return type.
    CHECK(parse_error("fn f(): bool { return 1; } fn main(): i32 { return 0; }") != "");
    // A float is a literal that has no type to take.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.5; return 0; }") == "float is not supported yet");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1e3; return 0; }") == "float is not supported yet");
    // ... and so is a string, for the same reason.
    CHECK(parse_error("fn main(): i32 { let x: i32 = \"hi\"; return 0; }") == "string is not supported yet");
    // A `1` followed by a dot is a `1` and a dot, not a float: the dot is not a number.
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.; return 0; }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = 1.0; return 0; }") == "float is not supported yet");
}

TEST_CASE("an operator on values it cannot fold still emits its command") {
    // `Neg` and `Not` on a runtime value are commands, not constants.
    const Program program = parse_ok("fn main(): i32 { let a: i32 = 1; let b: i32 = -a;"
                                     " let c: u8 = 1; let d: u8 = ~c; return 0; }");
    const std::vector<Command> commands = commands_of(program, 0);
    CHECK(has_op(ops_of(program, 0), Op::Neg));
    CHECK(has_op(ops_of(program, 0), Op::Not));
    // The destination is a fresh cell, not the operand's.
    int folded = 0;
    for (const Command& command : commands) {
        if (command.op != Op::Neg && command.op != Op::Not) continue;
        ++folded;
        CHECK(command.args[0].kind == OperandKind::VReg);
        CHECK(command.args[1].kind == OperandKind::VReg);
        CHECK(command.args[0].value != command.args[1].value);
    }
    CHECK(folded == 2);
    // A constant on either side still emits, because the value is not known.
    const Program mixed = parse_ok("fn main(): i32 { let a: i32 = 1; let b: i32 = a + 1; return 0; }");
    REQUIRE(commands_of(mixed, 0).size() == 4);
    CHECK(commands_of(mixed, 0)[1].op == Op::Add);
    CHECK(commands_of(mixed, 0)[1].args[1].kind == OperandKind::VReg);
    CHECK(commands_of(mixed, 0)[1].args[2].kind == OperandKind::Instant);
}

TEST_CASE("every integer type lowers and is named in the IR") {
    // The full set, so each arm of the type mapping runs at least once.
    const Program program =
        parse_ok("let a: i8 = 1; let b: u8 = 1; let c: i16 = 1; let d: u16 = 1; let e: i32 = 1;"
                 " let f: u32 = 1; let g: i64 = 1; let h: u64 = 1; fn main(): i32 { return 0; }");
    REQUIRE(program.globals.size() == 8);
    const std::vector<Type> expected{Type::I8,  Type::U8,  Type::I16, Type::U16,
                                     Type::I32, Type::U32, Type::I64, Type::U64};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(program.globals[index].type == expected[index]);
    }
    // A type name is printed the same way everywhere.
    CHECK(std::string{to_string(Type::I8)} == "i8");
    CHECK(std::string{to_string(Type::U8)} == "u8");
    CHECK(std::string{to_string(Type::I16)} == "i16");
    CHECK(std::string{to_string(Type::U16)} == "u16");
    CHECK(std::string{to_string(Type::I32)} == "i32");
    CHECK(std::string{to_string(Type::U32)} == "u32");
    CHECK(std::string{to_string(Type::I64)} == "i64");
    CHECK(std::string{to_string(Type::U64)} == "u64");
    CHECK(std::string{to_string(Type::Void)} == "void");
    // The 16-bit types are 2 bytes, the 64-bit ones 8, and `void` is no cell.
    CHECK(size_of(Type::I16) == 2);
    CHECK(size_of(Type::U16) == 2);
    CHECK(size_of(Type::U64) == 8);
    CHECK(size_of(Type::Void) == 0);
    CHECK(size_of(Type::I8) == 1);
    CHECK(size_of(Type::I32) == 4);
    // The parser's own view of a type round-trips through the machine one -- except for
    // the two that are not integers here, since `lift` cannot tell `bool` from `i8`.
    for (const SemType type : {SemType::I8, SemType::U8, SemType::I16, SemType::U16, SemType::I32,
                               SemType::U32, SemType::I64, SemType::U64}) {
        CHECK(lift(lower(type)) == type);
    }
    CHECK(lift(lower(SemType::Bool)) == SemType::I8);
    CHECK(lift(lower(SemType::Char)) == SemType::U8);
    CHECK(lower(SemType::Void) == Type::Void);
    CHECK(lift(Type::Void) == SemType::Void);
    CHECK(std::string{to_string(SemType::I64)} == "i64");
    CHECK(std::string{to_string(SemType::U64)} == "u64");
    CHECK(std::string{to_string(SemType::Bool)} == "bool");
    CHECK(std::string{to_string(SemType::Char)} == "char");
    CHECK(std::string{to_string(SemType::Void)} == "void");
    // `bool` and `char` are not interchangeable with the `i8` and `u8` they are.
    CHECK(lower(SemType::Bool) == Type::I8);
    CHECK(lower(SemType::Char) == Type::U8);
    CHECK(is_integer(SemType::Char));
    CHECK(is_integer(SemType::I8));
    CHECK(!is_integer(SemType::Bool));
    CHECK(!is_integer(SemType::Void));
    CHECK(is_signed(SemType::I32));
    CHECK(!is_signed(SemType::U32));
    CHECK(!is_signed(SemType::Char));
}

TEST_CASE("a call checks the type of every argument") {
    // An argument of the wrong type is named, with its position.
    CHECK(parse_error("fn f(a: i32): i32 { return a; } fn main(): i32 { let x: u32 = 1; return f(x); }") ==
          "argument 1 of 'f' is i32, not u32");
    CHECK(parse_error("fn f(a: i32, b: i32): i32 { return a; } fn main(): i32 { return f(1, true); }") != "");
    // A `bool` argument to an `i8` parameter is the same rule: `i8` is not `bool`.
    CHECK(parse_error("fn f(a: i8): i8 { return a; } fn main(): i32 { return f(true); }") != "");
    // An argument that does not fit its parameter is a constant that does not fit.
    CHECK(parse_error("fn f(a: u8): u8 { return a; } fn main(): i32 { return f(256); }") != "");
    // A call with no arguments, and one with the last argument optional, both parse.
    parse_ok("fn f() { } fn main(): i32 { f(); return 0; }");
    parse_ok("fn f(a: i32) { } fn main(): i32 { f(1); return 0; }");
    // The closing paren is required.
    CHECK(parse_error("fn f(a: i32): i32 { return a; } fn main(): i32 { return f(1; }") ==
          "expected ')' after the arguments of 'f'");
    // A callee is a name, so a literal is not one.
    CHECK(parse_error("fn main(): i32 { return 1(2); }") == "expected ';'");
    CHECK(parse_error("fn main(): i32 { return 1(); }") == "expected ';'");
    // A function name is not a value.
    CHECK(parse_error("fn f(): i32 { return 0; } fn main(): i32 { let x: i32 = f; return x; }") ==
          "'f' is a function and can only be called");
    // A parenthesized expression is the expression in it, and needs its paren back.
    const Program program = parse_ok("fn main(): i32 { let x: i32 = (1 + 2); return x; }");
    CHECK(commands_of(program, 0)[0].args[1].value == 3);
    CHECK(parse_error("fn main(): i32 { let x: i32 = (1 + 2; return x; }") == "expected ')'");
    CHECK(parse_error("fn main(): i32 { let x: i32 = (); return x; }") == "expected an expression");
    // An expression is required where one belongs.
    CHECK(parse_error("fn main(): i32 { let x: i32 = ; return 0; }") == "expected an expression");
    CHECK(parse_error("fn main(): i32 { return 1 + ; }") == "expected an expression");
    CHECK(parse_error("fn main(): i32 { return -; }") == "expected an expression");
    // ... and a `void` call is not an expression either.
    CHECK(parse_error("fn f() { } fn main(): i32 { let x: i32 = f(); return 0; }") != "");
    // A name is one identifier: a dot is a namespace, and v1 has none.
    CHECK(parse_error("fn main(): i32 { return a.b(); }") == "'.' is not part of a name in v1");
    CHECK(parse_error("fn main(): i32 { let a: i32 = 1; return a.b; }") == "'.' is not part of a name in v1");
    CHECK(parse_error("fn a.b(): i32 { return 0; } fn main(): i32 { return 0; }") == "expected '('");
}
