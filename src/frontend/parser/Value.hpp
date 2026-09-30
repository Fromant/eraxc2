#pragma once

#include <cstdint>
#include <optional>

#include "SemType.hpp"
#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

/// A compile-time constant, carried alongside the operand that holds it so a
/// global initializer and an `if` can be folded without a second walk.
///
/// A constant is two things: the raw bit pattern the machine will see, and — when
/// the value is small enough to be exact — the number it stands for. The second
/// is what makes `-1` a compile error where an `u8` is expected, which no bit
/// pattern can say, since as a bit pattern `-1` is a perfectly good 255.
struct ConstInt {
    middleend::Type type = middleend::Type::Void;
    std::uint64_t bits = 0;
    /// The value as a number, valid only when `exact` is set.
    std::int64_t exact = 0;
    /// The value fits in an `i64`; the widest constants (`u64` above `i64max`)
    /// do not, and are checked against their type by width instead.
    bool exact_known = false;

    /// The value as a number, read as `type`. An exact one is the number itself; a
    /// `u64` above `i64max` has no exact reading, so it is its bits interpreted.
    std::int64_t signed_value() const noexcept;

    /// The value is representable in `type`: an exact one must lie in the type's
    /// range, an inexact one can only be a `u64`.
    bool fits(middleend::Type type) const noexcept;

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
    std::optional<ConstInt> constant;

    /// The type is a placeholder from the no-expected-type default (`u32`), so a
    /// real type elsewhere in the expression can still retype it. Only a constant
    /// can be a placeholder, and only a constant can be retyped.
    bool retypeable = false;

    /// Re-reads the constant as `type`, which `fits` must accept.
    void retype(SemType type) noexcept;

    /// The operand as it goes into a command: a constant materializes as an
    /// instant, anything else is already an operand.
    middleend::Operand as_operand() const noexcept {
        return constant ? middleend::Operand::instant(constant->type, constant->bits) : operand;
    }
};

}  // namespace erax::frontend
