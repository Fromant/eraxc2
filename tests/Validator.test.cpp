#include "middleend/jir/Validator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "middleend/jir/Jir.hpp"

using namespace erax::middleend;

namespace {

  /// `RET 0`, the command that closes the entry block.
  Command ret() {
    return Command {Op::Ret, {Operand::instant(Type::I32, 0)}};
  }

  /// A program with just enough in it to be valid: one function, one block, and a
  /// `RET 0`. Every test here starts from this and breaks one rule, which is the
  /// only way to reach most of the validator -- the parser never emits an invalid
  /// program, and that is the parser's whole job.
  Program minimal() {
    Program program;
    Function main;
    main.return_type = Type::I32;
    main.frame_size = 1;
    main.cfg.blocks.resize(1);
    main.cfg.blocks[0].commands.push_back(ret());
    program.functions.push_back(std::move(main));
    return program;
  }

  /// The messages of every error. A test names the rule rather than the position,
  /// unless it is about the position.
  std::vector<std::string> messages(const Program& program) {
    std::vector<std::string> texts;
    for (const ValidationError& error : validate(program))
      texts.push_back(error.message);
    return texts;
  }

  /// Where each error is, for a test that is about the location.
  std::vector<std::string> locations(const Program& program) {
    std::vector<std::string> texts;
    for (const ValidationError& error : validate(program))
      texts.push_back(location_of(error));
    return texts;
  }

  /// The body of the entry block of `main`, keeping the `RET` that closes it, so a
  /// test breaks one rule rather than also leaving the block open.
  void body(Program& program, std::vector<Command> commands) {
    program.functions[0].cfg.blocks[0].commands = std::move(commands);
    program.functions[0].cfg.blocks[0].commands.push_back(ret());
  }

  /// Gives `main` parameters, which means it can no longer be the entrypoint, so a
  /// plain function stands in for it.
  void parameterized(Program& program, std::vector<Type> params, std::size_t frame) {
    program.functions[0].params = std::move(params);
    program.functions[0].frame_size = frame;
    Function entry;
    entry.return_type = Type::I32;
    entry.frame_size = 1;
    entry.cfg.blocks.resize(1);
    entry.cfg.blocks[0].commands.push_back(ret());
    program.functions.push_back(std::move(entry));
    program.entrypoint = program.functions.size() - 1;
  }

  /// A function `fn binary(a: i32, b: i32): i32 { return 0; }` as the IR, and the
  /// id it lands on.
  std::size_t add_binary(Program& program) {
    Function binary;
    binary.params = {Type::I32, Type::I32};
    binary.return_type = Type::I32;
    binary.frame_size = 3;
    binary.cfg.blocks.resize(1);
    binary.cfg.blocks[0].commands.push_back(ret());
    program.functions.push_back(std::move(binary));
    return program.functions.size() - 1;
  }

  /// A function `fn nothing(a: i32) { }` as the IR: one param, no result.
  std::size_t add_void(Program& program) {
    Function void_function;
    void_function.params = {Type::I32};
    void_function.return_type = Type::Void;
    void_function.frame_size = 2;
    void_function.cfg.blocks.resize(1);
    void_function.cfg.blocks[0].commands.push_back(Command {Op::Ret, {}});
    program.functions.push_back(std::move(void_function));
    return program.functions.size() - 1;
  }

}  // namespace

TEST_CASE("a program the parser could have emitted is valid") {
  // The parser is why most of these rules are unreachable from source, so the
  // corpus is the programs themselves: one of each shape the language has.
  const char* sources[] = {
      "fn main(): i32 { return 0; }",
      "let g: i32 = 7; fn twice(x: i32): i32 { return x + x; } fn main(): i32 { return twice(g); }",
      "fn log(x: i32) { } fn main(): i32 { log(1); return 0; }",
      "fn main(): i32 { let i: i32 = 0; while (i < 10) { i = i + 1; } return i; }",
      "fn main(): i32 { let i: i32 = 0; for (i = 0; i < 3; i = i + 1) { if (i > 1) { break; } } return i; }",
      "fn main(): i32 { let c: char = 'A'; let i: i32 = 0; while (i < 3) { c = c + 1; } return 0; }",
      "fn fact(n: i32): i32 { if (n < 1) { return 1; } return n * fact(n - 1); } fn main(): i32 { return fact(5); }",
  };
  for (const char* source : sources) {
    auto tokens = erax::frontend::Tokenizer {}.tokenize(source, "test.erx");
    REQUIRE(tokens.has_value());
    auto program = erax::frontend::ProgramParser {*tokens}.parse();
    REQUIRE(program.has_value());
    CHECK(validate(*program).empty());
  }
}

TEST_CASE("1. a CFG is entered at block 0 and reaches what it needs") {
  SECTION("a CFG with no blocks has no entry") {
    Program program = minimal();
    program.functions[0].cfg.blocks.clear();
    CHECK(messages(program) == std::vector<std::string> {"has no blocks, so it has no entry block"});
  }
  SECTION("an unreachable block is reported") {
    Program program = minimal();
    program.functions[0].cfg.blocks.resize(2);
    program.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(program) == std::vector<std::string> {"is not reachable from the entry block"});
  }
  SECTION("an edge to a block that does not exist is reported") {
    Program program = minimal();
    program.functions[0].cfg.blocks[0].commands.clear();
    program.functions[0].cfg.blocks[0].next = 4;
    CHECK(messages(program) == std::vector<std::string> {"its next block is block 4, which does not exist"});

    Program branch = minimal();
    branch.functions[0].cfg.blocks.resize(2);
    branch.functions[0].cfg.blocks[0].commands.clear();
    branch.functions[0].cfg.blocks[0].next = 1;
    branch.functions[0].cfg.blocks[0].branch =
        Branch {Condition {Op::CmpEq, Operand::instant(Type::I32, 0), Operand::instant(Type::I32, 0)}, 9};
    branch.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(branch) == std::vector<std::string> {"its branch target is block 9, which does not exist"});
  }
  SECTION("no path falls off the end, and RET ends a block") {
    Program falling = minimal();
    falling.functions[0].cfg.blocks[0].commands.clear();
    CHECK(messages(falling) ==
          std::vector<std::string> {
              "has no outgoing edge and does not end in RET or a terminating primitive, so a path falls off the end"});

    Program leaving = minimal();
    leaving.functions[0].cfg.blocks[0].next = 0;
    CHECK(messages(leaving) ==
          std::vector<std::string> {"ends in RET or a terminating primitive and also has an outgoing edge"});

    Program after = minimal();
    after.functions[0].cfg.blocks[0].commands = {
        ret(),
        Command {Op::Add,
                 {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 1)}}};
    CHECK(messages(after) ==
          std::vector<std::string> {
              "has no outgoing edge and does not end in RET or a terminating primitive, so a path falls off the end",
              "is not the last command in its block"});

    // A terminating `PRIM` ends a block exactly like `RET`: it is the last
    // command and the block takes no edges.
    Command exit = Command {Op::Primitive, {Operand::empty(), Operand::instant(Type::I32, 0)}, 3};
    Program prim_ok = minimal();
    prim_ok.functions[0].cfg.blocks[0].commands = {exit};
    CHECK(validate(prim_ok).empty());

    Program prim_leaving = minimal();
    prim_leaving.functions[0].cfg.blocks[0].commands = {exit};
    prim_leaving.functions[0].cfg.blocks[0].next = 0;
    CHECK(messages(prim_leaving) ==
          std::vector<std::string> {"ends in RET or a terminating primitive and also has an outgoing edge"});

    Program prim_after = minimal();
    prim_after.functions[0].cfg.blocks[0].commands = {exit, ret()};
    CHECK(messages(prim_after) == std::vector<std::string> {"is not the last command in its block"});

    // A `PRIM print` is an ordinary command: its block still needs an edge or
    // a close.
    Program prim_open = minimal();
    prim_open.functions[0].cfg.blocks[0].commands = {Command {Op::Primitive, {Operand::empty()}}};
    CHECK(messages(prim_open) ==
          std::vector<std::string> {
              "has no outgoing edge and does not end in RET or a terminating primitive, so a path falls off the end"});
  }
  SECTION("a branch test is a comparison on two values of one type") {
    Program not_compare = minimal();
    not_compare.functions[0].cfg.blocks.resize(2);
    not_compare.functions[0].cfg.blocks[0].commands.clear();
    not_compare.functions[0].cfg.blocks[0].next = 1;
    not_compare.functions[0].cfg.blocks[0].branch =
        Branch {Condition {Op::Add, Operand::instant(Type::I32, 0), Operand::instant(Type::I32, 0)}, 1};
    not_compare.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(not_compare) == std::vector<std::string> {"branches on ADD, which is not a comparison"});

    Program mixed = minimal();
    mixed.functions[0].cfg.blocks.resize(2);
    mixed.functions[0].cfg.blocks[0].commands.clear();
    mixed.functions[0].cfg.blocks[0].next = 1;
    mixed.functions[0].cfg.blocks[0].branch =
        Branch {Condition {Op::CmpEq, Operand::instant(Type::I32, 0), Operand::instant(Type::U32, 0)}, 1};
    mixed.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(mixed) == std::vector<std::string> {"branches on two types, i32 and u32"});
  }
  SECTION("the branch test holds a value") {
    Program empty = minimal();
    empty.functions[0].cfg.blocks.resize(2);
    empty.functions[0].cfg.blocks[0].commands.clear();
    empty.functions[0].cfg.blocks[0].next = 1;
    empty.functions[0].cfg.blocks[0].branch = Branch {Condition {Op::CmpEq, Operand::empty(), Operand::empty()}, 1};
    empty.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(empty) ==
          std::vector<std::string> {"the branch test is the EMPTY sentinel, which holds no value",
                                    "the branch test is the EMPTY sentinel, which holds no value"});

    Program checks = minimal();
    checks.functions[0].cfg.blocks.resize(2);
    checks.functions[0].cfg.blocks[0].commands.clear();
    checks.functions[0].cfg.blocks[0].next = 1;
    checks.functions[0].cfg.blocks[0].branch =
        Branch {Condition {Op::CmpEq, Operand::instant(Type::Void, 0), Operand::instant(Type::Void, 0)}, 1};
    checks.functions[0].cfg.blocks[1].commands = {ret()};
    CHECK(messages(checks) == std::vector<std::string> {"the branch test is void", "the branch test is void"});
  }
}

TEST_CASE("2. a command has the shape the op table gives it") {
  SECTION("the arity is the op's own") {
    Program program = minimal();
    body(program, {Command {Op::Add, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"ADD takes 3 arguments, but 2 were given"});

    Program call = minimal();
    body(call, {Command {Op::Call, {Operand::vreg(Type::I32, 0)}}});
    CHECK(messages(call) == std::vector<std::string> {"CALL takes at least 2 arguments, but 1 was given"});

    Program ret = minimal();
    ret.functions[0].cfg.blocks[0].commands = {
        Command {Op::Ret, {Operand::instant(Type::I32, 0), Operand::instant(Type::I32, 0)}}};
    CHECK(messages(ret) == std::vector<std::string> {"RET takes 0 or 1 arguments, but 2 were given"});
  }
  SECTION("a result is a vreg, and only MOV writes a global") {
    Program result = minimal();
    body(result,
         {Command {Op::Add,
                   {Operand::instant(Type::I32, 0), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(result) == std::vector<std::string> {"the result must be a vreg, but arg 0 is an instant"});

    Program mov = minimal();
    body(mov, {Command {Op::Mov, {Operand::instant(Type::I32, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(mov) == std::vector<std::string> {"MOV needs a vreg or a global to write, but arg 0 is an instant"});

    Program read_global = minimal();
    body(read_global,
         {Command {Op::Add,
                   {Operand::global(Type::I32, 0), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(read_global) == std::vector<std::string> {"the result must be a vreg, but arg 0 is a global"});

    Program into_global = minimal();
    into_global.globals.push_back(Global {Type::I32, Cfg {}});
    body(into_global, {Command {Op::Mov, {Operand::global(Type::I32, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(validate(into_global).empty());

    Program into_function = minimal();
    body(into_function, {Command {Op::Mov, {Operand::function(Type::I32, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(into_function) ==
          std::vector<std::string> {"MOV needs a vreg or a global to write, but arg 0 is a function"});
  }
  SECTION("one op is one type") {
    Program program = minimal();
    body(program,
         {Command {Op::Add,
                   {Operand::vreg(Type::I32, 0), Operand::instant(Type::U32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) ==
          std::vector<std::string> {"ADD needs one type throughout, but arg 1 is u32 and arg 2 is i32",
                                    "ADD needs one type throughout, but arg 1 is u32 and arg 0 is i32"});
  }
  SECTION("a cast is the one op whose operands are two types") {
    Program program = minimal();
    body(program, {Command {Op::Cast, {Operand::vreg(Type::I8, 0), Operand::instant(Type::I32, 12888)}}});
    CHECK(validate(program).empty());

    Program instant = minimal();
    body(instant, {Command {Op::Cast, {Operand::instant(Type::I8, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(instant) == std::vector<std::string> {"the result must be a vreg, but arg 0 is an instant"});

    Program void_source = minimal();
    body(void_source, {Command {Op::Cast, {Operand::vreg(Type::I32, 0), Operand::instant(Type::Void, 0)}}});
    CHECK(messages(void_source) == std::vector<std::string> {"the source is void"});
  }
  SECTION("a comparison answers in an i8") {
    Program program = minimal();
    body(program,
         {Command {Op::CmpEq,
                   {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"CMP_EQ needs arg 0 to be i8, but it is i32"});
  }
  SECTION("an op that names a signedness has operands that match") {
    Program wrong_signedness = minimal();
    body(wrong_signedness,
         {Command {Op::Sdiv,
                   {Operand::vreg(Type::U32, 0), Operand::instant(Type::U32, 6), Operand::instant(Type::U32, 3)}}});
    CHECK(messages(wrong_signedness) ==
          std::vector<std::string> {"SDIV needs signed operands, but arg 1 is u32",
                                    "SDIV needs signed operands, but arg 2 is u32"});

    Program wrong_unsigned = minimal();
    body(wrong_unsigned,
         {Command {Op::Umod,
                   {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 6), Operand::instant(Type::I32, 3)}}});
    CHECK(messages(wrong_unsigned) ==
          std::vector<std::string> {"UMOD needs unsigned operands, but arg 1 is i32",
                                    "UMOD needs unsigned operands, but arg 2 is i32"});

    // A shift is defined either way, so the signedness of its operands is not
    // something the op has an opinion about.
    Program shift = minimal();
    body(shift,
         {Command {Op::Shl,
                   {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 2)}}});
    CHECK(validate(shift).empty());
  }
  SECTION("the EMPTY sentinel is only a CALL or PRIM result") {
    Program program = minimal();
    body(program, {Command {Op::Add, {Operand::vreg(Type::I32, 0), Operand::empty(), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"an operand is the EMPTY sentinel, which holds no value"});

    Program in_return = minimal();
    in_return.functions[0].cfg.blocks[0].commands[0] = Command {Op::Ret, {Operand::empty()}};
    CHECK(messages(in_return) ==
          std::vector<std::string> {"the returned value is the EMPTY sentinel, which holds no value"});
  }
  SECTION("a function name is not a value") {
    Program program = minimal();
    body(program,
         {Command {Op::Add,
                   {Operand::vreg(Type::I32, 0), Operand::function(Type::I32, 0), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"an operand names a function rather than holding a value"});
  }
  SECTION("PRIM print takes whatever values it is given") {
    // Any value is printable — mixed types and none at all: a bare `print` is
    // just the newline, its result slot is `EMPTY`, and no two arguments share
    // a type.
    Program empty = minimal();
    body(empty, {Command {Op::Primitive, {Operand::empty()}}});
    CHECK(validate(empty).empty());

    Program mixed = minimal();
    body(mixed,
         {Command {Op::Primitive,
                   {Operand::empty(), Operand::instant(Type::I32, 1), Operand::instant(Type::U8, 2),
                    Operand::instant(Type::I8, 1), Operand::global(Type::I32, 0)}}});
    mixed.globals.push_back(Global {Type::I32, Cfg {}});
    CHECK(validate(mixed).empty());

    Program function_value = minimal();
    body(function_value, {Command {Op::Primitive, {Operand::empty(), Operand::function(Type::I32, 0)}}});
    CHECK(messages(function_value) ==
          std::vector<std::string> {"an argument to primitive print names a function rather than holding a value"});

    Program void_value = minimal();
    body(void_value, {Command {Op::Primitive, {Operand::empty(), Operand::instant(Type::Void, 0)}}});
    CHECK(messages(void_value) == std::vector<std::string> {"an argument to primitive print is void"});

    Program empty_value = minimal();
    body(empty_value, {Command {Op::Primitive, {Operand::empty(), Operand::empty()}}});
    CHECK(messages(empty_value) ==
          std::vector<std::string> {"an argument to primitive print is the EMPTY sentinel, which holds no value"});
  }
  SECTION("a PRIM's shape comes from the catalog") {
    Program unknown = minimal();
    body(unknown, {Command {Op::Primitive, {Operand::empty()}, 99}});
    CHECK(messages(unknown) == std::vector<std::string> {"PRIM names primitive 99, which does not exist"});

    // `exit` takes one i32 and leaves nothing behind. A terminating `PRIM`
    // is its block's last command, so these own the block whole.
    Program missing = minimal();
    missing.functions[0].cfg.blocks[0].commands = {Command {Op::Primitive, {Operand::empty()}, 3}};
    CHECK(messages(missing) == std::vector<std::string> {"gives 0 arguments to primitive exit, which takes 1"});

    Program wrong_argument = minimal();
    wrong_argument.functions[0].cfg.blocks[0].commands = {
        Command {Op::Primitive, {Operand::empty(), Operand::instant(Type::U32, 0)}, 3}};
    CHECK(messages(wrong_argument) == std::vector<std::string> {"PRIM needs arg 1 to be i32, but it is u32"});

    Program leaked = minimal();
    leaked.functions[0].cfg.blocks[0].commands = {
        Command {Op::Primitive, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 0)}, 3}};
    CHECK(messages(leaked) == std::vector<std::string> {"leaves a result in arg 0, but primitive exit returns void"});

    // `read_char` returns i32, so its result slot is a cell of exactly that type.
    Program no_result = minimal();
    body(no_result, {Command {Op::Primitive, {Operand::empty()}, 2}});
    CHECK(messages(no_result) == std::vector<std::string> {"leaves arg 0 empty, but primitive read_char returns i32"});

    Program wrong_result = minimal();
    body(wrong_result, {Command {Op::Primitive, {Operand::vreg(Type::U32, 0)}, 2}});
    CHECK(messages(wrong_result) == std::vector<std::string> {"PRIM needs arg 0 to be i32, but it is u32"});
  }
  SECTION("RET carries what the function returns") {
    Program void_return = minimal();
    void_return.functions[0].return_type = Type::Void;
    CHECK(messages(void_return) ==
          std::vector<std::string> {"returns arg 0 from a function that returns void",
                                    "the entrypoint returns void, but must return i32"});

    Program missing = minimal();
    missing.functions[0].cfg.blocks[0].commands[0] = Command {Op::Ret, {}};
    CHECK(messages(missing) == std::vector<std::string> {"returns nothing, but the function returns i32"});

    Program wrong = minimal();
    wrong.functions[0].cfg.blocks[0].commands[0] = Command {Op::Ret, {Operand::instant(Type::U32, 0)}};
    CHECK(messages(wrong) == std::vector<std::string> {"RET needs arg 0 to be i32, but it is u32"});
  }
}

TEST_CASE("3. a vreg is a cell in this frame") {
  SECTION("a frame is never empty, and holds the params") {
    Program empty = minimal();
    empty.functions[0].frame_size = 0;
    CHECK(messages(empty) == std::vector<std::string> {"has a frame of no cells, and a frame is never empty"});

    Program narrow = minimal();
    parameterized(narrow, {Type::I32, Type::I32}, 1);
    CHECK(messages(narrow) ==
          std::vector<std::string> {"has a frame of 1 cells, which does not hold its 2 parameters"});
  }
  SECTION("no vreg is outside the frame") {
    Program program = minimal();
    program.functions[0].frame_size = 2;
    body(program,
         {Command {Op::Add,
                   {Operand::vreg(Type::I32, 5), Operand::instant(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"ADD names $5 in a frame of 2 cells"});
  }
}

TEST_CASE("4. every cell read has been written, and it is one type") {
  SECTION("a read before any write is reported") {
    Program program = minimal();
    program.functions[0].frame_size = 2;
    body(program,
         {Command {Op::Add,
                   {Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"ADD reads $1, which nothing has written on the way here"});
  }
  SECTION("a param and a global are defined at entry") {
    Program program = minimal();
    parameterized(program, {Type::I32}, 3);
    program.globals.push_back(Global {Type::I32, Cfg {}});
    body(
        program,
        {Command {Op::Add, {Operand::vreg(Type::I32, 2), Operand::vreg(Type::I32, 0), Operand::global(Type::I32, 0)}}});
    CHECK(validate(program).empty());
  }
  SECTION("a cell written on one path only is not defined at the join") {
    // `if (b) { x = 1; } x = x + 1;` -- the addition is reachable without
    // the write, so $1 has no reaching definition.
    Program program = minimal();
    parameterized(program, {Type::I32}, 3);
    Cfg& cfg = program.functions[0].cfg;
    cfg.blocks.resize(3);
    cfg.blocks[0].commands.clear();
    cfg.blocks[0].next = 1;
    cfg.blocks[0].branch =
        Branch {Condition {Op::CmpNe, Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 0)}, 2};
    cfg.blocks[1].commands = {Command {Op::Mov, {Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}}};
    cfg.blocks[1].next = 2;
    cfg.blocks[2].commands = {
        Command {Op::Add, {Operand::vreg(Type::I32, 2), Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}},
        ret()};
    CHECK(messages(program) == std::vector<std::string> {"ADD reads $1, which nothing has written on the way here"});
  }
  SECTION("a cell written on every path is defined at the join") {
    Program program = minimal();
    parameterized(program, {Type::I32}, 3);
    Cfg& cfg = program.functions[0].cfg;
    cfg.blocks.resize(3);
    cfg.blocks[0].commands.clear();
    cfg.blocks[0].next = 1;
    cfg.blocks[0].branch =
        Branch {Condition {Op::CmpNe, Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 0)}, 2};
    const Command write {Op::Mov, {Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}};
    cfg.blocks[1].commands = {write};
    cfg.blocks[1].next = 2;
    cfg.blocks[2].commands = {write};
    cfg.blocks[2].commands.push_back(
        Command {Op::Add, {Operand::vreg(Type::I32, 2), Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}});
    cfg.blocks[2].commands.push_back(ret());
    CHECK(validate(program).empty());
  }
  SECTION("one cell is one type") {
    Program program = minimal();
    program.functions[0].frame_size = 3;
    body(program,
         {Command {Op::Mov, {Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}},
          Command {Op::Mov, {Operand::vreg(Type::U32, 1), Operand::instant(Type::U32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"MOV uses $1 as u32, but it is i32"});
  }
  SECTION("a branch test is a read too") {
    Program program = minimal();
    parameterized(program, {}, 3);
    Cfg& cfg = program.functions[0].cfg;
    cfg.blocks.resize(2);
    cfg.blocks[0].commands.clear();
    cfg.blocks[0].next = 1;
    cfg.blocks[0].branch =
        Branch {Condition {Op::CmpNe, Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 0)}, 1};
    cfg.blocks[1].commands = {ret()};
    CHECK(messages(program) ==
          std::vector<std::string> {"CMP_NE in the branch test reads $1, which nothing has written on the way here"});
  }
}

TEST_CASE("5. a call is checked against the function it names") {
  SECTION("the callee is a function that exists") {
    Program not_a_function = minimal();
    body(not_a_function, {Command {Op::Call, {Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 0)}}});
    CHECK(messages(not_a_function) == std::vector<std::string> {"CALL needs a function to call, but arg 1 is a vreg"});

    Program missing = minimal();
    body(missing, {Command {Op::Call, {Operand::vreg(Type::I32, 0), Operand::function(Type::I32, 4)}}});
    CHECK(messages(missing) == std::vector<std::string> {"calls function 4, which does not exist"});
  }
  SECTION("the arguments are the callee's params") {
    Program program = minimal();
    const std::size_t callee = add_binary(program);
    body(program,
         {Command {
             Op::Call,
             {Operand::vreg(Type::I32, 0), Operand::function(Type::I32, callee), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(program) == std::vector<std::string> {"gives 1 arguments to function 1, which takes 2"});

    Program wrong = minimal();
    const std::size_t other = add_binary(wrong);
    body(wrong,
         {Command {Op::Call,
                   {Operand::vreg(Type::I32, 0), Operand::function(Type::I32, other), Operand::instant(Type::I32, 1),
                    Operand::instant(Type::U32, 2)}}});
    CHECK(messages(wrong) == std::vector<std::string> {"CALL needs arg 3 to be i32, but it is u32"});

    Program good = minimal();
    const std::size_t right = add_binary(good);
    body(good,
         {Command {Op::Call,
                   {Operand::vreg(Type::I32, 0), Operand::function(Type::I32, right), Operand::instant(Type::I32, 1),
                    Operand::instant(Type::I32, 2)}}});
    CHECK(validate(good).empty());
  }
  SECTION("the result slot is EMPTY exactly when the callee returns void") {
    Program with_result = minimal();
    const std::size_t nothing = add_void(with_result);
    body(with_result,
         {Command {
             Op::Call,
             {Operand::vreg(Type::I32, 0), Operand::function(Type::Void, nothing), Operand::instant(Type::I32, 1)}}});
    CHECK(messages(with_result) == std::vector<std::string> {"leaves a result in arg 0, but function 1 returns void"});

    Program empty = minimal();
    const std::size_t value = add_binary(empty);
    body(empty,
         {Command {Op::Call,
                   {Operand::empty(), Operand::function(Type::I32, value), Operand::instant(Type::I32, 1),
                    Operand::instant(Type::I32, 2)}}});
    CHECK(messages(empty) == std::vector<std::string> {"leaves arg 0 empty, but function 1 returns i32"});

    Program right = minimal();
    const std::size_t void_callee = add_void(right);
    body(right,
         {Command {Op::Call,
                   {Operand::empty(), Operand::function(Type::Void, void_callee), Operand::instant(Type::I32, 1)}}});
    CHECK(validate(right).empty());
  }
  SECTION("the result is the callee's return type") {
    Program wrong = minimal();
    const std::size_t callee = add_binary(wrong);
    body(wrong,
         {Command {Op::Call,
                   {Operand::vreg(Type::U32, 0), Operand::function(Type::I32, callee), Operand::instant(Type::I32, 1),
                    Operand::instant(Type::I32, 2)}}});
    CHECK(messages(wrong) == std::vector<std::string> {"CALL needs arg 0 to be i32, but it is u32"});

    // The callee operand names its return type too; when it disagrees with the
    // function it names, that is reported before the result slot is looked at.
    Program lied = minimal();
    const std::size_t other = add_binary(lied);
    body(lied,
         {Command {Op::Call,
                   {Operand::vreg(Type::I32, 0), Operand::function(Type::U32, other), Operand::instant(Type::I32, 1),
                    Operand::instant(Type::I32, 2)}}});
    CHECK(messages(lied) == std::vector<std::string> {"calls a function returning u32, but function 1 returns i32"});
  }
}

TEST_CASE("6. the entrypoint is a function of no arguments returning i32") {
  Program missing = minimal();
  missing.entrypoint = 3;
  CHECK(messages(missing) == std::vector<std::string> {"the entrypoint is function 3, which does not exist"});
  CHECK(locations(missing) == std::vector<std::string> {"the program"});

  Program params = minimal();
  params.functions[0].params = {Type::I32};
  CHECK(messages(params) == std::vector<std::string> {"the entrypoint takes 1 arguments, but takes none"});

  Program returns = minimal();
  returns.functions[0].return_type = Type::U32;
  returns.functions[0].cfg.blocks[0].commands[0] = Command {Op::Ret, {Operand::instant(Type::U32, 0)}};
  CHECK(messages(returns) == std::vector<std::string> {"the entrypoint returns u32, but must return i32"});
}

TEST_CASE("7. a global initializer is a CFG like any other") {
  SECTION("the same CFG rules apply to it") {
    Program program = minimal();
    Cfg init;
    init.blocks.resize(3);
    init.blocks[0].next = 1;
    init.blocks[1].commands = {Command {Op::Ret, {}}};
    init.blocks[2].commands = {Command {Op::Ret, {}}};
    program.globals.push_back(Global {Type::I32, std::move(init)});
    CHECK(messages(program) == std::vector<std::string> {"is not reachable from the entry block"});
    CHECK(locations(program) == std::vector<std::string> {"global 0's initializer, block 2"});
  }
  SECTION("an initializer reads nothing") {
    Program program = minimal();
    Global reads;
    reads.type = Type::I32;
    reads.init.blocks.resize(1);
    reads.init.blocks[0].commands = {Command {Op::Mov, {Operand::global(Type::I32, 0), Operand::vreg(Type::I32, 3)}},
                                     Command {Op::Ret, {}}};
    program.globals.push_back(std::move(reads));
    CHECK(messages(program) == std::vector<std::string> {"MOV reads $3, which nothing has written on the way here"});
  }
  SECTION("a global the machine zeroes needs no initializer") {
    Program program = minimal();
    program.globals.push_back(Global {Type::I32, Cfg {}});
    CHECK(validate(program).empty());
  }
}

TEST_CASE("an error says which function, block, and command it is in") {
  Program program = minimal();
  program.functions[0].frame_size = 2;
  body(program,
       {Command {Op::Add, {Operand::vreg(Type::I32, 0), Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1)}}});
  const std::vector<ValidationError> errors = validate(program);
  REQUIRE(errors.size() == 1);
  CHECK(location_of(errors[0]) == "function 0, block 0");
  REQUIRE(errors[0].command.has_value());
  CHECK(*errors[0].command == 0);
  CHECK(to_string(errors[0]) ==
        "function 0, block 0, command 0: ADD reads $1, which nothing has written on the way here");
}

TEST_CASE("check is the same validation as a fallible step") {
  const Program program = minimal();
  CHECK(check(program).has_value());

  Program broken = minimal();
  broken.functions[0].frame_size = 0;
  const auto result = check(broken);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().size() == 1);
  CHECK(result.error()[0].message == "has a frame of no cells, and a frame is never empty");
}

TEST_CASE("a load needs a pointer, and one that points at the right thing") {
  SECTION("a non-pointer source is named as the type it is") {
    Program program = minimal();
    body(program, {Command {Op::Load, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 0)}}});
    CHECK(messages(program) == std::vector<std::string> {"LOAD needs arg 1 to be a pointer, but it is i32"});
  }
  SECTION("a pointer to the wrong pointee is named as what it points at") {
    Program program = minimal();
    body(program, {Command {Op::Load, {Operand::vreg(Type::I32, 0), Operand::instant(Type::pointer(Type::U8), 0)}}});
    CHECK(messages(program) == std::vector<std::string> {"LOAD needs arg 1 to point at i32, but it points at u8"});
  }
}

TEST_CASE("an address is only of a cell") {
  Program program = minimal();
  body(program, {Command {Op::Addr, {Operand::vreg(Type::pointer(Type::I32), 0), Operand::instant(Type::I32, 0)}}});
  CHECK(messages(program) ==
        std::vector<std::string> {"ADDR needs a vreg or a global to take the address of, but arg 1 is an instant"});
}