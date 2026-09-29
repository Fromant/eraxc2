#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "middleend/jir/Builder.hpp"
#include "middleend/jir/Fold.hpp"
#include "middleend/jir/Jir.hpp"

using namespace erax::middleend;

namespace {

using fold::Result;

Op from_name(std::string_view name) {
    for (int i = 0; i <= static_cast<int>(Op::Ret); ++i) {
        const auto op = static_cast<Op>(i);
        if (to_string(op) == name) return op;
    }
    return Op::Ret;
}

}  // namespace

TEST_CASE("types are sized and signed") {
    CHECK(size_of(Type::Void) == 0);
    CHECK(size_of(Type::I8) == 1);
    CHECK(size_of(Type::U16) == 2);
    CHECK(size_of(Type::U32) == 4);
    CHECK(size_of(Type::I64) == 8);
    CHECK(size_of(Type::U64) == 8);

    CHECK(is_integer(Type::Void) == false);
    CHECK(is_integer(Type::U8) == true);
    CHECK(is_signed(Type::Void) == false);
    CHECK(is_signed(Type::I32) == true);
    CHECK(is_signed(Type::U8) == false);
    CHECK(is_signed(Type::I8) == true);

    CHECK(to_string(Type::Void) == "void");
    CHECK(to_string(Type::I32) == "i32");
    CHECK(to_string(Type::U64) == "u64");
}

TEST_CASE("shift counts mask to the operand width") {
    // The spec keeps 3 bits of the count for an 8-bit operand, 4 for 16, and so
    // on: a mask of 2^n - 1.
    CHECK(shift_mask(Type::I8) == 0x7);
    CHECK(shift_mask(Type::U8) == 0x7);
    CHECK(shift_mask(Type::I16) == 0xF);
    CHECK(shift_mask(Type::U32) == 0x1F);
    CHECK(shift_mask(Type::I64) == 0x3F);
}

TEST_CASE("the op table is complete and round-trips by name") {
    // Every op has a name, a name parses back, and the name matches the spec.
    REQUIRE(from_name("MOV") == Op::Mov);
    REQUIRE(from_name("SHR") == Op::Shr);
    REQUIRE(from_name("CMP_GE") == Op::CmpGe);
    REQUIRE(from_name("CALL") == Op::Call);
    REQUIRE(from_name("RET") == Op::Ret);
    REQUIRE(from_name("NOPE") == Op::Ret);

    for (int i = 0; i <= static_cast<int>(Op::Ret); ++i) {
        const auto op = static_cast<Op>(i);
        REQUIRE(from_name(to_string(op)) == op);
    }
}

TEST_CASE("arity comes from the op table") {
    CHECK(arity(Op::Add).min == 3);
    CHECK(arity(Op::Add).max == 3);
    CHECK(arity(Op::Neg).min == 2);
    CHECK(arity(Op::Neg).max == 2);
    CHECK(arity(Op::Ret).min == 0);
    CHECK(arity(Op::Ret).max == 1);
    CHECK(arity(Op::Call).min == 2);
    CHECK(arity(Op::Call).max == Arity::unbounded);

    CHECK(arity(Op::Add).accepts(3) == true);
    CHECK(arity(Op::Add).accepts(2) == false);
    CHECK(arity(Op::Ret).accepts(0) == true);
    CHECK(arity(Op::Ret).accepts(1) == true);
    CHECK(arity(Op::Ret).accepts(2) == false);
    CHECK(arity(Op::Call).accepts(2) == true);
    CHECK(arity(Op::Call).accepts(99) == true);
    CHECK(arity(Op::Call).accepts(1) == false);
}

TEST_CASE("only the comparisons are edge conditions") {
    CHECK(is_compare(Op::CmpEq) == true);
    CHECK(is_compare(Op::CmpGe) == true);
    CHECK(is_compare(Op::Add) == false);
    CHECK(is_compare(Op::Call) == false);
    CHECK(is_compare(Op::Ret) == false);

    CHECK(has_result(Op::Mov) == true);
    CHECK(has_result(Op::Call) == true);
    CHECK(has_result(Op::Ret) == false);
}

TEST_CASE("operands print per the spec's lexicon") {
    CHECK(to_string(Operand::instant(Type::I32, 5)) == "i32 5");
    CHECK(to_string(Operand::instant(Type::U8, 1)) == "u8 1");
    // A vreg prints with its type, so a `CALL`'s shape is readable.
    CHECK(to_string(Operand::vreg(Type::I32, 3)) == "i32 $3");
    CHECK(to_string(Operand::vreg(Type::Void, 0)) == "void $0");
    CHECK(to_string(Operand::global(Type::I64, 0)) == "@0");
    CHECK(to_string(Operand::function(Type::I32, 2)) == "&2");
    CHECK(to_string(Operand::empty()) == "EMPTY");
}

TEST_CASE("an instant prints by its type's signedness") {
    // The stored value is the raw bit pattern either way.
    CHECK(to_string(Operand::instant(Type::I8, 0xFF)) == "i8 -1");
    CHECK(to_string(Operand::instant(Type::U8, 0xFF)) == "u8 255");
    CHECK(to_string(Operand::instant(Type::I32, 0xFFFFFFFFu)) == "i32 -1");
}

TEST_CASE("the EMPTY sentinel is (VReg, void, -1)") {
    const Operand sentinel = Operand::empty();
    CHECK(sentinel.kind == OperandKind::VReg);
    CHECK(sentinel.type == Type::Void);
    CHECK(sentinel.value == Operand::empty_value);
    CHECK(sentinel.is_empty() == true);
    CHECK(Operand::vreg(Type::Void, Operand::empty_value).is_empty() == true);
    CHECK(Operand::vreg(Type::I32, 0).is_empty() == false);
    CHECK(Operand::instant(Type::Void, 0).is_empty() == false);
}

TEST_CASE("blocks admit 0, 1 or 2 edges") {
    BasicBlock block;
    CHECK(block.edge_count() == 0);
    block.next = 3;
    CHECK(block.edge_count() == 1);
    block.branch = Branch{Condition{Op::CmpNe, Operand::vreg(Type::I8, 0), Operand::instant(Type::I8, 0)}, 1};
    CHECK(block.edge_count() == 2);
}

TEST_CASE("a program prints the spec's CALL example") {
    CfgBuilder builder;
    builder.emit(Op::Call, {Operand::vreg(Type::I32, 3), Operand::function(Type::I32, 0),
                            Operand::vreg(Type::I32, 1), Operand::instant(Type::I32, 1),
                            Operand::vreg(Type::I32, 0)});
    builder.emit(Op::Ret, {Operand::vreg(Type::I32, 3)});

    Program program;
    Function function;
    function.params = {Type::I32, Type::I32, Type::I32};
    function.return_type = Type::I32;
    function.frame_size = 4;
    function.cfg = builder.take();
    program.functions.push_back(function);

    const std::string expected =
        "function 0: (i32, i32, i32) -> i32, frame 4\n"
        "  0:\n"
        "    CALL i32 $3 &0 i32 $1 i32 1 i32 $0\n"
        "    RET i32 $3\n"
        "entrypoint: 0\n";
    CHECK(print(program) == expected);
}

TEST_CASE("a void call states its shape with an EMPTY result") {
    CfgBuilder builder;
    builder.emit(Op::Call, {Operand::empty(), Operand::function(Type::Void, 1), Operand::vreg(Type::I32, 3)});
    builder.emit(Op::Ret, {});

    Program program;
    Function function;
    function.frame_size = 4;
    function.cfg = builder.take();
    program.functions.push_back(function);

    const std::string printed = print(program);
    CHECK(printed.find("CALL EMPTY &1 i32 $3") != std::string::npos);
    CHECK(printed.find("-> void") != std::string::npos);
}

TEST_CASE("a frame is never empty") {
    CHECK(CfgBuilder{}.frame_size() == 1);
    CHECK(CfgBuilder{0}.frame_size() == 1);
    CHECK(CfgBuilder{3}.frame_size() == 3);

    CfgBuilder builder;
    CHECK(builder.alloc(Type::I32) == 0);
    CHECK(builder.alloc(Type::I32) == 1);
    CHECK(builder.frame_size() == 2);

    CfgBuilder with_params{2};
    CHECK(with_params.alloc(Type::I32) == 2);
    CHECK(with_params.frame_size() == 3);
}

TEST_CASE("a closed block swallows no more commands") {
    CfgBuilder builder;
    builder.emit(Op::Ret, {});
    CHECK(builder.terminated() == true);
    builder.emit(Op::Add, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1),
                           Operand::instant(Type::I32, 2)});
    // The command went to a fresh block, not the closed one.
    CHECK(builder.cfg().blocks[0].commands.size() == 1);
    CHECK(builder.cfg().blocks.size() == 2);
}

TEST_CASE("a block takes at most one set of edges") {
    CfgBuilder builder;
    const std::size_t target = builder.new_block();
    CHECK(builder.link_next(target) == true);
    CHECK(builder.link_next(target) == false);
    CHECK(builder.link_branch(target, Condition{}, 0) == false);
}

TEST_CASE("a branch is the taken edge and next the fallthrough") {
    CfgBuilder builder;
    const std::size_t taken = builder.new_block();
    const std::size_t fallthrough = builder.new_block();
    builder.link_branch(taken, Condition{Op::CmpNe, Operand::vreg(Type::I8, 0), Operand::instant(Type::I8, 0)},
                        fallthrough);
    CHECK(builder.cfg().blocks[0].branch->target == taken);
    CHECK(builder.cfg().blocks[0].next.value() == fallthrough);
}

TEST_CASE("rollback undoes emission") {
    CfgBuilder builder;
    const auto first = builder.alloc(Type::I32);
    builder.emit(Op::Add, {Operand::vreg(Type::I32, first), Operand::instant(Type::I32, 1),
                           Operand::instant(Type::I32, 2)});
    const auto mark = builder.mark();

    const auto second = builder.alloc(Type::I32);
    builder.emit(Op::Add, {Operand::vreg(Type::I32, second), Operand::instant(Type::I32, 1),
                           Operand::instant(Type::I32, 2)});
    CHECK(builder.cfg().blocks[0].commands.size() == 2);
    CHECK(builder.alloc(Type::I32) == 2);

    builder.rollback(mark);
    CHECK(builder.cfg().blocks[0].commands.size() == 1);
    // The cells handed out after the mark are free again.
    CHECK(builder.alloc(Type::I32) == second);
}

TEST_CASE("rollback discards blocks created after the mark") {
    CfgBuilder builder;
    builder.emit(Op::Add, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1),
                           Operand::instant(Type::I32, 2)});
    const auto mark = builder.mark();
    builder.new_block();
    builder.new_block();
    CHECK(builder.cfg().blocks.size() == 3);
    builder.rollback(mark);
    CHECK(builder.cfg().blocks.size() == 1);
}

TEST_CASE("prune drops unreachable blocks and remaps edges") {
    CfgBuilder builder;
    builder.emit(Op::Add, {Operand::vreg(Type::I32, 0), Operand::instant(Type::I32, 1),
                           Operand::instant(Type::I32, 2)});
    builder.emit(Op::Ret, {Operand::vreg(Type::I32, 0)});
    // Two blocks no edge can ever reach.
    builder.new_block();
    builder.new_block();
    builder.prune();

    REQUIRE(builder.cfg().blocks.size() == 1);
    CHECK(builder.cfg().blocks[0].commands.size() == 2);
}

TEST_CASE("prune renumbers the blocks it keeps") {
    CfgBuilder builder;
    // 0: -> 1 (body); 1: -> 2; 2: RET.  Block 3 is unreachable.
    const std::size_t body = builder.new_block();
    const std::size_t last = builder.new_block();
    builder.link_next(body);
    builder.set_current(body);
    builder.link_next(last);
    builder.set_current(last);
    builder.emit(Op::Ret, {Operand::instant(Type::I32, 7)});
    builder.new_block();
    builder.prune();

    REQUIRE(builder.cfg().blocks.size() == 3);
    CHECK(builder.cfg().blocks[0].next.value() == 1);
    CHECK(builder.cfg().blocks[1].next.value() == 2);
    CHECK(builder.cfg().blocks[2].commands.size() == 1);
}

TEST_CASE("prune keeps a self-looping block") {
    CfgBuilder builder;
    // A block whose only edge is to itself has no end to fall off.
    builder.link_next(0);
    builder.prune();

    REQUIRE(builder.cfg().blocks.size() == 1);
    CHECK(builder.cfg().blocks[0].next.value() == 0);
}

TEST_CASE("recycle hands a scope's cells back") {
    CfgBuilder builder;
    const std::uint64_t first = builder.alloc(Type::I32);
    const std::uint64_t scope_low_water = builder.alloc(Type::I32);
    builder.alloc(Type::I32);
    CHECK(builder.frame_size() == 3);

    builder.recycle(scope_low_water);
    CHECK(builder.alloc(Type::I32) == scope_low_water);
    // Recycling does not shrink the frame: the high-water mark stands.
    CHECK(builder.frame_size() == 3);
    CHECK(first == 0);
}

TEST_CASE("a recycled cell is only reused at its own type") {
    // One vreg id means one type for the whole function, so a cell allocated
    // as i32 can never back an i8.
    CfgBuilder builder;
    const std::uint64_t low_water = builder.alloc(Type::I32);
    builder.alloc(Type::I32);
    builder.recycle(low_water);

    CHECK(builder.alloc(Type::I8) == 2);
    CHECK(builder.alloc(Type::I32) == low_water);
}

TEST_CASE("a rollback releases the cells it had handed out") {
    CfgBuilder builder;
    const auto mark = builder.mark();
    const std::uint64_t rolled_back = builder.alloc(Type::I32);
    builder.recycle(rolled_back);
    builder.rollback(mark);

    // The id is unallocated again, and hands out once more as a fresh cell.
    CHECK(builder.alloc(Type::I32) == rolled_back);
}

TEST_CASE("folding wraps like the machine") {
    CHECK(fold::binary(Op::Add, Type::I8, 0x7F, 1).value.value() == 0x80);
    CHECK(fold::binary(Op::Sub, Type::U8, 0, 1).value.value() == 0xFF);
    CHECK(fold::binary(Op::Mul, Type::I32, 0x10000, 0x10000).value.value() == 0);
    CHECK(fold::binary(Op::Add, Type::I64, ~std::uint64_t{0}, 1).value.value() == 0);
    CHECK(fold::binary(Op::Add, Type::I32, 0x7FFFFFFFu, 1).value.value() == 0x80000000u);
}

TEST_CASE("folding divides toward zero") {
    // -7 / 2 == -3, -7 % 2 == -1, narrowed to the 32-bit bit pattern.
    CHECK(fold::binary(Op::Sdiv, Type::I32, 0xFFFFFFF9u, 2).value.value() == 0xFFFFFFFDu);
    CHECK(fold::binary(Op::Smod, Type::I32, 0xFFFFFFF9u, 2).value.value() == 0xFFFFFFFFu);
    CHECK(fold::binary(Op::Udiv, Type::U32, 7, 2).value.value() == 3);
    CHECK(fold::binary(Op::Umod, Type::U32, 7, 2).value.value() == 1);
}

TEST_CASE("folding rejects the traps the machine dies on") {
    CHECK(fold::binary(Op::Sdiv, Type::I32, 1, 0).failure.value() == fold::Failure::DivisionByZero);
    CHECK(fold::binary(Op::Umod, Type::U8, 1, 0).failure.value() == fold::Failure::DivisionByZero);
    const std::uint64_t min = 0x80000000;
    CHECK(fold::binary(Op::Sdiv, Type::I32, min, 0xFFFFFFFFu).failure.value() == fold::Failure::SignedOverflow);
    // Unsigned division of the same bits is fine.
    CHECK(fold::binary(Op::Udiv, Type::U32, min, 0xFFFFFFFFu).value.has_value() == true);
}

TEST_CASE("folding masks shift counts and picks signedness") {
    CHECK(fold::binary(Op::Shl, Type::I8, 1, 8).value.value() == 1);
    CHECK(fold::binary(Op::Shl, Type::I8, 1, 3).value.value() == 8);
    // -1 >> 1 is -1 signed, 0x7FFF_FFFF unsigned.
    CHECK(fold::binary(Op::Sar, Type::I32, 0xFFFFFFFFu, 1).value.value() == 0xFFFFFFFFu);
    CHECK(fold::binary(Op::Shr, Type::U32, 0xFFFFFFFFu, 1).value.value() == 0x7FFFFFFFu);
}

TEST_CASE("folded comparisons are i8 0/1") {
    CHECK(fold::binary(Op::CmpLt, Type::I32, 1, 2).value.value() == 1);
    CHECK(fold::binary(Op::CmpLt, Type::I32, 2, 1).value.value() == 0);
    CHECK(fold::binary(Op::CmpEq, Type::U8, 0xFF, 0xFF).value.value() == 1);
    CHECK(fold::binary(Op::CmpGe, Type::I8, 0xFF, 1).value.value() == 0);
    CHECK(fold::binary(Op::CmpGe, Type::U8, 0xFF, 1).value.value() == 1);
}

TEST_CASE("folded negation and bitwise not") {
    CHECK(fold::unary(Op::Neg, Type::I32, 5).value.value() == 0xFFFFFFFBu);
    CHECK(fold::unary(Op::Not, Type::U8, 0x0F).value.value() == 0xF0);
    CHECK(fold::unary(Op::Neg, Type::I8, 0x80).value.value() == 0x80);
}

TEST_CASE("type ranges bound the literal checks") {
    CHECK(fold::range_of(Type::I8).min == -128);
    CHECK(fold::range_of(Type::I8).max == 127);
    CHECK(fold::range_of(Type::U8).min == 0);
    CHECK(fold::range_of(Type::U8).max == 255);
    CHECK(fold::range_of(Type::U16).max == 65535);
    CHECK(fold::range_of(Type::I32).max == 2147483647);
    // 2^64-1 is not an int64, so u64's range stops at i64max.
    CHECK(fold::range_of(Type::U64).max == std::numeric_limits<std::int64_t>::max());
    CHECK(fold::range_of(Type::Void).max < 0);
}

TEST_CASE("a printer renders a program with globals and a branch") {
    CfgBuilder init;
    init.emit(Op::Mov, {Operand::global(Type::I32, 0), Operand::instant(Type::I32, 42)});
    init.emit(Op::Ret, {});

    CfgBuilder body;
    body.emit(Op::CmpNe, {Operand::vreg(Type::I8, 1), Operand::vreg(Type::I8, 0), Operand::instant(Type::I8, 0)});
    const std::size_t then_block = body.new_block();
    const std::size_t after = body.new_block();
    body.link_branch(then_block,
                     Condition{Op::CmpEq, Operand::vreg(Type::I8, 1), Operand::instant(Type::I8, 0)}, after);
    body.set_current(then_block);
    body.emit(Op::Ret, {Operand::instant(Type::I32, 1)});
    body.set_current(after);
    body.emit(Op::Ret, {Operand::instant(Type::I32, 0)});

    Program program;
    program.globals.push_back(Global{Type::I32, init.take()});
    Function main;
    main.return_type = Type::I32;
    main.frame_size = 2;
    main.cfg = body.take();
    program.functions.push_back(main);

    const std::string printed = print(program);
    CHECK(printed.find("global 0: i32") != std::string::npos);
    CHECK(printed.find("MOV @0 i32 42") != std::string::npos);
    CHECK(printed.find("if CMP_EQ i8 $1, i8 0 then 1") != std::string::npos);
    CHECK(printed.find("-> 2") != std::string::npos);
}
