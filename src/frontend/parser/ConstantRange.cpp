#include "ConstantRange.hpp"

#include "SemType.hpp"
#include "middleend/jir/Fold.hpp"

namespace erax::frontend::range {

  namespace {

    using erax::middleend::Op;
    using erax::middleend::Type;

    /// The magnitude of \p value, without negating `i64min`, which has no positive
    /// counterpart and would not survive the round trip.
    std::uint64_t magnitude(std::int64_t value) noexcept {
      return value < 0 ? ~static_cast<std::uint64_t>(value) + 1 : static_cast<std::uint64_t>(value);
    }

    /// `a + b`, `a - b` or `a * b`, without wrapping. Fails where the result's magnitude
    /// outgrows a `u64`: a sum of two wide values can reach `2^64` and a product can reach
    /// `2^128`, and neither is a number any type can hold.
    bool arithmetic_of(Op op, Number a, Number b, Number& out) noexcept {
      constexpr std::uint64_t top = ~std::uint64_t {0};
      if (op == Op::Mul) {
        if (a.magnitude != 0 && b.magnitude > top / a.magnitude)
          return false;
        out = {a.negative != b.negative, a.magnitude * b.magnitude};
        return true;
      }
      // A subtraction is an addition of the other sign, so both come to here.
      if (op == Op::Sub)
        b.negative = !b.negative;
      if (a.negative != b.negative) {
        // The signs disagree, so the larger magnitude wins and the sign is its own.
        if (a.magnitude >= b.magnitude) {
          out = {a.negative, a.magnitude - b.magnitude};
        } else {
          out = {b.negative, b.magnitude - a.magnitude};
        }
        return true;
      }
      if (a.magnitude > top - b.magnitude)
        return false;
      out = {a.negative, a.magnitude + b.magnitude};
      return true;
    }

    /// Whether `value << count` keeps every bit of \p value, which is what a shift that
    /// stays in the range is. The count is masked to the operand width first, so it is the
    /// shift the machine performs: a count that reaches the width is a shift of nothing.
    bool shift_keeps_every_bit(std::uint64_t value, Type type, std::uint64_t count) noexcept {
      const std::size_t width = erax::middleend::size_of(type) * 8;
      const std::size_t amount = static_cast<std::size_t>(count % width);
      return amount == 0 || (value << amount >> amount) == value;
    }

  }  // namespace

  Number number_of(const ConstInt& constant) noexcept {
    if (!erax::middleend::is_signed(constant.type))
      return Number {false, constant.unsigned_value()};
    const std::int64_t value = constant.signed_value();
    return value < 0 ? Number {true, magnitude(value)} : Number {false, static_cast<std::uint64_t>(value)};
  }

  std::optional<std::uint64_t> bits_in_range(Number value, Type type) noexcept {
    const std::size_t width = erax::middleend::size_of(type) * 8;
    if (width == 0)
      return std::nullopt;
    const std::uint64_t sign_bit = std::uint64_t {1} << (width - 1);
    if (value.negative) {
      if (!erax::middleend::is_signed(type) || value.magnitude > sign_bit)
        return std::nullopt;
    } else if (erax::middleend::is_signed(type)) {
      // Up to a signed type is one short of its sign bit, which is not a magnitude.
      if (value.magnitude >= sign_bit)
        return std::nullopt;
    } else if (width < 64 && value.magnitude >= sign_bit * 2) {
      // Up to an unsigned type is the whole of it. A `u64` is already that, so it has
      // nothing above it that a `u64` can hold.
      return std::nullopt;
    }
    return value.negative ? ~value.magnitude + 1 : value.magnitude;
  }

  bool magnitude_fits(std::uint64_t value, Type type) noexcept {
    return bits_in_range(Number {false, value}, type).has_value();
  }

  bool negated_magnitude_fits(std::uint64_t value, Type type) noexcept {
    return erax::middleend::is_signed(type) && bits_in_range(Number {true, value}, type).has_value();
  }

  std::expected<ConstInt, std::string> fold(Op op, Type type, const ConstInt& a, const ConstInt& b) {
    const std::string name {erax::frontend::to_string(erax::frontend::lift(type))};
    const std::string out_of_range = "this constant does not fit " + name;

    if (op == Op::Add || op == Op::Sub || op == Op::Mul) {
      Number result;
      // A magnitude past `u64max` is in no type, and the error says so rather than
      // naming a type that could not hold it either way.
      if (!arithmetic_of(op, number_of(a), number_of(b), result)) {
        return std::unexpected("this constant expression is too large for any type");
      }
      const std::optional<std::uint64_t> bits = bits_in_range(result, type);
      if (!bits)
        return std::unexpected(out_of_range);
      return ConstInt::bits_of(*bits, type);
    }

    // Everything else cannot leave the range: a bitwise operation is a bit pattern, a
    // division and a right shift shrink the value, and a comparison produces a `bool`.
    // So the machine's answer is the answer, and the only failure it has is one it
    // cannot do.
    const erax::middleend::fold::Result folded = erax::middleend::fold::binary(op, type, a.bits, b.bits);
    if (folded.failure) {
      if (*folded.failure == erax::middleend::fold::Failure::DivisionByZero) {
        return std::unexpected("division by zero in a constant expression");
      }
      // `imin / -1` is `2^(w-1)`, which the type does not hold, and the fold reports
      // the trap rather than producing the number the machine cannot.
      return std::unexpected(out_of_range);
    }
    // A left shift is the one op left that can grow a value off the top of its type.
    if (op == Op::Shl && !shift_keeps_every_bit(a.bits, type, b.unsigned_value())) {
      return std::unexpected(out_of_range);
    }
    return ConstInt::bits_of(*folded.value, type);
  }

  std::expected<ConstInt, std::string> negate(Type type, const ConstInt& value) {
    const std::string out_of_range =
        "this constant does not fit " + std::string {erax::frontend::to_string(erax::frontend::lift(type))};
    Number result = number_of(value);
    result.negative = !result.negative;
    const std::optional<std::uint64_t> bits = bits_in_range(result, type);
    if (!bits)
      return std::unexpected(out_of_range);
    return ConstInt::bits_of(*bits, type);
  }

}  // namespace erax::frontend::range