#include "Value.hpp"

#include "middleend/jir/Fold.hpp"

namespace erax::frontend {

namespace {

/// The low `width` bits of \p bits, sign-extended when \p type is signed.
std::int64_t reinterpret(std::uint64_t bits, middleend::Type type) noexcept {
    const std::size_t width = middleend::size_of(type) * 8;
    if (width == 0) return 0;
    if (width == 64) return static_cast<std::int64_t>(bits);
    const std::uint64_t masked = bits & ((std::uint64_t{1} << width) - 1);
    if (middleend::is_signed(type) && (masked & (std::uint64_t{1} << (width - 1))) != 0) {
        return static_cast<std::int64_t>(masked) - static_cast<std::int64_t>(std::uint64_t{1} << width);
    }
    return static_cast<std::int64_t>(masked);
}

std::uint64_t truncate(std::uint64_t bits, middleend::Type type) noexcept {
    const std::size_t width = middleend::size_of(type) * 8;
    if (width == 0) return 0;
    return width == 64 ? bits : bits & ((std::uint64_t{1} << width) - 1);
}

}  // namespace

std::int64_t ConstInt::signed_value() const noexcept {
    return exact_known ? exact : reinterpret(bits, type);
}

std::uint64_t ConstInt::unsigned_value() const noexcept {
    if (exact_known) return static_cast<std::uint64_t>(exact);
    return truncate(bits, type);
}

bool ConstInt::fits(middleend::Type type) const noexcept {
    if (!middleend::is_integer(type)) return false;
    if (type == middleend::Type::U64) return true;
    const middleend::fold::Range range = middleend::fold::range_of(type);
    // A value too large to be exact never fits a type narrower than u64.
    if (!exact_known) return false;
    if (middleend::is_signed(type)) return exact >= range.min && exact <= range.max;
    return exact >= 0 && static_cast<std::uint64_t>(exact) <= static_cast<std::uint64_t>(range.max);
}

ConstInt ConstInt::of(std::int64_t value, middleend::Type type) noexcept {
    return ConstInt{type, static_cast<std::uint64_t>(value), value, true};
}

ConstInt ConstInt::bits_of(std::uint64_t bits, middleend::Type type) noexcept {
    ConstInt constant{type, truncate(bits, type), 0, false};
    if (bits <= static_cast<std::uint64_t>(INT64_MAX)) {
        constant.exact = static_cast<std::int64_t>(bits);
        constant.exact_known = true;
    }
    return constant;
}

void Value::retype(SemType type) noexcept {
    if (!constant) return;
    constant->type = lower(type);
    constant->bits = truncate(constant->bits, constant->type);
}

}  // namespace erax::frontend
