#include "primitives/Primitives.hpp"

#include <utility>

namespace erax::primitives {

  namespace {

    /// The catalog itself, in id order: the index of a row *is* its id, which is
    /// what `by_id` and the `.jir` format rely on.
    constexpr std::array<Signature, 5> table {{
        {Prim::Print, "print", {}, 0, SigType::Void, true, false},
        {Prim::PrintChar, "print_char", {SigType::Char}, 1, SigType::Void, false, false},
        {Prim::ReadChar, "read_char", {}, 0, SigType::I32, false, false},
        {Prim::Exit, "exit", {SigType::I32}, 1, SigType::Void, false, true},
        {Prim::Abort, "abort", {}, 0, SigType::Void, false, true},
    }};

    template<std::size_t... Index>
    constexpr bool rows_match_ids(std::index_sequence<Index...>) noexcept {
      return ((table[Index].id == static_cast<Prim>(Index)) && ...);
    }

    static_assert(table.size() == static_cast<std::size_t>(Prim::Abort) + 1,
                  "every catalog id has exactly one row, and no row has an id of its own");
    static_assert(rows_match_ids(std::make_index_sequence<table.size()> {}));

  }  // namespace

  std::span<const Signature> catalog() noexcept {
    return table;
  }

  const Signature* by_name(std::string_view name) noexcept {
    for (const Signature& signature : table)
      if (signature.name == name)
        return &signature;
    return nullptr;
  }

  const Signature* by_id(std::uint32_t id) noexcept {
    if (id >= table.size())
      return nullptr;
    return &table[id];
  }

  middleend::Type lower(SigType type) noexcept {
    switch (type) {
      case SigType::Void: return middleend::Type::Void;
      case SigType::Bool: return middleend::Type::I8;
      case SigType::Char: return middleend::Type::U8;
      case SigType::I8: return middleend::Type::I8;
      case SigType::U8: return middleend::Type::U8;
      case SigType::I16: return middleend::Type::I16;
      case SigType::U16: return middleend::Type::U16;
      case SigType::I32: return middleend::Type::I32;
      case SigType::U32: return middleend::Type::U32;
      case SigType::I64: return middleend::Type::I64;
      case SigType::U64: return middleend::Type::U64;
    }
    return middleend::Type::Void;
  }

  bool terminates(const middleend::Command& command) noexcept {
    if (command.op != middleend::Op::Primitive)
      return false;
    const Signature* signature = by_id(command.prim);
    return signature && signature->terminating;
  }

}  // namespace erax::primitives
