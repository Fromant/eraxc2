#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "SemType.hpp"
#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

  struct ReferenceOrigin {
    std::size_t local = 0;
    std::size_t generation = 0;
    bool external = false;
    bool interior = false;
    std::size_t loan = std::numeric_limits<std::size_t>::max();
    friend bool operator==(const ReferenceOrigin&, const ReferenceOrigin&) = default;
  };

  /// A compile-time constant, carried alongside the operand that holds it so a
  /// global initializer and an `if` can be folded without a second walk.
  ///
  /// A constant is a type and the bits that type holds, and nothing else. Every
  /// literal is typed the moment it is read — by its suffix, by the position it
  /// fills, or by `default_literal_type` — so there is no "not yet known to be a
  /// value" state for a constant to be in, and therefore no need to carry the value
  /// separately from the bits. The bits are the value: `i32 -1` is `0xFFFFFFFF`, and
  /// a range check reads them as the type says.
  struct ConstInt {
    middleend::Type type = middleend::Type::Void;
    std::uint64_t bits = 0;

    /// The value as a number, read as `type`: sign-extended for a signed type, and
    /// a `u64` above `i64max` has no such reading, so it is compared as bits.
    std::int64_t signed_value() const noexcept;
    /// The value as a number read as an unsigned type of the same width.
    std::uint64_t unsigned_value() const noexcept;

    /// The same number read as `type`, which must be a type it fits.
    static ConstInt of(std::int64_t value, middleend::Type type) noexcept;
    /// The same bit pattern read as `type`, for a literal that already carries
    /// its bits.
    static ConstInt bits_of(std::uint64_t bits, middleend::Type type) noexcept;
  };

  /// The result of parsing an expression: the operand the value ends up in, and
  /// the constant it is when nothing had to be emitted for it.
  struct Value {
    SemType type = SemType::Void;
    middleend::Operand operand;
    std::optional<ConstInt> constant = std::nullopt;
    /// Whether the operand names a variable or global's own cell: the only thing
    /// `&` may take the address of. A deref's or a binary operator's result is a
    /// scratch vreg and is not addressable.
    bool addressable = false;
    /// The source cell consumed by `move`, for assignment's self-move check.
    std::optional<middleend::Operand> moved_from = std::nullopt;
    /// A field or dereference's actual storage address; operand holds its read value.
    std::optional<middleend::Operand> place_address = std::nullopt;
    bool object_mutable = false;
    bool raw_storage = false;
    bool global_storage = false;
    std::vector<ReferenceOrigin> origins = {};
    std::vector<ReferenceOrigin> place_origins = {};
    /// A temporary ownership flag, cleared when its value is transferred.
    std::optional<middleend::Operand> live_flag = std::nullopt;
    std::optional<std::size_t> local_id = std::nullopt;

    /// The operand as it goes into a command: a constant materializes as an
    /// instant, anything else is already an operand.
    middleend::Operand as_operand() const noexcept {
      return constant ? middleend::Operand::instant(constant->type, constant->bits) : operand;
    }
  };

}  // namespace erax::frontend
