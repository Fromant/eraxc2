#include "Value.hpp"

#include "middleend/jir/Fold.hpp"

namespace erax::frontend {

  namespace {

    /// The low `width` bits of \p bits, with the sign the type says they carry. This is
    /// a value in no type, or the value of the type -- which is the whole reading of a
    /// constant, so both halves live here.
    std::uint64_t truncate(std::uint64_t bits, middleend::Type type) noexcept {
      const std::size_t width = middleend::size_of(type) * 8;
      if (width == 0 || width == 64)
        return bits;
      return bits & ((std::uint64_t {1} << width) - 1);
    }

  }  // namespace

  std::int64_t ConstInt::signed_value() const noexcept {
    const std::size_t width = middleend::size_of(type) * 8;
    if (!middleend::is_signed(type))
      return static_cast<std::int64_t>(truncate(bits, type));
    if (width == 0)
      return 0;
    if (width == 64)
      return static_cast<std::int64_t>(bits);
    const std::uint64_t value = truncate(bits, type);
    const std::uint64_t sign = std::uint64_t {1} << (width - 1);
    if ((value & sign) == 0)
      return static_cast<std::int64_t>(value);
    return static_cast<std::int64_t>(value) - static_cast<std::int64_t>(std::uint64_t {1} << width);
  }

  std::uint64_t ConstInt::unsigned_value() const noexcept {
    return truncate(bits, type);
  }

  ConstInt ConstInt::of(std::int64_t value, middleend::Type type) noexcept {
    return ConstInt {type, truncate(static_cast<std::uint64_t>(value), type)};
  }

  ConstInt ConstInt::bits_of(std::uint64_t bits, middleend::Type type) noexcept {
    return ConstInt {type, truncate(bits, type)};
  }

}  // namespace erax::frontend