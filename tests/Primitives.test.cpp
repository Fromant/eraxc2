#include "primitives/Primitives.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "middleend/jir/Jir.hpp"

using erax::middleend::Command;
using erax::middleend::Op;
using erax::middleend::Operand;
using erax::middleend::Type;
using namespace erax::primitives;

namespace {

  std::vector<std::string> names() {
    std::vector<std::string> listed;
    for (const Signature& signature : catalog()) {
      listed.emplace_back(signature.name);
    }
    return listed;
  }

}  // namespace

TEST_CASE("the catalog is the primitives of primitive.md, in id order") {
  CHECK(names() ==
        std::vector<std::string> {"print", "print_char", "read_char", "exit", "abort", "alloc", "free", "offset"});
  // The index is the id: a primitive is never renumbered.
  for (std::size_t id = 0; id < catalog().size(); ++id) {
    REQUIRE(by_id(static_cast<std::uint32_t>(id)) != nullptr);
    CHECK(by_id(static_cast<std::uint32_t>(id))->id == static_cast<Prim>(id));
  }
  CHECK(by_id(8) == nullptr);
  CHECK(by_id(0xFFFFFFFFu) == nullptr);
}

TEST_CASE("every signature reads the same in the table as through the lookups") {
  for (const Signature& signature : catalog()) {
    const Signature* byname = by_name(signature.name);
    REQUIRE(byname != nullptr);
    CHECK(byname == &signature);
    CHECK(by_id(static_cast<std::uint32_t>(signature.id)) == &signature);
  }
  CHECK(by_name("nope") == nullptr);
  CHECK(by_name("") == nullptr);
}

TEST_CASE("a signature types its parameters, its result, and its flow") {
  const Signature* print = by_name("print");
  REQUIRE(print != nullptr);
  CHECK(print->param_count == 0);
  CHECK(print->variadic_values);
  CHECK(print->return_type == SigType::Void);
  CHECK_FALSE(print->terminating);

  const Signature* print_char = by_name("print_char");
  REQUIRE(print_char != nullptr);
  CHECK(print_char->param_count == 1);
  CHECK(print_char->params[0] == SigType::Char);
  CHECK_FALSE(print_char->variadic_values);
  CHECK(print_char->return_type == SigType::Void);

  const Signature* read_char = by_name("read_char");
  REQUIRE(read_char != nullptr);
  CHECK(read_char->param_count == 0);
  CHECK_FALSE(read_char->variadic_values);
  CHECK(read_char->return_type == SigType::I32);

  const Signature* exit = by_name("exit");
  REQUIRE(exit != nullptr);
  CHECK(exit->param_count == 1);
  CHECK(exit->params[0] == SigType::I32);
  CHECK(exit->terminating);

  const Signature* abort = by_name("abort");
  REQUIRE(abort != nullptr);
  CHECK(abort->param_count == 0);
  CHECK(abort->terminating);

  const Signature* alloc = by_name("alloc");
  REQUIRE(alloc != nullptr);
  CHECK(alloc->param_count == 1);
  CHECK(alloc->params[0] == SigType::U64);
  CHECK(alloc->return_type == SigType::Ptr);
  CHECK_FALSE(alloc->terminating);

  const Signature* release = by_name("free");
  REQUIRE(release != nullptr);
  CHECK(release->param_count == 1);
  CHECK(release->params[0] == SigType::Ptr);
  CHECK(release->return_type == SigType::Void);

  const Signature* offset = by_name("offset");
  REQUIRE(offset != nullptr);
  CHECK(offset->param_count == 2);
  CHECK(offset->params[0] == SigType::Ptr);
  CHECK(offset->params[1] == SigType::U64);
  CHECK(offset->return_type == SigType::Ptr);
  CHECK_FALSE(offset->terminating);
}

TEST_CASE("a signature type lowers the way the source type does") {
  CHECK(lower(SigType::Void) == Type::Void);
  CHECK(lower(SigType::Bool) == Type::I8);
  CHECK(lower(SigType::Char) == Type::U8);
  CHECK(lower(SigType::I8) == Type::I8);
  CHECK(lower(SigType::U8) == Type::U8);
  CHECK(lower(SigType::I16) == Type::I16);
  CHECK(lower(SigType::U16) == Type::U16);
  CHECK(lower(SigType::I32) == Type::I32);
  CHECK(lower(SigType::U32) == Type::U32);
  CHECK(lower(SigType::I64) == Type::I64);
  CHECK(lower(SigType::U64) == Type::U64);
  CHECK(lower(SigType::Ptr) == Type::pointer(Type::U8));
}

TEST_CASE("termination is read off the command, by catalog id") {
  // A PRIM that names a terminating primitive closes its block; the same op
  // with another id does not, and neither do other ops.
  Command exit {Op::Primitive, {Operand::empty()}, static_cast<std::uint32_t>(Prim::Exit)};
  Command abort {Op::Primitive, {Operand::empty()}, static_cast<std::uint32_t>(Prim::Abort)};
  Command print {Op::Primitive, {Operand::empty()}, static_cast<std::uint32_t>(Prim::Print)};
  CHECK(terminates(exit));
  CHECK(terminates(abort));
  CHECK_FALSE(terminates(print));
  CHECK_FALSE(terminates(Command {Op::Ret, {}}));
  // An id the catalog does not have is not terminating — the validator is what
  // rejects the program for having it at all.
  CHECK_FALSE(terminates(Command {Op::Primitive, {}, 99}));
}
