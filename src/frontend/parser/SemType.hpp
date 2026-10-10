#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

  /// A source-level type, kept distinct from the IR type: `bool` and `i8` are both
  /// one signed byte in the machine but are not interchangeable in the source, so
  /// the parser has to tell them apart — `if (x)` on an `i8` is a type error. Like
  /// the IR type it is a scalar kind or a pointer to another source type.
  struct SemType {
    enum class Kind : std::uint8_t {
      Void,
      Bool,
      Char,
      I8,
      U8,
      I16,
      U16,
      I32,
      U32,
      I64,
      U64,
      Ptr,
    };

    Kind kind = Kind::Void;
    std::shared_ptr<const SemType> pointee;

    static const SemType Void;
    static const SemType Bool;
    static const SemType Char;
    static const SemType I8;
    static const SemType U8;
    static const SemType I16;
    static const SemType U16;
    static const SemType I32;
    static const SemType U32;
    static const SemType I64;
    static const SemType U64;

    /// A pointer to \p pointee.
    static SemType pointer(SemType pointee) noexcept {
      return SemType {Kind::Ptr, std::make_shared<const SemType>(pointee)};
    }

    bool is_pointer() const noexcept {
      return kind == Kind::Ptr;
    }
  };

  inline const SemType SemType::Void {SemType::Kind::Void, {}};
  inline const SemType SemType::Bool {SemType::Kind::Bool, {}};
  inline const SemType SemType::Char {SemType::Kind::Char, {}};
  inline const SemType SemType::I8 {SemType::Kind::I8, {}};
  inline const SemType SemType::U8 {SemType::Kind::U8, {}};
  inline const SemType SemType::I16 {SemType::Kind::I16, {}};
  inline const SemType SemType::U16 {SemType::Kind::U16, {}};
  inline const SemType SemType::I32 {SemType::Kind::I32, {}};
  inline const SemType SemType::U32 {SemType::Kind::U32, {}};
  inline const SemType SemType::I64 {SemType::Kind::I64, {}};
  inline const SemType SemType::U64 {SemType::Kind::U64, {}};

  /// Structural equality, so a pointer type built in one place matches the same
  /// type built in another.
  inline bool operator==(const SemType& a, const SemType& b) noexcept {
    if (a.kind != b.kind)
      return false;
    if (a.kind != SemType::Kind::Ptr)
      return true;
    return *a.pointee == *b.pointee;
  }

  /// The machine type a source type lowers to: `bool` → `i8`, `char` → `u8`, every
  /// integer to itself, and a pointer to a pointer to the lowered pointee.
  middleend::Type lower(const SemType& type) noexcept;

  std::string to_string(const SemType& type);

  /// The source type a lowered type came from, the inverse of `lower` for the
  /// types that have a distinct source spelling.
  SemType lift(middleend::Type type) noexcept;

  /// True for the types arithmetic is defined on: the integers and `char`, but
  /// not `bool`, not a pointer, and not the absence of a value.
  inline bool is_integer(const SemType& type) noexcept {
    return type.kind != SemType::Kind::Void && type.kind != SemType::Kind::Bool && type.kind != SemType::Kind::Ptr;
  }

  inline bool is_signed(const SemType& type) noexcept {
    return middleend::is_signed(lower(type));
  }

  /// The type an integer literal takes where nothing expects one: a bare `5` is an
  /// `i32`, which is the width the language is smallest at and the one that costs the
  /// least to be wrong about. A literal that has to be another type says so — with a
  /// suffix, or by sitting in a position that expects one.
  inline const SemType default_literal_type = SemType::I32;

}  // namespace erax::frontend
