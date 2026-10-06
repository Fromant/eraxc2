#pragma once

#include <cstdint>
#include <string_view>

#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

    /// A source-level type, kept distinct from the IR type: `bool` and `i8` are both
    /// one signed byte in the machine but are not interchangeable in the source, so
    /// the parser has to tell them apart — `if (x)` on an `i8` is a type error.
    enum class SemType : std::uint8_t {
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
    };

    /// The machine type a source type lowers to: `bool` → `i8`, `char` → `u8`, and
    /// every integer to itself.
    middleend::Type lower(SemType type) noexcept;

    std::string_view to_string(SemType type) noexcept;

    /// The source type a lowered type came from, the inverse of `lower` for the
    /// types that have a distinct source spelling.
    SemType lift(middleend::Type type) noexcept;

    /// True for the types arithmetic is defined on: the integers and `char`, but
    /// not `bool` and not the absence of a value.
    constexpr bool is_integer(SemType type) noexcept {
        return type != SemType::Void && type != SemType::Bool;
    }

    constexpr bool is_signed(SemType type) noexcept {
        return middleend::is_signed(lower(type));
    }

    /// The type an integer literal takes where nothing expects one: a bare `5` is an
    /// `i32`, which is the width the language is smallest at and the one that costs the
    /// least to be wrong about. A literal that has to be another type says so — with a
    /// suffix, or by sitting in a position that expects one.
    constexpr SemType default_literal_type = SemType::I32;

}  // namespace erax::frontend
