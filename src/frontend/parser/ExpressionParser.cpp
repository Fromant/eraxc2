#include "ExpressionParser.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "middleend/jir/Fold.hpp"

namespace erax::frontend {

using middleend::Operand;
using middleend::Op;
using middleend::Type;

namespace {

ParseError at(std::string message, SourceLocation location) { return ParseError{std::move(message), location}; }

bool is_radix_letter(char c) noexcept {
    return c == 'x' || c == 'X' || c == 'b' || c == 'B' || c == 'o' || c == 'O';
}

/// A `Number` token is a float when it has a fraction or an exponent. A radix
/// literal is never one, even though `0xE` holds an `E`.
bool is_float_literal(std::string_view text) noexcept {
    if (text.size() > 2 && text[0] == '0' && is_radix_letter(text[1])) return false;
    return text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
           text.find('E') != std::string_view::npos;
}

int radix_of(char c) noexcept {
    if (c == 'x' || c == 'X') return 16;
    if (c == 'o' || c == 'O') return 8;
    return 2;
}

std::expected<std::uint64_t, std::string> parse_number(std::string_view text) {
    int base = 10;
    std::size_t index = 0;
    if (text.size() > 2 && text[0] == '0' && is_radix_letter(text[1])) {
        base = radix_of(text[1]);
        index = 2;
    }
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '_') continue;
        const std::uint64_t digit = c <= '9' ? static_cast<std::uint64_t>(c - '0')
                                             : static_cast<std::uint64_t>(c | 0x20) - 'a' + 10;
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / static_cast<std::uint64_t>(base)) {
            return std::unexpected("integer literal '" + std::string{text} + "' is too large for any type");
        }
        value = value * static_cast<std::uint64_t>(base) + digit;
    }
    return value;
}

/// The code point of a character literal, whose token text is the raw inner
/// slice with the escape still spelled out.
std::expected<std::uint64_t, std::string> parse_char(std::string_view raw) {
    if (raw.size() == 1) return static_cast<std::uint64_t>(static_cast<unsigned char>(raw[0]));
    if (raw[0] != '\\' || raw.size() < 2) return std::unexpected("expected a character literal");
    const char escape = raw[1];
    switch (escape) {
        case 'n': return std::uint64_t{'\n'};
        case 't': return std::uint64_t{'\t'};
        case 'r': return std::uint64_t{'\r'};
        case '0': return std::uint64_t{0};
        case 'b': return std::uint64_t{'\b'};
        case 'f': return std::uint64_t{'\f'};
        case 'v': return std::uint64_t{'\v'};
        case '\\': return std::uint64_t{'\\'};
        case '\'': return std::uint64_t{'\''};
        case '"': return std::uint64_t{'"'};
        case 'x':
        case 'u': break;
        default: return std::unexpected("invalid escape sequence '\\" + std::string{escape} + "'");
    }
    const std::size_t digits = escape == 'x' ? 2 : 4;
    if (raw.size() != digits + 2) {
        return std::unexpected("expected " + std::to_string(digits) + " hexadecimal digits after \\" +
                               std::string{escape});
    }
    std::uint64_t value = 0;
    for (std::size_t index = 2; index < raw.size(); ++index) {
        const char c = raw[index];
        value = value * 16 + (c <= '9' ? static_cast<std::uint64_t>(c - '0')
                                       : static_cast<std::uint64_t>(c | 0x20) - 'a' + 10);
    }
    return value;
}

std::uint64_t truncate(std::uint64_t bits, Type type) noexcept {
    const std::size_t width = middleend::size_of(type) * 8;
    if (width == 0 || width == 64) return bits;
    return bits & ((std::uint64_t{1} << width) - 1);
}

bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
    out = static_cast<std::int64_t>(sum);
    return static_cast<std::uint64_t>(out) == sum;
}

bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    const std::uint64_t difference = static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b);
    out = static_cast<std::int64_t>(difference);
    return static_cast<std::uint64_t>(out) == difference;
}

bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) ||
        (b == -1 && a == std::numeric_limits<std::int64_t>::min())) {
        return false;
    }
    const std::uint64_t product = static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b);
    out = static_cast<std::int64_t>(product);
    return static_cast<std::uint64_t>(out) == product;
}

/// The number a folded bit pattern stands for, sign-extended for a signed type.
/// Exact unless the pattern is a `u64` above `i64max`.
ConstInt from_bits(std::uint64_t bits, Type type) {
    ConstInt constant{type, truncate(bits, type), 0, false};
    const std::uint64_t width = middleend::size_of(type) * 8;
    if (width == 64) {
        // An `i64` is every 64-bit pattern read as a number; only a `u64` can be one
        // that has no number at all.
        if (middleend::is_signed(type)) {
            constant.exact = static_cast<std::int64_t>(constant.bits);
            constant.exact_known = true;
            return constant;
        }
        if (constant.bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            constant.exact = static_cast<std::int64_t>(constant.bits);
            constant.exact_known = true;
        }
        return constant;
    }
    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    if ((constant.bits & sign) != 0) {
        constant.exact = static_cast<std::int64_t>(constant.bits) - static_cast<std::int64_t>(std::uint64_t{1} << width);
    } else {
        constant.exact = static_cast<std::int64_t>(constant.bits);
    }
    constant.exact_known = true;
    return constant;
}

/// The exact value of a constant expression, when it has one. This is a question
/// about the source, not a fold: `fold::binary` answers in bit patterns and always
/// has one, but the two answers are not the same. `0x80000000` is a value the
/// source can name, or it is a value that left the `i64` range a constant is
/// written in and wrapped, and only the value tells those apart.
///
/// So there are three answers, not two: the value, `std::nullopt` when only the
/// bits are known (a shift or a product out of `i64` range), or a `ParseError`
/// where the expression cannot be a constant at all. The value is what a range
/// check, a folded `if`, and a division's sign all read, which is why the bits
/// cannot stand in for it.
std::expected<std::optional<ConstInt>, ParseError> exact_value(Op op, SemType operand, SemType result,
                                                              const ConstInt& a, const ConstInt& b,
                                                              SourceLocation location) {
    const Type operand_type = lower(operand);
    const Type result_type = lower(result);
    std::int64_t value = 0;

    switch (op) {
        case Op::Add: if (!checked_add(a.exact, b.exact, value)) return std::optional<ConstInt>{}; break;
        case Op::Sub: if (!checked_sub(a.exact, b.exact, value)) return std::optional<ConstInt>{}; break;
        case Op::Mul: if (!checked_mul(a.exact, b.exact, value)) return std::optional<ConstInt>{}; break;
        case Op::Sdiv:
        case Op::Smod:
            if (b.exact == 0) {
                return std::unexpected(at("division by zero in a constant expression", location));
            }
            if (a.exact == std::numeric_limits<std::int64_t>::min() && b.exact == -1) {
                return std::unexpected(
                    at("constant expression overflows " + std::string{to_string(result)}, location));
            }
            value = op == Op::Sdiv ? a.exact / b.exact : a.exact % b.exact;
            break;
        case Op::Udiv:
        case Op::Umod:
            if (b.exact == 0) {
                return std::unexpected(at("division by zero in a constant expression", location));
            }
            if (op == Op::Udiv) {
                value = static_cast<std::int64_t>(static_cast<std::uint64_t>(a.exact) /
                                                  static_cast<std::uint64_t>(b.exact));
            } else {
                value = static_cast<std::int64_t>(static_cast<std::uint64_t>(a.exact) %
                                                  static_cast<std::uint64_t>(b.exact));
            }
            break;
        case Op::Shl: {
            // A negative value has no exact shifted form, and a count of 63 or
            // more leaves the i64 range for anything but zero.
            const std::uint64_t amount = static_cast<std::uint64_t>(b.exact) & middleend::shift_mask(operand_type);
            if (a.exact == 0) {
                value = 0;
                break;
            }
            if (a.exact < 0 || amount >= 63 || !checked_mul(a.exact, std::int64_t{1} << amount, value)) {
                return std::optional<ConstInt>{};
            }
            break;
        }
        case Op::Shr:
        case Op::Sar:
            // A logical shift is only ever chosen for an unsigned operand, whose
            // exact value is never negative, and an arithmetic one is defined on
            // the two's-complement value.
            value = a.exact >> (static_cast<std::uint64_t>(b.exact) & middleend::shift_mask(operand_type));
            break;
        case Op::And:
        case Op::Or:
        case Op::Xor: {
            const std::uint64_t bits =
                op == Op::And ? (a.bits & b.bits) : (op == Op::Or ? (a.bits | b.bits) : (a.bits ^ b.bits));
            return from_bits(bits, result_type);
        }
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::CmpLt:
        case Op::CmpLe:
        case Op::CmpGt:
        case Op::CmpGe: {
            const middleend::fold::Result compared =
                middleend::fold::binary(op, operand_type, a.bits, b.bits);
            return ConstInt::of(compared.value.value_or(0), result_type);
        }
        case Op::Mov:
        case Op::Ret:
        case Op::Call:
        case Op::Inc:
        case Op::Dec:
        case Op::Neg:
        case Op::Not: break;
    }
    return ConstInt::of(value, result_type);
}

}  // namespace

ExpressionParser::result ExpressionParser::expression(std::optional<SemType> expected) {
    return or_expression(expected);
}

ExpressionParser::result ExpressionParser::const_expression(SemType expected) {
    const SourceLocation start = context().cursor.location();
    const bool was_const = const_mode_;
    const_mode_ = true;
    auto value = or_expression(expected);
    const_mode_ = was_const;
    if (!value) return value;
    if (!value->constant) {
        return std::unexpected(at("this must be a constant expression", start));
    }
    return value;
}

ExpressionParser::result ExpressionParser::or_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::logxor_expression, expected, {"||"});
}

ExpressionParser::result ExpressionParser::logxor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::and_expression, expected, {"^^"});
}

ExpressionParser::result ExpressionParser::and_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitor_expression, expected, {"&&"});
}

ExpressionParser::result ExpressionParser::bitor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitxor_expression, expected, {"|"});
}

ExpressionParser::result ExpressionParser::bitxor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitand_expression, expected, {"^"});
}

ExpressionParser::result ExpressionParser::bitand_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::equality_expression, expected, {"&"});
}

ExpressionParser::result ExpressionParser::equality_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::relational_expression, expected, {"==", "!="});
}

ExpressionParser::result ExpressionParser::relational_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::shift_expression, expected, {"<", "<=", ">", ">="});
}

ExpressionParser::result ExpressionParser::shift_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::additive_expression, expected, {"<<", ">>"});
}

ExpressionParser::result ExpressionParser::additive_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::multiplicative_expression, expected, {"+", "-"});
}

ExpressionParser::result ExpressionParser::multiplicative_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::unary_expression, expected, {"*", "/", "%"});
}

ExpressionParser::result ExpressionParser::binary_level(operand_level operand, std::optional<SemType> expected,
                                                        std::initializer_list<std::string_view> operators) {
    // The expected type belongs to the whole expression, not to its operands: an
    // operator's result has the operator's own type, so nothing here may be typed
    // by it until it is known that there is no operator at all. That is what keeps
    // `1 < 2` a `bool` in a `bool` position instead of typing its operands `bool`.
    const SourceLocation start = context().cursor.location();
    auto first = (this->*operand)(std::nullopt);
    if (!first) return first;
    Value left = *first;
    bool combined_any = false;

    while (true) {
        std::string_view op;
        for (const std::string_view candidate : operators) {
            if (context().cursor.at_op(candidate)) {
                op = candidate;
                break;
            }
        }
        if (op.empty()) break;

        const SourceLocation location = context().cursor.location();
        context().cursor.take();

        // A left operand that is only a placeholder — a constant typed by the
        // no-expected-type default — leaves the type open, so the right operand is
        // parsed without one and the left one follows it.
        const std::optional<SemType> right_expected =
            left.retypeable ? std::nullopt : std::optional<SemType>{left.type};
        auto parsed = (this->*operand)(right_expected);
        if (!parsed) return parsed;
        Value right = *parsed;
        if (left.retypeable && !right.retypeable && right.type != SemType::Void) {
            auto typed = retype(left, right.type, location);
            if (!typed) return std::unexpected(typed.error());
        }

        auto combined = combine(op, left, right, location);
        if (!combined) return std::unexpected(combined.error());
        left = *combined;
        combined_any = true;
    }

    // A lone operand is the whole expression, so a literal in it takes the type of
    // the position it fills. Anything else already has a type of its own, and the
    // statement that wanted one — a `let`, a return, a test — is what checks it. The
    // retyping happens even where the placeholder's default type already reads as the
    // wanted one, because it is the retyping that checks the value against it.
    if (!combined_any && expected && left.retypeable) {
        auto typed = retype(left, *expected, start);
        if (!typed) return std::unexpected(typed.error());
    }
    return left;
}

ExpressionParser::result ExpressionParser::retype(Value& value, SemType expected, SourceLocation location) {
    // Only a constant of an integer type can change type: a literal adapting to the
    // position it fills. A `bool` is not an integer, so `1` is not a `bool` and
    // `let x: bool = 1;` is the type error it looks like.
    if (value.type != expected && (!value.constant || !is_integer(expected) || !is_integer(value.type))) {
        return std::unexpected(
            at("expected a value of type " + std::string{to_string(expected)} + ", not " +
                   std::string{to_string(value.type)},
               location));
    }
    if (value.constant && !value.constant->fits(lower(expected))) {
        return std::unexpected(at("this constant does not fit " + std::string{to_string(expected)}, location));
    }
    value.retype(expected);
    value.type = expected;
    value.retypeable = false;
    return value;
}

ExpressionParser::result ExpressionParser::combine(std::string_view op, Value& left, Value& right,
                                                   SourceLocation location) {
    const std::string quoted = "'" + std::string{op} + "'";

    if (op == "||" || op == "^^" || op == "&&") {
        if (left.type != SemType::Bool || right.type != SemType::Bool) {
            return std::unexpected(at(quoted + " is defined on 'bool' operands", location));
        }
        const Op lowered = op == "||" ? Op::Or : (op == "&&" ? Op::And : Op::Xor);
        return apply(lowered, SemType::Bool, left, right, location);
    }

    if (left.type == SemType::Void || right.type == SemType::Void) {
        return std::unexpected(at(quoted + " cannot be applied to a call that returns nothing", location));
    }
    if (left.type != right.type) {
        return std::unexpected(at(quoted + " needs two operands of the same type, but got " +
                                      std::string{to_string(left.type)} + " and " +
                                      std::string{to_string(right.type)},
                                  location));
    }

    if (op == "==" || op == "!=") {
        return apply(op == "==" ? Op::CmpEq : Op::CmpNe, SemType::Bool, left, right, location);
    }
    if (op == "<") return apply(Op::CmpLt, SemType::Bool, left, right, location);
    if (op == "<=") return apply(Op::CmpLe, SemType::Bool, left, right, location);
    if (op == ">") return apply(Op::CmpGt, SemType::Bool, left, right, location);
    if (op == ">=") return apply(Op::CmpGe, SemType::Bool, left, right, location);

    if (!is_integer(left.type)) {
        return std::unexpected(at(quoted + " is not defined for " + std::string{to_string(left.type)}, location));
    }

    if (op == "|") return apply(Op::Or, left.type, left, right, location);
    if (op == "^") return apply(Op::Xor, left.type, left, right, location);
    if (op == "&") return apply(Op::And, left.type, left, right, location);
    if (op == "+") return apply(Op::Add, left.type, left, right, location);
    if (op == "-") return apply(Op::Sub, left.type, left, right, location);
    if (op == "*") return apply(Op::Mul, left.type, left, right, location);
    // A placeholder has no type yet, and an exact value folds as a number rather
    // than as bits, so it is folded in the signed form. The two forms only differ
    // for a negative operand, and a negative value is not one an unsigned type can
    // be retyped to. A placeholder above `i64max` is not such a value: it has only
    // a bit pattern, and a pattern is not signed or unsigned on its own -- `u64max`
    // as bits is a large unsigned value, not -1. There the type decides, as it
    // does everywhere else.
    const bool exact_placeholder = left.retypeable && left.constant && left.constant->exact_known;
    const bool arithmetic = exact_placeholder || is_signed(left.type);
    if (op == "/") return apply(arithmetic ? Op::Sdiv : Op::Udiv, left.type, left, right, location);
    if (op == "%") return apply(arithmetic ? Op::Smod : Op::Umod, left.type, left, right, location);
    if (op == "<<") return apply(Op::Shl, left.type, left, right, location);
    // A signed shift right is arithmetic; an unsigned one — which is every
    // `char`, a 0-255 byte — is logical.
    return apply(arithmetic ? Op::Sar : Op::Shr, left.type, left, right, location);
}

ExpressionParser::result ExpressionParser::apply(Op op, SemType result, const Value& left, const Value& right,
                                                 SourceLocation location) {
    if (left.constant && right.constant) {
        // A constant is folded at the width its bits actually carry, which for a placeholder
        // is the full `u64` and not the default type it reports. Its range is not checked
        // either: a placeholder has no type until the expression takes the one it fills.
        const bool placeholder = result == left.type && left.retypeable && right.retypeable;
        const SemType operand_semtype = lift(left.constant->type);
        const Type operand_type = lower(operand_semtype);
        const Type result_type = placeholder ? operand_type : lower(result);
        const ConstInt& a = *left.constant;
        const ConstInt& b = *right.constant;
        std::optional<ConstInt> folded;
        if (a.exact_known && b.exact_known) {
            auto exact = exact_value(op, operand_semtype, result, a, b, location);
            if (!exact) return std::unexpected(exact.error());
            folded = *exact;
        }
        if (!folded) {
            // The bits are all that is left to fold: either an operand was already
            // only a pattern, or the value outgrew the range a constant is written in.
            // A division still has no answer here, and an overflow is a compile error
            // even though the same operation only wraps when it runs.
            const middleend::fold::Result raw = middleend::fold::binary(op, operand_type, a.bits, b.bits);
            if (raw.failure) {
                if (*raw.failure == middleend::fold::Failure::DivisionByZero) {
                    return std::unexpected(at("division by zero in a constant expression", location));
                }
                return std::unexpected(
                    at("constant expression overflows " + std::string{to_string(left.type)}, location));
            }
            folded = from_bits(*raw.value, result_type);
        }
        if (!placeholder && folded->exact_known && !folded->fits(result_type)) {
            return std::unexpected(at("constant expression overflows " + std::string{to_string(result)}, location));
        }
        return Value{result, Operand::instant(result_type, folded->bits), folded, placeholder};
    }

    const Type result_type = lower(result);
    const std::uint64_t destination = context().builder.alloc(result_type);
    context().builder.emit(op, {Operand::vreg(result_type, destination), left.as_operand(), right.as_operand()});
    return Value{result, Operand::vreg(result_type, destination), std::nullopt, false};
}

ExpressionParser::result ExpressionParser::apply_unary(Op op, SemType type, const Value& operand,
                                                       SourceLocation location) {
    if (operand.constant) {
        // As in `apply`: a placeholder is folded at the width its bits carry and is not
        // range-checked, because its type is still the position's to decide.
        const bool placeholder = operand.retypeable;
        const Type operand_type = operand.constant->type;
        std::optional<ConstInt> folded;
        if (operand.constant->exact_known) {
            std::int64_t value = 0;
            if (op == Op::Neg && checked_sub(0, operand.constant->exact, value)) {
                folded = ConstInt::of(value, operand_type);
            }
            // `~x` is `-x - 1`, so like a negation it keeps the value exact.
            if (op == Op::Not) folded = ConstInt::of(~operand.constant->exact, operand_type);
        }
        if (!folded) {
            const middleend::fold::Result raw = middleend::fold::unary(op, operand_type, operand.constant->bits);
            if (raw.failure) {
                return std::unexpected(
                    at("constant expression overflows " + std::string{to_string(type)}, location));
            }
            folded = from_bits(*raw.value, operand_type);
        }
        if (!placeholder && folded->exact_known && !folded->fits(operand_type)) {
            return std::unexpected(at("constant expression overflows " + std::string{to_string(type)}, location));
        }
        return Value{type, Operand::instant(operand_type, folded->bits), folded, placeholder};
    }

    const Type result_type = lower(type);
    const std::uint64_t destination = context().builder.alloc(result_type);
    context().builder.emit(op, {Operand::vreg(result_type, destination), operand.as_operand()});
    return Value{type, Operand::vreg(result_type, destination), std::nullopt, false};
}

ExpressionParser::result ExpressionParser::integer_literal(const Token& token, std::optional<SemType> expected) {
    if (is_float_literal(token.text)) {
        return std::unexpected(at("float is not supported yet", token.location));
    }
    // A literal is contextual, but only among the integer types: a `bool` is not an
    // integer that 0 and 1 happen to fit, so `let b: bool = 1;` is a type error and
    // `true` / `false` are the only way to write one.
    if (expected && !is_integer(*expected)) {
        return std::unexpected(
            at("expected a value of type " + std::string{to_string(*expected)} + ", not an integer literal",
               token.location));
    }
    auto parsed = parse_number(token.text);
    if (!parsed) return std::unexpected(at(parsed.error(), token.location));

    // With no expected type the literal is only a placeholder: the position it fills decides
    // its type later, so there is nothing to check it against yet and its bits are held at
    // full width — `let x: u64 = 4294967296;` must not lose the value to the default type.
    // The check belongs to the retyping that follows, which names the real type.
    const Type held = lower(expected.value_or(SemType::U64));
    const ConstInt constant = ConstInt::bits_of(*parsed, held);
    if (expected && !constant.fits(held)) {
        return std::unexpected(
            at("'" + token.text + "' does not fit " + std::string{to_string(*expected)}, token.location));
    }
    return Value{expected.value_or(default_literal_type), Operand::instant(held, constant.bits), constant,
                 !expected.has_value()};
}

ExpressionParser::result ExpressionParser::unary_expression(std::optional<SemType> expected) {
    TokenCursor& cursor = context().cursor;
    std::string_view op;
    if (cursor.at_op("-")) op = "-";
    if (cursor.at_op("!")) op = "!";
    if (cursor.at_op("~")) op = "~";

    if (!op.empty()) {
        const SourceLocation location = cursor.location();
        cursor.take();
        // `!` is defined on `bool`, so that is the only type its operand can have.
        const std::optional<SemType> operand_expected =
            op == "!" ? std::optional<SemType>{SemType::Bool} : expected;
        auto operand = unary_expression(operand_expected);
        if (!operand) return operand;
        const Value value = *operand;
        if (op == "!") {
            if (value.type != SemType::Bool) {
                return std::unexpected(
                    at("'!' is defined on 'bool', but got " + std::string{to_string(value.type)},
                       location));
            }
            const Value zero{SemType::Bool, Operand::instant(Type::I8, 0), ConstInt::of(0, Type::I8), false};
            return apply(Op::CmpNe, SemType::Bool, value, zero, location);
        }
        if (!is_integer(value.type)) {
            return std::unexpected(
                at("'" + std::string{op} + "' is not defined for " + std::string{to_string(value.type)},
                   location));
        }
        return apply_unary(op == "-" ? Op::Neg : Op::Not, value.type, value, location);
    }
    return primary(expected);
}

ExpressionParser::result ExpressionParser::call(const std::string& name, SourceLocation location) {
    const FunctionInfo* function = context().find_function(name);
    if (!function) {
        return std::unexpected(at("call to undeclared function '" + name + "'", location));
    }
    if (const_mode_) {
        return std::unexpected(at("a constant expression may not call '" + name + "'", location));
    }

    std::vector<Value> arguments;
    if (!context().cursor.at(TokenType::RParen)) {
        while (true) {
            // An argument past the last parameter has no type to take, so it is
            // left untyped and the count check below is what rejects it.
            const std::optional<SemType> expected =
                arguments.size() < function->params.size()
                    ? std::optional<SemType>{function->params[arguments.size()]}
                    : std::nullopt;
            auto argument = expression(expected);
            if (!argument) return argument;
            if (expected && argument->type != *expected) {
                return std::unexpected(at("argument " + std::to_string(arguments.size() + 1) + " of '" +
                                              name + "' is " + std::string{to_string(*expected)} +
                                              ", not " + std::string{to_string(argument->type)}, location));
            }
            arguments.push_back(*argument);
            if (!context().cursor.match(TokenType::Comma)) break;
        }
    }
    if (arguments.size() != function->params.size()) {
        return std::unexpected(at("'" + name + "' takes " + std::to_string(function->params.size()) +
                                      " arguments, but " + std::to_string(arguments.size()) + " were given",
                                  location));
    }
    if (context().cursor.take().type != TokenType::RParen) {
        return std::unexpected(at("expected ')' after the arguments of '" + name + "'", context().cursor.location()));
    }

    const Type return_type = lower(function->return_type);
    const std::uint64_t destination =
        return_type == Type::Void ? 0 : context().builder.alloc(return_type);
    std::vector<Operand> args;
    args.push_back(return_type == Type::Void ? Operand::empty() : Operand::vreg(return_type, destination));
    args.push_back(Operand::function(return_type, function->id));
    for (const Value& argument : arguments) args.push_back(argument.as_operand());
    context().builder.emit(Op::Call, std::move(args));

    if (return_type == Type::Void) return Value{SemType::Void, Operand::empty(), std::nullopt, false};
    return Value{function->return_type, Operand::vreg(return_type, destination), std::nullopt, false};
}

ExpressionParser::result ExpressionParser::primary(std::optional<SemType> expected) {
    TokenCursor& cursor = context().cursor;
    const SourceLocation location = cursor.location();
    const Token token = cursor.current();

    switch (token.type) {
        case TokenType::Number: {
            cursor.take();
            return integer_literal(token, expected);
        }
        case TokenType::Char: {
            cursor.take();
            auto code = parse_char(token.text);
            if (!code) return std::unexpected(at(code.error(), token.location));
            if (*code > 255) {
                return std::unexpected(at("character literal is not a 0-255 code point", token.location));
            }
            const ConstInt constant = ConstInt::of(static_cast<std::int64_t>(*code), Type::U8);
            return Value{SemType::Char, Operand::instant(Type::U8, constant.bits), constant, false};
        }
        case TokenType::String:
            return std::unexpected(at("string is not supported yet", location));
        case TokenType::Identifier:
            break;
        case TokenType::LParen: {
            cursor.take();
            auto inner = or_expression(expected);
            if (!inner) return inner;
            if (cursor.take().type != TokenType::RParen) {
                return std::unexpected(at("expected ')'", cursor.location()));
            }
            return inner;
        }
        default:
            return std::unexpected(at("expected an expression", location));
    }

    // An identifier is a name: a call, a local, or a global.
    const std::string name = token.text;
    const Keyword keyword = keyword_of(name);
    if (keyword == Keyword::True || keyword == Keyword::False) {
        cursor.take();
        // A `bool` is not an integer, so a literal in a position expecting one is
        // a type error rather than something to convert.
        if (expected && *expected != SemType::Bool) {
            return std::unexpected(
                at("expected a value of type " + std::string{to_string(*expected)} + ", not 'bool'", location));
        }
        const std::int64_t value = keyword == Keyword::True ? 1 : 0;
        const ConstInt constant = ConstInt::of(value, Type::I8);
        return Value{SemType::Bool, Operand::instant(Type::I8, constant.bits), constant, false};
    }
    if (keyword != Keyword::None) {
        return std::unexpected(at("'" + name + "' is a reserved word", location));
    }

    // A dot would be the start of a namespace or a member access, and v1 has
    // neither: a name is one identifier.
    cursor.take();
    if (cursor.at(TokenType::Dot)) {
        return std::unexpected(at("'.' is not part of a name in v1", location));
    }
    if (cursor.at(TokenType::LParen)) {
        if (context().find_function(name) == nullptr) {
            return std::unexpected(at("call to undeclared function '" + name + "'", location));
        }
        cursor.take();
        return call(name, location);
    }
    if (context().find_function(name) != nullptr) {
        return std::unexpected(at("'" + name + "' is a function and can only be called", location));
    }

    if (const LocalInfo* local = context().find_local(name)) {
        if (!local->assigned) {
            return std::unexpected(at("'" + name + "' is read before it is assigned", location));
        }
        return Value{local->sem_type, Operand::vreg(local->type, local->vreg), std::nullopt, false};
    }
    if (const GlobalInfo* global = context().find_global(name)) {
        if (const_mode_) {
            if (!global->value) {
                return std::unexpected(
                    at("'" + name + "' is declared after this initializer", location));
            }
            return Value{global->sem_type, Operand::global(global->type, global->id), global->value,
                         false};
        }
        return Value{global->sem_type, Operand::global(global->type, global->id), std::nullopt, false};
    }
    return std::unexpected(at("undeclared identifier '" + name + "'", location));
}

}  // namespace erax::frontend
