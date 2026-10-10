#include "Fold.hpp"

#include <cassert>
#include <limits>

namespace erax::middleend::fold {

  namespace {

    constexpr Result ok(std::uint64_t value) noexcept {
      return Result {value, std::nullopt};
    }
    constexpr Result fail(Failure failure) noexcept {
      return Result {std::nullopt, failure};
    }

    /// Truncates a 64-bit pattern to the width of `type` and reinterprets it.
    std::uint64_t narrow(std::uint64_t value, Type type) noexcept {
      const std::size_t used = size_of(type) * 8;
      return used == 64 ? value : value & ((std::uint64_t {1} << used) - 1);
    }

    std::int64_t to_signed(std::uint64_t bits, Type type) noexcept {
      const std::size_t used = size_of(type) * 8;
      if (used == 64) {
        return static_cast<std::int64_t>(bits);
      }
      const std::uint64_t sign = std::uint64_t {1} << (used - 1);
      const std::uint64_t value = bits & ((std::uint64_t {1} << used) - 1);
      if ((value & sign) != 0) {
        return static_cast<std::int64_t>(value) - static_cast<std::int64_t>(std::uint64_t {1} << used);
      }
      return static_cast<std::int64_t>(value);
    }

    std::int64_t signed_min(Type type) noexcept {
      const std::size_t used = size_of(type) * 8;
      return used == 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t {1} << (used - 1));
    }

    std::int64_t signed_max(Type type) noexcept {
      const std::size_t used = size_of(type) * 8;
      return used == 64 ? std::numeric_limits<std::int64_t>::max() : (std::int64_t {1} << (used - 1)) - 1;
    }

    Result compare(Op op, Type type, std::uint64_t left, std::uint64_t right) noexcept {
      bool holds = false;
      if (is_signed(type)) {
        const std::int64_t a = to_signed(left, type);
        const std::int64_t b = to_signed(right, type);
        switch (op) {
          case Op::CmpEq: holds = a == b; break;
          case Op::CmpNe: holds = a != b; break;
          case Op::CmpLt: holds = a < b; break;
          case Op::CmpLe: holds = a <= b; break;
          case Op::CmpGt: holds = a > b; break;
          case Op::CmpGe: holds = a >= b; break;
          default: break;
        }
      } else {
        const std::uint64_t a = narrow(left, type);
        const std::uint64_t b = narrow(right, type);
        switch (op) {
          case Op::CmpEq: holds = a == b; break;
          case Op::CmpNe: holds = a != b; break;
          case Op::CmpLt: holds = a < b; break;
          case Op::CmpLe: holds = a <= b; break;
          case Op::CmpGt: holds = a > b; break;
          case Op::CmpGe: holds = a >= b; break;
          default: break;
        }
      }
      return ok(holds ? 1 : 0);
    }

    Result divide(Op op, Type type, std::uint64_t left, std::uint64_t right) noexcept {
      if (is_signed(type)) {
        const std::int64_t a = to_signed(left, type);
        const std::int64_t b = to_signed(right, type);
        if (b == 0) {
          return fail(Failure::DivisionByZero);
        }
        if (a == signed_min(type) && b == -1) {
          return fail(Failure::SignedOverflow);
        }
        const std::int64_t result = op == Op::Sdiv ? a / b : a % b;
        return ok(narrow(static_cast<std::uint64_t>(result), type));
      }
      const std::uint64_t a = narrow(left, type);
      const std::uint64_t b = narrow(right, type);
      if (b == 0) {
        return fail(Failure::DivisionByZero);
      }
      return ok(op == Op::Udiv ? a / b : a % b);
    }

    Result shift(Op op, Type type, std::uint64_t left, std::uint64_t right) noexcept {
      const std::uint64_t amount = narrow(right, type) & shift_mask(type);
      const std::uint64_t value = narrow(left, type);
      switch (op) {
        case Op::Shl: return ok(narrow(value << amount, type));
        case Op::Shr: return ok(value >> amount);
        case Op::Sar: return ok(narrow(static_cast<std::uint64_t>(to_signed(value, type) >> amount), type));
        default: break;
      }
      return ok(0);
    }

  }  // namespace

  Range range_of(Type type) noexcept {
    if (!is_integer(type)) {
      return {0, -1};
    }
    if (is_signed(type)) {
      return {signed_min(type), signed_max(type)};
    }
    if (size_of(type) == 8) {
      return {0, std::numeric_limits<std::int64_t>::max()};
    }
    return {0, static_cast<std::int64_t>(narrow(~std::uint64_t {0}, type))};
  }

  Result binary(Op op, Type type, std::uint64_t left, std::uint64_t right) noexcept {
    // A comparison is defined on pointers too (they compare as unsigned words), so
    // it is answered before the assertion that the operands are an integer type.
    switch (op) {
      case Op::CmpEq:
      case Op::CmpNe:
      case Op::CmpLt:
      case Op::CmpLe:
      case Op::CmpGt:
      case Op::CmpGe: return compare(op, type, left, right);
      default: break;
    }
    assert(is_integer(type));
    switch (op) {
      case Op::Add: return ok(narrow(left + right, type));
      case Op::Sub: return ok(narrow(left - right, type));
      case Op::Mul: return ok(narrow(left * right, type));
      case Op::Sdiv:
      case Op::Smod:
      case Op::Udiv:
      case Op::Umod: return divide(op, type, left, right);
      case Op::Shl:
      case Op::Shr:
      case Op::Sar: return shift(op, type, left, right);
      case Op::And: return ok(narrow(left & right, type));
      case Op::Or: return ok(narrow(left | right, type));
      case Op::Xor: return ok(narrow(left ^ right, type));
      default: break;
    }
    return fail(Failure::SignedOverflow);
  }

  Result unary(Op op, Type type, std::uint64_t value) noexcept {
    assert(is_integer(type));
    switch (op) {
      case Op::Neg: return ok(narrow(0 - value, type));
      case Op::Not: return ok(narrow(~value, type));
      case Op::Inc: return ok(narrow(value + 1, type));
      case Op::Dec: return ok(narrow(value - 1, type));
      default: break;
    }
    return fail(Failure::SignedOverflow);
  }

  std::uint64_t convert(Type to, Type from, std::uint64_t bits) noexcept {
    // Any non-void type converts to any non-void type: an integer and a pointer are
    // both a cell's worth of bits, so a conversion is a truncation (or a bit-preserving
    // move). A pointer reads as an unsigned word, so it takes the unsigned path.
    assert(to != Type::Void && from != Type::Void);
    const std::int64_t value = is_signed(from) ? to_signed(bits, from) : static_cast<std::int64_t>(narrow(bits, from));
    return narrow(static_cast<std::uint64_t>(value), to);
  }

}  // namespace erax::middleend::fold
