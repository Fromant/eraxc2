#include "SemType.hpp"

namespace erax::frontend {

  middleend::Type lower(SemType type) noexcept {
    switch (type) {
      case SemType::Void: return middleend::Type::Void;
      case SemType::Bool: return middleend::Type::I8;
      case SemType::Char: return middleend::Type::U8;
      case SemType::I8: return middleend::Type::I8;
      case SemType::U8: return middleend::Type::U8;
      case SemType::I16: return middleend::Type::I16;
      case SemType::U16: return middleend::Type::U16;
      case SemType::I32: return middleend::Type::I32;
      case SemType::U32: return middleend::Type::U32;
      case SemType::I64: return middleend::Type::I64;
      case SemType::U64: return middleend::Type::U64;
    }
    return middleend::Type::Void;
  }

  SemType lift(middleend::Type type) noexcept {
    switch (type) {
      case middleend::Type::Void: return SemType::Void;
      case middleend::Type::I8: return SemType::I8;
      case middleend::Type::U8: return SemType::U8;
      case middleend::Type::I16: return SemType::I16;
      case middleend::Type::U16: return SemType::U16;
      case middleend::Type::I32: return SemType::I32;
      case middleend::Type::U32: return SemType::U32;
      case middleend::Type::I64: return SemType::I64;
      case middleend::Type::U64: return SemType::U64;
    }
    return SemType::Void;
  }

  std::string_view to_string(SemType type) noexcept {
    switch (type) {
      case SemType::Void: return "void";
      case SemType::Bool: return "bool";
      case SemType::Char: return "char";
      case SemType::I8: return "i8";
      case SemType::U8: return "u8";
      case SemType::I16: return "i16";
      case SemType::U16: return "u16";
      case SemType::I32: return "i32";
      case SemType::U32: return "u32";
      case SemType::I64: return "i64";
      case SemType::U64: return "u64";
    }
    return "?";
  }

}  // namespace erax::frontend
