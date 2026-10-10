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
    if (type == "i8" || type == "u8" || type == "char" || type == "bool") {
      return 8;
    }
    if (type == "i16" || type == "u16") {
      return 16;
    }
    if (type == "i32" || type == "u32") {
      return 32;
    }
    return 64;
  }

  /// The single folded initializer of `let x: T = <expression>;`, as raw bits.
  std::uint64_t folded_bits(std::string_view expression, std::string_view type = "i32") {
    const Program program =
        parse_ok("fn main(): i32 { let x: " + std::string {type} + " = " + std::string {expression} + "; return 0; }");
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
    const std::uint64_t mask = width == 64 ? ~std::uint64_t {0} : (std::uint64_t {1} << width) - 1;
    if ((bits & (std::uint64_t {1} << (width - 1))) == 0) {
      return static_cast<std::int64_t>(bits);
    }
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

TEST_CASE("the widest literals are values of the type that names them") {
  // The position types a lone literal, and the position is the widest one there is.
  CHECK(folded_bits("9223372036854775807", "i64") == 9223372036854775807ULL);
  // `u64max` is a value of `u64` and no narrower type, so it needs a `u64` suffix to
  // be read at all: without one it is a lone literal in a position that types it.
  CHECK(folded_bits("18446744073709551615u64", "u64") == 18446744073709551615ULL);
  CHECK(parse_error("fn main(): i32 { let x: i64 = 18446744073709551615; return 0; }") ==
        "'18446744073709551615' does not fit i64");
  CHECK(parse_error("fn main(): i32 { let x: u32 = 4294967296; return 0; }") == "'4294967296' does not fit u32");
  CHECK(folded_bits("4294967296", "u64") == 4294967296ULL);
  CHECK(folded_bits("4294967296", "i64") == 4294967296ULL);
  // A suffix names the type instead of the position, which is how the values no
  // signed literal reaches are written: `2^63` is a `u64` and is written as one.
  CHECK(folded_bits("9223372036854775808u64", "u64") == 0x8000000000000000ULL);
  // A minimum is spelled under a unary `-`, and the literal there is the magnitude.
  CHECK(folded_signed("-9223372036854775808i64", "i64") == -9223372036854775807LL - 1);
  CHECK(folded_signed("-2147483648i32", "i32") == -2147483647LL - 1);
  CHECK(folded_signed("-128i8", "i8") == -128);
  CHECK(folded_bits("-128i8", "i8") == 0x80);
  // An unsigned type has no negative side, so `-` is not defined on one at all.
  CHECK(parse_error("fn main(): i32 { let x: u8 = -1; return 0; }") == "'-' is not defined for u8");
  // A suffix does not smuggle the sign past that: the suffix names the type the
  // literal is read in, and `-1i8` is still a negation of an `i8` value.
  CHECK(parse_error("fn main(): i32 { let x: u8 = -1i8; return 0; }") == "'-' is not defined for u8");
  // A magnitude that needs the negative half and does not have it is out of range,
  // not a truncation: `200` is not `imin` in an `i8`.
  CHECK(parse_error("fn main(): i32 { let x: i8 = -200; return 0; }") == "'200' does not fit i8");
  // The expected type threads to arithmetic operators, so `-128 + 0` folds in `i8`
  // and `-128` fits `i8` (it is `i8min`).
  CHECK(folded_signed("-128 + 0", "i8") == -128);
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
  CHECK(any_error("fn main(): i32 { let c: char = '\\q'; return 0; }") == "invalid escape sequence '\\q'");
  // The digit count is the parser's rule, because the tokenizer keeps the escape
  // text raw and cannot know which of the two forms it is in.
  CHECK(parse_error("fn main(): i32 { let c: char = '\\x4'; return 0; }") == "expected 2 hexadecimal digits after \\x");
  CHECK(parse_error("fn main(): i32 { let c: char = '\\u041'; return 0; }") ==
        "expected 4 hexadecimal digits after \\u");
  CHECK(parse_error("fn main(): i32 { let c: char = '\\x412'; return 0; }") != "");
  // A `char` is a byte, so a bare `char` literal is its own code point, and it is
  // the only type a char literal can fill: it is not an integer literal.
  CHECK(folded_bits("'A'", "char") == 65);
  CHECK(parse_error("fn main(): i32 { let c: i8 = 'A'; return 0; }") == "expected a value of type i8, not char");
  CHECK(parse_error("fn main(): i32 { let c: u8 = '\\u0100'; return 0; }") != "");
}

TEST_CASE("a constant expression is exact, and its value must fit") {
  // The value has to be representable in the type it takes, which is not the same
  // thing as its bits fitting: 256 is a perfectly good 0 in eight bits.
  CHECK(folded_bits("127i8", "i8") == 127);
  CHECK(parse_error("fn main(): i32 { let x: i8 = 128; return 0; }") == "'128' does not fit i8");
  CHECK(parse_error("fn main(): i32 { let x: u8 = 255u8 + 1u8; return 0; }") == "this constant does not fit u8");
  CHECK(parse_error("fn main(): i32 { let x: i32 = 2147483647i32 + 1i32; return 0; }") ==
        "this constant does not fit i32");
  // A negative value is not an unsigned one, even where its bits would fit.
  CHECK(parse_error("fn main(): i32 { let x: u8 = -1; return 0; }") == "'-' is not defined for u8");
  // The whole expression is checked, not each literal in it: a literal under a binary
  // operator is typed by its neighbour, and the fold is in that type.
  CHECK(folded_bits("100i8 + 27i8", "i8") == 127);
  CHECK(parse_error("fn main(): i32 { let x: i8 = 100i8 + 28i8; return 0; }") == "this constant does not fit i8");
  // ... and the expected type now threads to arithmetic operators, so unsuffixed
  // literals take the context type and fold in it.
  CHECK(folded_bits("100 + 27", "i8") == 127);
  // A parenthesized expression is the expression in it, so it takes the type too.
  parse_ok("fn main(): i32 { let x: i8 = (1i8 + 2i8); return 0; }");
  CHECK(parse_error("fn main(): i32 { let x: i8 = (127i8 + 2i8); return 0; }") == "this constant does not fit i8");
  CHECK(folded_bits("0xF0u8 | 0x0Fu8", "u8") == 0xFF);
  CHECK(folded_bits("0xFFu8 & 0x0Fu8", "u8") == 0x0F);
  CHECK(folded_bits("0xFFu8 ^ 0x0Fu8", "u8") == 0xF0);
  CHECK(folded_signed("-7i32 / 2i32") == -3);
  CHECK(folded_signed("-7i32 % 2i32") == -1);
  CHECK(folded_bits("255u8 / 16u8", "u8") == 15);
  CHECK(folded_bits("255u8 % 16u8", "u8") == 15);
  CHECK(folded_signed("100i32 * -3i32") == -300);
  // Division and modulus by zero have no answer at all, typed or not.
  CHECK(parse_error("fn main(): i32 { let x: i32 = 1i32 / 0i32; return 0; }") ==
        "division by zero in a constant expression");
  CHECK(parse_error("fn main(): i32 { let x: u32 = 1u32 % 0u32; return 0; }") ==
        "division by zero in a constant expression");
  // A negation and a `~` keep the value exact, so they are range-checked like one.
  // `i32min` is a constant, so negating it overflows at parse time...
  CHECK(parse_error("fn main(): i32 { let x: i32 = -(-2147483647i32 - 1i32); return 0; }") ==
        "this constant does not fit i32");
  CHECK(folded_signed("~5i32") == -6);
  CHECK(parse_error("fn main(): i32 { let x: i8 = -(-127i8 - 1i8); return 0; }") == "this constant does not fit i8");
  // ... but negating a value read at runtime only wraps, which is the JIR's rule.
  parse_ok("fn main(): i32 { let x: i8 = -128; let y: i8 = -x; return 0; }");
  // `~` is the same on both signednesses and never leaves the range, so on an
  // unsigned one it is 255 and not an error.
  CHECK(folded_bits("~0u8", "u8") == 255);
  // A shift count masks to the width the fold is done in, so a constant shifted past
  // its own width is a shift of the count that is left, not an overflow.
  CHECK(folded_bits("1i64 << 66i64", "i64") == 4);
  CHECK(folded_bits("1i64 << 62i64", "i64") == 1ULL << 62);
  CHECK(folded_bits("1i64 << 40i64", "i64") == 1ULL << 40);
  CHECK(folded_signed("-8i32 >> 1i32") == -4);
  CHECK(folded_signed("-1i32 >> 1i32") == -1);
  CHECK(folded_bits("0i32 << 31i32", "i32") == 0);
  CHECK(folded_bits("7i32 << 0i32", "i32") == 7);
  // An unsigned right shift is logical, and a `char` is unsigned -- but a `char` is
  // its own type, so it is the `u8` suffix in a `u8` position that says so here.
  CHECK(folded_bits("255u8 >> 4u8", "u8") == 15);
  // A product that leaves the width the expression is checked in is not a constant
  // that can be used there, but the same product in a wider type is.
  CHECK(folded_bits("65536i64 * 65536i64", "i64") == 4294967296ULL);
  CHECK(parse_error("fn main(): i32 { let x: i32 = 65536i32 * 65536i32; return 0; }") ==
        "this constant does not fit i32");
}

TEST_CASE("a constant that outgrows every type says so rather than wrapping") {
  // A constant is exact arithmetic on numbers, so the only thing that can happen to
  // one besides a value is a magnitude past `u64max` -- and there is no type to name
  // in the error. Wrapping it would produce a number that fits everywhere, which is
  // exactly the silent mistake the exactness is there to prevent.
  //
  // 2^32 * 2^32 is 2^64, which is one past the widest type there is.
  CHECK(parse_error("fn main(): i32 { let x: u64 = 4294967296u64 * 4294967296u64; return 0; }") ==
        "this constant expression is too large for any type");
  // The type of the position cannot rescue it: there is no type to name in the error,
  // and asking for the widest one does not make it fit.
  CHECK(parse_error("fn main(): i32 { let x: i64 = 4294967296u64 * 4294967296u64; return 0; }") ==
        "'4294967296u64' does not fit i64");
  // A sum can outgrow a `u64` as well as a product: `u64max + 1` is `2^64`.
  CHECK(parse_error("fn main(): i32 { let x: u64 = 18446744073709551615u64 + 1u64; return 0; }") ==
        "this constant expression is too large for any type");
  // One factor lower and it is `2^63`, which is a `u64` value and no signed one.
  CHECK(folded_bits("4294967296u64 * 2147483648u64", "u64") == 0x8000000000000000ULL);
  // With i64 context, the u64 suffix mismatches the expected type.
  CHECK(parse_error("fn main(): i32 { let x: i64 = 4294967296u64 * 2147483648u64; return 0; }") ==
        "'4294967296u64' does not fit i64");
  // 3037000500 squared is 2^63-ish, past `i64max` but inside `u64max`, so it is a
  // `u64` constant and the position has to be a `u64` to hold it.
  CHECK(folded_bits("3037000500u64 * 3037000500u64", "u64") == 9223372037000250000ULL);
  CHECK(parse_error("fn main(): i32 { let x: i64 = 3037000500u64 * 3037000500u64; return 0; }") ==
        "'3037000500u64' does not fit i64");
  // The signed product that stays in range is an ordinary constant.
  CHECK(folded_signed("1000i64 * -1000i64", "i64") == -1000000);

  // `i64min` is a value, so the sum that produces it is not a constant overflow but
  // an exact one, and one step further down is out of every type's range.
  CHECK(folded_signed("-9223372036854775807i64 - 1i64", "i64") == -9223372036854775807LL - 1);
  CHECK(parse_error("fn main(): i32 { let x: i64 = -9223372036854775807i64 - 1i64 - 1i64; return 0; }") ==
        "this constant does not fit i64");
  // `imin / -1` is `2^(w-1)`, which the type does not hold, so the fold reports it
  // rather than producing the number the machine cannot.
  CHECK(parse_error("fn main(): i32 { let x: i64 = ((-9223372036854775807i64) - 1i64) / -1i64; return 0; }") ==
        "this constant does not fit i64");
  // With i64 context, the unsuffixed literals take i64 and the result fits.
  CHECK(folded_signed("-2147483647 - 1", "i64") == -2147483648LL);
  CHECK(folded_signed("-2147483647i32 - 1i32", "i32") == -2147483648LL);
  // Read at runtime it is the machine's problem and wraps instead.
  parse_ok("fn main(): i32 { let x: i64 = -9223372036854775807i64 - 1i64; let y: i64 = x / -1; return 0; }");
}

TEST_CASE("two exact values are compared as numbers, not as patterns") {
  // Read as bits, `-1` is `0xFFFF...` and the largest value there is, so every one
  // of these folded the wrong way round. The bits are a `u64` until an operator needs
  // to know the sign, and a comparison does.
  CHECK(folded_bits("-1i32 < 1i32", "bool") == 1);
  CHECK(folded_bits("-1i32 > 1i32", "bool") == 0);
  CHECK(folded_bits("-1i32 >= 1i32", "bool") == 0);
  CHECK(folded_bits("1i32 > -1i32", "bool") == 1);
  CHECK(folded_bits("-1i32 == -1i32", "bool") == 1);
  CHECK(folded_bits("-1i32 != -2i32", "bool") == 1);
  CHECK(folded_bits("-2i32 <= -1i32", "bool") == 1);
  CHECK(folded_bits("-1i32 <= -1i32", "bool") == 1);
  // A negative against a positive, in both orders, at more than one width.
  CHECK(folded_bits("-128i8 < 1i8", "bool") == 1);
  CHECK(folded_bits("-32768i16 < 1i16", "bool") == 1);
  CHECK(folded_bits("-1i64 < 2147483648i64", "bool") == 1);
  // Two negatives order by magnitude, and `-1` is the largest.
  CHECK(folded_bits("-1i32 < -2i32", "bool") == 0);
  CHECK(folded_bits("-2i32 < -1i32", "bool") == 1);
  // A folded test takes the edge it names, and drops the other. A true condition
  // keeps the then-block and loses the code after it; a false one is the reverse.
  // The only difference left is which constant the surviving `return` carries.
  const auto ret_value = [](const Program& program) {
    for (const Command& command : commands_of(program, 0)) {
      if (command.op == Op::Ret && command.args[0].kind == OperandKind::Instant) {
        return command.args[0].value;
      }
    }
    return ~std::uint64_t {0};
  };
  const Program taken = parse_ok("fn main(): i32 { if (-1i32 < 1i32) { return 1; } return 0; }");
  CHECK(ret_value(taken) == 1);
  CHECK(count_of(ops_of(taken, 0), Op::Ret) == 1);
  const Program skipped = parse_ok("fn main(): i32 { if (-1i32 > 1i32) { return 1; } return 0; }");
  CHECK(ret_value(skipped) == 0);
  CHECK(count_of(ops_of(skipped, 0), Op::Ret) == 1);
  // A genuinely unsigned operand is compared as unsigned, and because `a` and `b` are
  // variables and not constants the test is not folded: both edges stay and the
  // comparison is emitted as a command against the `u8` cells.
  const Program unsigned_gt =
      parse_ok("fn main(): i32 { let a: u8 = 255; let b: u8 = 1; if (a > b) { return 1; } return 0; }");
  CHECK(has_op(ops_of(unsigned_gt, 0), Op::CmpGt));
  CHECK(count_of(ops_of(unsigned_gt, 0), Op::Ret) == 2);
  // A `u64` above `i64max` has no signed reading, so it compares as the `u64` it is.
  CHECK(folded_bits("1u64 << 63u64 > 0u64", "bool") == 1);
  CHECK(folded_bits("1u64 << 63u64 < 0u64", "bool") == 0);
}

TEST_CASE("the logical operators are on bool, and eager") {
  // `^^` is exclusive or, and it is the only operator that needs two different
  // operators' definitions at once: both `&&`/`||` and `^^` on `bool`, and `&&`,
  // `||`, `^^` on integers as bitwise and/or/xor.
  CHECK(folded_bits("true ^^ true", "bool") == 0);
  CHECK(folded_bits("true ^^ false", "bool") == 1);
  CHECK(folded_bits("false ^^ false", "bool") == 0);
  CHECK(folded_bits("true && true", "bool") == 1);
  CHECK(folded_bits("true && false", "bool") == 0);
  CHECK(folded_bits("false || true", "bool") == 1);
  CHECK(folded_bits("false || false", "bool") == 0);
  // On integers they are the bitwise operators, so the names overlap but the
  // spelling does not: `^` is xor and `^^` is not an operator at all.
  CHECK(parse_error("fn main(): i32 { let x: i8 = 6 ^^ 3; return 0; }") == "'^^' is defined on 'bool' operands");
  // A comparison result is a `bool`, so it is an operand of these.
  CHECK(folded_bits("(1 < 2) ^^ (2 < 1)", "bool") == 1);
  // They are eager: both sides are evaluated whatever the left one says, so there
  // is no constant to fold away on the strength of the left alone.
  const Program eager = parse_ok("fn main(): i32 { let a: i32 = 1; if (false && a > 0) { return 1; } return 0; }");
  CHECK(count_of(ops_of(eager, 0), Op::Ret) == 2);
  // ... and `false && x` is not `false` at compile time, so the test is not folded.
  const Program not_folded = parse_ok("fn main(): i32 { let a: i32 = 1; if (false && a > 0) { return 1; } return 0; }");
  CHECK(has_op(ops_of(not_folded, 0), Op::And));
}

TEST_CASE("an unsigned division is unsigned throughout") {
  // The signed and unsigned forms differ, so the operand's type is what picks one,
  // and a `u64` above `i64max` is the case where the difference is visible: read as
  // `u64` it is the largest value there is, read as `i64` it is -1.
  CHECK(folded_bits("18446744073709551615u64 / 2u64", "u64") == 9223372036854775807ULL);
  CHECK(folded_bits("18446744073709551615u64 % 2u64", "u64") == 1);
  // A signed position is a different number, and is a value.
  CHECK(folded_signed("-1i32 / 2i32") == 0);
  CHECK(folded_signed("-1i32 % 2i32") == -1);
  // Every unsigned width divides as itself, with no sign to carry.
  CHECK(folded_bits("255u8 / 7u8", "u8") == 36);
  CHECK(folded_bits("255u8 % 7u8", "u8") == 3);
  CHECK(folded_bits("65535u16 / 255u16", "u16") == 257);
  CHECK(folded_bits("4294967295u32 / 65536u32", "u32") == 65535);
  CHECK(folded_bits("0u32 / 5u32", "u32") == 0);
  CHECK(folded_bits("5u32 / 1u32", "u32") == 5);
  // A `char` is a byte, so a division on `char` values is the unsigned one -- but a
  // `char` is a type of its own and there is no `char` suffix, so a *constant* `char`
  // division cannot be written: the operands would both be `i32`. Two `char` names
  // divide unsigned, which the test on the emitted command covers.
  parse_ok("fn main(): i32 { let a: char = 255; let b: char = 16; let c: char = a / b; return 0; }");
  // With char context, unsuffixed literals take char (u8) and fold in it.
  CHECK(folded_bits("255 / 16", "char") == 15);
  // An unsigned division by zero has no answer, and the message is the same one a
  // signed division gives: the divisor is zero whichever way it is read.
  CHECK(parse_error("fn main(): i32 { let x: u8 = 255u8 / 0u8; return 0; }") ==
        "division by zero in a constant expression");
  CHECK(parse_error("fn main(): i32 { let x: u8 = 255u8 % 0u8; return 0; }") ==
        "division by zero in a constant expression");
  // `imin / -1` is `2^(w-1)`, which the type does not hold, so the fold reports the
  // trap rather than the number the machine cannot.
  CHECK(parse_error("fn main(): i32 { let x: i64 = ((-9223372036854775807i64) - 1i64) / -1i64; return 0; }") ==
        "this constant does not fit i64");
}

TEST_CASE("a bool has no order, but it does have equality") {
  // `<`, `<=`, `>` and `>=` are defined on integers and chars. `bool` lowers to
  // `i8`, where the comparison would be perfectly well defined, so this is a rule
  // of the language and not something inherited from the machine.
  CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: bool = a < b; return 0; }") !=
        "");
  CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: bool = a <= b; return 0; }") !=
        "");
  CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: bool = a > b; return 0; }") !=
        "");
  CHECK(parse_error("fn main(): i32 { let a: bool = true; let b: bool = false; let c: bool = a >= b; return 0; }") !=
        "");
  // `==` and `!=` are defined on any two values of the same type, `bool` included.
  CHECK(folded_bits("true == false", "bool") == 0);
  CHECK(folded_bits("true != false", "bool") == 1);
  CHECK(folded_bits("true == true", "bool") == 1);
  CHECK(folded_bits("false != false", "bool") == 0);
  // A `char` is a byte and so does have an order, and it compares as an unsigned one.
  CHECK(folded_bits("'a' < 'b'", "bool") == 1);
  CHECK(folded_bits("'a' > 'b'", "bool") == 0);
  CHECK(folded_bits("'a' == 'a'", "bool") == 1);
  // A comparison of two ints is ordered, and produces a `bool` that is not an `i8`.
  CHECK(folded_bits("1 < 2", "bool") == 1);
  CHECK(folded_bits("2 <= 2", "bool") == 1);
  CHECK(folded_bits("3 > 4", "bool") == 0);
  CHECK(folded_bits("4 >= 4", "bool") == 1);
  // The result of a comparison is a `bool` and not an `i8`, so it does not fill an
  // `i32` -- the two are one cell in the IR and are not one type in the language.
  CHECK(parse_error("fn main(): i32 { let c: i32 = 1 < 2; return 0; }") != "");
  CHECK(parse_error("fn main(): i32 { let c: bool = 1 < 2; let d: i8 = c; return 0; }") != "");
}

TEST_CASE("a `u64` literal is a value of `u64` and of no other type") {
  // There is no bit-pattern state any more: `u64max` is a `u64`, and it is written
  // with the `u64` suffix because no signed literal reaches it.
  CHECK(folded_bits("18446744073709551615u64", "u64") == 0xFFFFFFFFFFFFFFFFULL);
  // Arithmetic on it is exact, so the two operations that leave the range are
  // errors rather than wrapped numbers.
  CHECK(parse_error("fn main(): i32 { let x: u64 = 18446744073709551615u64 + 1u64; return 0; }") ==
        "this constant expression is too large for any type");
  CHECK(parse_error("fn main(): i32 { let x: u64 = 18446744073709551615u64 * 2u64; return 0; }") ==
        "this constant expression is too large for any type");
  // A division it cannot do is still reported, and the message is the same one a
  // signed division gives.
  CHECK(parse_error("fn main(): i32 { let x: u64 = 18446744073709551615u64 / 0u64; return 0; }") ==
        "division by zero in a constant expression");
  // Division and modulus on it are the unsigned ones, so the answer is the large
  // one rather than the -1 the same bits would be read as in an `i64`.
  CHECK(folded_bits("18446744073709551615u64 / 1000u64", "u64") == 18446744073709551ULL);
  CHECK(folded_bits("18446744073709551615u64 % 1000u64", "u64") == 615);
  // A `u64` is not an `i64`, so the position has to be the one that holds it.
  CHECK(parse_error("fn main(): i32 { let x: i64 = 18446744073709551615u64; return 0; }") ==
        "'18446744073709551615u64' does not fit i64");
  // A comparison on it is the unsigned one, which is the same either way.
  CHECK(folded_bits("18446744073709551615u64 > 0u64", "bool") == 1);
  // A lone literal with no suffix takes the type of its position, so `4294967296` is an
  // `i64` in an `i64` position and out of range in every narrower one.
  CHECK(folded_bits("4294967296", "i64") == 4294967296ULL);
  CHECK(folded_bits("4294967296", "u64") == 4294967296ULL);
  CHECK(parse_error("fn main(): i32 { let x: u8 = 4294967296; return 0; }") == "'4294967296' does not fit u8");
  // With i64 context, unsuffixed literals take i64 and the arithmetic folds in it.
  CHECK(folded_bits("4294967296 + 1", "i64") == 4294967297ULL);
  CHECK(folded_bits("1 + 2", "i64") == 3);
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
  CHECK(parse_error("fn main(): i32 { let a: i32 = true; return 0; }") == "expected a value of type i32, not bool");
  CHECK(parse_error("fn main(): i32 { let a: char = true; return 0; }") != "");
  // An operator does not take the type of its position: its own type wins, which is
  // what keeps `1 < 2` a `bool`.
  parse_ok("fn main(): i32 { let a: bool = 1 < 2; return 0; }");
  parse_ok("fn main(): i32 { let a: i32 = 1 + 2; return a; }");
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1 < 2; return 0; }") == "expected a value of type i32, not bool");
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
    if (command.op != Op::Neg && command.op != Op::Not) {
      continue;
    }
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
  const Program program = parse_ok("let a: i8 = 1; let b: u8 = 1; let c: i16 = 1; let d: u16 = 1; let e: i32 = 1;"
                                   " let f: u32 = 1; let g: i64 = 1; let h: u64 = 1; fn main(): i32 { return 0; }");
  REQUIRE(program.globals.size() == 8);
  const std::vector<Type> expected {Type::I8,  Type::U8,  Type::I16, Type::U16,
                                    Type::I32, Type::U32, Type::I64, Type::U64};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(program.globals[index].type == expected[index]);
  }
  // A type name is printed the same way everywhere.
  CHECK(std::string {to_string(Type::I8)} == "i8");
  CHECK(std::string {to_string(Type::U8)} == "u8");
  CHECK(std::string {to_string(Type::I16)} == "i16");
  CHECK(std::string {to_string(Type::U16)} == "u16");
  CHECK(std::string {to_string(Type::I32)} == "i32");
  CHECK(std::string {to_string(Type::U32)} == "u32");
  CHECK(std::string {to_string(Type::I64)} == "i64");
  CHECK(std::string {to_string(Type::U64)} == "u64");
  CHECK(std::string {to_string(Type::Void)} == "void");
  // The 16-bit types are 2 bytes, the 64-bit ones 8, and `void` is no cell.
  CHECK(size_of(Type::I16) == 2);
  CHECK(size_of(Type::U16) == 2);
  CHECK(size_of(Type::U64) == 8);
  CHECK(size_of(Type::Void) == 0);
  CHECK(size_of(Type::I8) == 1);
  CHECK(size_of(Type::I32) == 4);
  // The parser's own view of a type round-trips through the machine one -- except for
  // the two that are not integers here, since `lift` cannot tell `bool` from `i8`.
  for (const SemType& type :
       {SemType::I8, SemType::U8, SemType::I16, SemType::U16, SemType::I32, SemType::U32, SemType::I64, SemType::U64}) {
    CHECK(lift(lower(type)) == type);
  }
  CHECK(lift(lower(SemType::Bool)) == SemType::I8);
  CHECK(lift(lower(SemType::Char)) == SemType::U8);
  CHECK(lower(SemType::Void) == Type::Void);
  CHECK(lift(Type::Void) == SemType::Void);
  CHECK(std::string {to_string(SemType::I64)} == "i64");
  CHECK(std::string {to_string(SemType::U64)} == "u64");
  CHECK(std::string {to_string(SemType::Bool)} == "bool");
  CHECK(std::string {to_string(SemType::Char)} == "char");
  CHECK(std::string {to_string(SemType::Void)} == "void");
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
  // A dotted name is a namespace-qualified reference; a dot that does not lead
  // to a namespace is the unknown-namespace error.
  CHECK(parse_error("fn main(): i32 { return a.b(); }") == "unknown namespace 'a'");
  CHECK(parse_error("fn main(): i32 { let a: i32 = 1; return a.b; }") == "unknown namespace 'a'");
  CHECK(parse_error("fn a.b(): i32 { return 0; } fn main(): i32 { return 0; }") == "expected '('");
}
