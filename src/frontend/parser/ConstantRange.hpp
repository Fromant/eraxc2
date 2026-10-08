#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "Value.hpp"
#include "middleend/jir/Jir.hpp"

/// The range check on a constant expression: what has to be true of a value for it
/// to be a value of its type, and where a folded operation is not one.
///
/// This is the whole of "a constant is checked, arithmetic wraps". The machine has no
/// such rule -- `docs/middleend/JIR.md` gives every arithmetic op a wrapping result --
/// so the check lives here, apart from the folding that wraps, and comes out of the
/// parser by deleting this module and the `fold` call in `ExpressionParser::apply`.
/// `Number` is the exact form a constant is checked in: two's complement has no room
/// for a value past the width of its own cell, and a wrapped one comes back looking
/// small and legitimate -- `2^64` wraps to `0`, and `imin - 1` wraps to `imax`.
namespace erax::frontend::range {

  struct Number {
    bool negative = false;
    std::uint64_t magnitude = 0;
  };

  /// The value of \p constant, read as its type says to read it.
  Number number_of(const ConstInt& constant) noexcept;

  /// The bits of \p value in \p type, if it is a value of that type at all.
  ///
  /// A negative number needs a signed type, and a signed type reaches one further down
  /// than it does up -- `imin` is `-2^(w-1)` and `imax` is `2^(w-1) - 1` -- because the
  /// sign bit is not a magnitude. A `u64` has no negative side, so its limit is the whole
  /// of the `u64` range and nothing above `u64max` reaches it.
  std::optional<std::uint64_t> bits_in_range(Number value, erax::middleend::Type type) noexcept;

  /// Whether a literal's magnitude is a value of \p type. A literal is written without a
  /// sign, so only the positive half of the range is asked about.
  bool magnitude_fits(std::uint64_t value, erax::middleend::Type type) noexcept;

  /// Whether a literal's magnitude is the operand of a unary `-` in \p type, which is the
  /// one place the negative half of a signed type is spelled: `-128i8` is `imin`.
  bool negated_magnitude_fits(std::uint64_t value, erax::middleend::Type type) noexcept;

  /// The exact result of a constant binary operation, or why it is not a value of
  /// \p type.
  ///
  /// Two constants always fold -- there is nothing to wait for -- so this fails only on
  /// a division with no answer or on a result that is not a value of the type it lands
  /// in. Every op is folded by the machine's own rules; `+`, `-`, `*` and `<<` are then
  /// checked against the range, because they are the only ones that can leave it, and a
  /// wrapped one reads back as a small plausible value that no check on the bits would
  /// catch.
  std::expected<ConstInt, std::string> fold(erax::middleend::Op op, erax::middleend::Type type, const ConstInt& left,
                                            const ConstInt& right);

  /// The exact result of negating the constant \p value in \p type, or why it is not a
  /// value of that type.
  ///
  /// A literal's `-` is read with the sign in view (`-128i8` is `imin`), so this is the
  /// *second* negation -- the one on a value the parser has already folded -- and it is
  /// the case the docs call out: `imin` is a value, and `-imin` is not.
  std::expected<ConstInt, std::string> negate(erax::middleend::Type type, const ConstInt& value);

}  // namespace erax::frontend::range