#include "SemType.hpp"

namespace erax::frontend {

  middleend::Type lower(const SemType& type) noexcept {
    switch (type.kind) {
      case SemType::Kind::Void: return middleend::Type::Void;
      case SemType::Kind::Bool: return middleend::Type::I8;
      case SemType::Kind::Char: return middleend::Type::U8;
      case SemType::Kind::I8: return middleend::Type::I8;
      case SemType::Kind::U8: return middleend::Type::U8;
      case SemType::Kind::I16: return middleend::Type::I16;
      case SemType::Kind::U16: return middleend::Type::U16;
      case SemType::Kind::I32: return middleend::Type::I32;
      case SemType::Kind::U32: return middleend::Type::U32;
      case SemType::Kind::I64: return middleend::Type::I64;
      case SemType::Kind::U64: return middleend::Type::U64;
      case SemType::Kind::Ptr: return middleend::Type::pointer(lower(*type.pointee));
    }
    return middleend::Type::Void;
  }

  SemType lift(middleend::Type type) noexcept {
    switch (type.kind) {
      case middleend::Type::Kind::Void: return SemType::Void;
      case middleend::Type::Kind::I8: return SemType::I8;
      case middleend::Type::Kind::U8: return SemType::U8;
      case middleend::Type::Kind::I16: return SemType::I16;
      case middleend::Type::Kind::U16: return SemType::U16;
      case middleend::Type::Kind::I32: return SemType::I32;
      case middleend::Type::Kind::U32: return SemType::U32;
      case middleend::Type::Kind::I64: return SemType::I64;
      case middleend::Type::Kind::U64: return SemType::U64;
      case middleend::Type::Kind::Ptr: return SemType::pointer(lift(*type.pointee));
    }
    return SemType::Void;
  }

  std::string to_string(const SemType& type) {
    switch (type.kind) {
      case SemType::Kind::Void: return "void";
      case SemType::Kind::Bool: return "bool";
      case SemType::Kind::Char: return "char";
      case SemType::Kind::I8: return "i8";
      case SemType::Kind::U8: return "u8";
      case SemType::Kind::I16: return "i16";
      case SemType::Kind::U16: return "u16";
      case SemType::Kind::I32: return "i32";
      case SemType::Kind::U32: return "u32";
      case SemType::Kind::I64: return "i64";
      case SemType::Kind::U64: return "u64";
      case SemType::Kind::Ptr: return "ptr " + to_string(*type.pointee);
    }
    return "?";
  }

}  // namespace erax::frontend
