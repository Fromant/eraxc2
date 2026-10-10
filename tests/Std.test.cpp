#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "StdSource.hpp"
#include "StdTestSource.hpp"
#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "interp/Interpreter.hpp"

using erax::frontend::ProgramParser;
using erax::frontend::Tokenizer;
using erax::interp::Outcome;

namespace {

  /// Lowers std plus \p source exactly the way the driver auto-imports
  /// (`docs/std.md`): std's tokens first, its own end-of-file dropped, then the
  /// program's — one token stream, one parse.
  ProgramParser::result parse_with_std(std::string_view source) {
    auto imported = Tokenizer {}.tokenize(erax::std_source, "std.erx");
    REQUIRE(imported.has_value());
    auto tokens = Tokenizer {}.tokenize(source, "test.erx");
    REQUIRE(tokens.has_value());
    std::vector<erax::frontend::Token> all;
    all.insert(all.end(), imported->begin(), std::prev(imported->end()));
    all.insert(all.end(), tokens->begin(), tokens->end());
    return ProgramParser {all}.parse();
  }

  std::expected<Outcome, erax::interp::Failure> run_with_std(std::string_view source) {
    auto program = parse_with_std(source);
    REQUIRE(program.has_value());
    return erax::interp::run(*program);
  }

  /// Lowers std plus the checked-in std.test.erx, mirroring parse_with_std's
  /// concatenation: std's tokens first, the self-test's, one parse.
  std::expected<Outcome, erax::interp::Failure> run_std_test_suite() {
    auto imported = Tokenizer {}.tokenize(erax::std_source, "std.erx");
    REQUIRE(imported.has_value());
    auto tests = Tokenizer {}.tokenize(erax::std_test_source, "std.test.erx");
    REQUIRE(tests.has_value());
    std::vector<erax::frontend::Token> all;
    all.insert(all.end(), imported->begin(), std::prev(imported->end()));
    all.insert(all.end(), tests->begin(), tests->end());
    auto program = ProgramParser {all}.parse();
    REQUIRE(program.has_value());
    return erax::interp::run(*program);
  }

}  // namespace

TEST_CASE("std parses on its own: no globals, no main, closed") {
  auto tokens = Tokenizer {}.tokenize(erax::std_source, "std.erx");
  REQUIRE(tokens.has_value());
  // The library alone is not a program — no entrypoint — but every function in
  // it resolves: primitives and its own calls, nothing else.
  auto program = ProgramParser {*tokens}.parse();
  REQUIRE_FALSE(program.has_value());
  CHECK(program.error().message == "the program must declare 'fn main(): i32'");
}

TEST_CASE("std's names live in namespace std, and the root is their own") {
  // std's declarations are inside `namespace std`, so a program may declare the
  // same name at the root — no clash where a flat namespace would clash.
  CHECK(parse_with_std("fn abs<T extends Ordered & Negatable>(x: T): T { return x; } "
                       "fn main(): i32 { return 0; }")
            .has_value());
  CHECK(parse_with_std("fn assert(cond: bool) { } fn main(): i32 { return 0; }").has_value());
  CHECK(parse_with_std("let gcd: i32 = 1; fn main(): i32 { return 0; }").has_value());
  // The root and std do not see each other's bare names: `std` only by its
  // qualified path, and no bare `print` exists at the root.
  CHECK(run_with_std("fn main(): i32 { std.print('A'); return 0; }")->output == "A");
  CHECK(parse_with_std("fn main(): i32 { print(1); return 0; }").has_value() == false);
  // A namespace is not reopened: auto-import already registered `std`.
  CHECK(parse_with_std("namespace std { } fn main(): i32 { return 0; }").has_value() == false);
  // An ordinary name still declares fine next to std's.
  CHECK(parse_with_std("fn helper(): i32 { return 1; } fn main(): i32 { return helper(); }").has_value());
}

TEST_CASE("std's output writes the number or the word per type") {
  // One name: the template deduces the declaration whose parameter matches the
  // argument's type — literals default to i32, so the wide ones carry their
  // suffix.
  CHECK(run_with_std("fn main(): i32 { std.print(-42); return 0; }")->output == "-42");
  CHECK(run_with_std("fn main(): i32 { std.print(9223372036854775807i64); return 0; }")->output ==
        "9223372036854775807");
  CHECK(run_with_std("fn main(): i32 { std.print(18446744073709551615u64); return 0; }")->output ==
        "18446744073709551615");
  // Narrow widths deduce their own type, and print through the same core.
  CHECK(run_with_std("fn main(): i32 { let lo: i8 = -128; std.print(lo); return 0; }")->output == "-128");
  CHECK(run_with_std("fn main(): i32 { let hi: u8 = 255; std.print(hi); return 0; }")->output == "255");
  // i64min prints as itself: the sign, then its magnitude as a wrapping `u64`.
  CHECK(run_with_std("fn main(): i32 { let imin: i64 = -9223372036854775808; std.print(imin); return 0; }")->output ==
        "-9223372036854775808");
  CHECK(run_with_std("fn main(): i32 { std.print(true); std.print(false); return 0; }")->output == "truefalse");
  CHECK(run_with_std("fn main(): i32 { std.print('A'); return 0; }")->output == "A");
  // The zero-argument print is one newline.
  CHECK(run_with_std("fn main(): i32 { std.print(1); std.print(); std.print(true); return 0; }")->output == "1\ntrue");
  CHECK(run_with_std("fn main(): i32 { std.print(' '); std.print('\\n'); return 0; }")->output == " \n");
}

TEST_CASE("std classifies and converts characters") {
  CHECK(run_with_std("fn main(): i32 { std.print(std.is_digit('5')); std.print(std.is_digit('x')); return 0; }")
            ->output == "truefalse");
  CHECK(run_with_std("fn main(): i32 { std.print(std.is_alpha('z')); std.print(std.is_alpha('0')); return 0; }")
            ->output == "truefalse");
  CHECK(run_with_std("fn main(): i32 { std.print(std.is_alnum('7')); std.print(std.is_alnum('-')); return 0; }")
            ->output == "truefalse");
  CHECK(run_with_std("fn main(): i32 { std.print(std.is_upper('Q')); std.print(std.is_lower('q')); return 0; }")
            ->output == "truetrue");
  // to_digit is the digit's value, or -1 when it is not one — computed by `as`.
  CHECK(run_with_std(
            "fn main(): i32 { std.print(std.to_digit('7')); std.print(' '); std.print(std.to_digit('x')); return 0; }")
            ->output == "7 -1");
  CHECK(run_with_std("fn main(): i32 { std.print(std.to_digit('0')); return 0; }")->output == "0");
  // Case conversion leaves anything else alone.
  CHECK(run_with_std("fn main(): i32 { primitive print_char(std.to_upper('q')); "
                     "primitive print_char(std.to_lower('W')); primitive print_char(std.to_upper('3')); return 0; }")
            ->output == "Qw3");
}

TEST_CASE("std's math is the language's wrapping arithmetic") {
  CHECK(run_with_std("fn main(): i32 { std.print(std.abs(-99)); std.print(' '); std.print(std.abs(4)); return 0; }")
            ->output == "99 4");
  CHECK(run_with_std("fn main(): i32 { std.print(std.min(3, -2)); std.print(' '); std.print(std.max(3, -2)); "
                     "return 0; }")
            ->output == "-2 3");
  CHECK(run_with_std("fn main(): i32 { std.print(std.min(3u64, 2u64)); std.print(' '); "
                     "std.print(std.max(3u64, 2u64)); return 0; }")
            ->output == "2 3");
  // pow is exponentiation by squaring in wrapping u64: 2^10, 10^0, 0^0 = 1.
  CHECK(run_with_std("fn main(): i32 { std.print(std.pow(2, 10)); std.print(' '); std.print(std.pow(10, 0)); "
                     "std.print(' '); std.print(std.pow(0, 0)); return 0; }")
            ->output == "1024 1 1");
  // Past u64max the product wraps, exactly as `*` on two u64 does.
  CHECK(run_with_std("fn main(): i32 { std.print(std.pow(2, 64)); return 0; }")->output == "0");
  CHECK(run_with_std("fn main(): i32 { std.print(std.pow(2, 65)); return 0; }")->output == "0");
  CHECK(run_with_std("fn main(): i32 { std.print(std.pow(3, 5)); return 0; }")->output == "243");
  // gcd is Euclid, and gcd(0, 0) = 0 as the doc says.
  CHECK(run_with_std("fn main(): i32 { std.print(std.gcd(48, 18)); std.print(' '); std.print(std.gcd(0, 0)); "
                     "std.print(' '); std.print(std.gcd(7, 0)); return 0; }")
            ->output == "6 0 7");
}

TEST_CASE("std's assert aborts on a false condition and nothing else") {
  // A true condition writes nothing and changes nothing.
  CHECK(run_with_std("fn main(): i32 { std.assert(true); return 5; }")->code == 5);
  // A false one is the abort code, 134, with no message of its own.
  const auto outcome = run_with_std("fn main(): i32 { std.assert(false); return 0; }");
  REQUIRE(outcome.has_value());
  CHECK(outcome->code == 134);
  CHECK(outcome->output.empty());
  // assert takes a bool — a comparison, not a number.
  CHECK(run_with_std("fn main(): i32 { std.assert(1 == 1); return 0; }")->code == 0);
}

TEST_CASE("std has no privileges: user code composes the same way") {
  // A program writes its own spacing around std's pieces, exactly as std.md
  // says composition is the caller's.
  CHECK(run_with_std("fn main(): i32 { std.print(1); std.print(' '); std.print(true); std.print('\\n'); return 0; }")
            ->output == "1 true\n");
  // And std functions call each other — is_alnum is is_alpha || is_digit.
  CHECK(run_with_std("fn main(): i32 { std.print(std.is_alnum('a')); return 0; }")->output == "true");
}

TEST_CASE("std.test.erx runs to a zero-failure tally") {
  // The self-test is ordinary erax, so the interpreter runs it like any
  // program: `main` returns the failed count — 0 here — and the tally line
  // state says checks <checks> then <failed>. The pinned count is 31, so a
  // dropped check below changes this test.
  const auto outcome = run_std_test_suite();
  REQUIRE(outcome.has_value());
  CHECK(outcome->code == 0);
  CHECK(outcome->output == "-42 9223372036854775807 18446744073709551615 true A\n34 0\n");
}