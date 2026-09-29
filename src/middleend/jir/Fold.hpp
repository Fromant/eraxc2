#pragma once

#include <cstdint>
#include <optional>

#include "Jir.hpp"

/// Compile-time evaluation of IR ops, using the same semantics the machine has
/// (`docs/middleend/JIR.md`): wrapping, truncation toward zero, masked shift
/// counts, and signedness from the type.
namespace erax::middleend::fold {

/// Why an op cannot be folded. Only reachable from a constant expression, so it
/// is a compile-time error rather than process death.
enum class Failure {
    DivisionByZero,
    SignedOverflow,
};

/// The folded result of a binary op, or why it is not foldable.
struct Result {
    std::optional<std::uint64_t> value;
    std::optional<Failure> failure;
};

/// Signed result range: `[min, max]` of the type, for range checks.
struct Range {
    std::int64_t min;
    std::int64_t max;
};

/// The value range of a type as an `int64` pair: `0..max` unsigned, `min..max`
/// signed. `u64`'s maximum is not an `int64`, so its `max` is capped at
/// `i64max` and a range check has to allow the wider values itself;
/// `Type::Void` has none, reported as an empty range.
Range range_of(Type type) noexcept;

/// Folds a binary op over two raw bit patterns. Comparisons produce `i8` 0/1.
/// Shifts mask the count to the operand width, so `1 << 40` is well defined.
Result binary(Op op, Type type, std::uint64_t left, std::uint64_t right) noexcept;

/// Folds a unary op over a raw bit pattern.
Result unary(Op op, Type type, std::uint64_t value) noexcept;

}  // namespace erax::middleend::fold
