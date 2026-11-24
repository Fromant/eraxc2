#include "ExpressionParser.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "ConstantRange.hpp"
#include "middleend/jir/Fold.hpp"
#include "primitives/Primitives.hpp"

namespace erax::frontend {

  using middleend::Op;
  using middleend::Operand;
  using middleend::Type;

  namespace {

    ParseError at(std::string message, SourceLocation location) {
      return ParseError {std::move(message), location};
    }

    /// A dotted name from its segments (`a`, `b`, `c` → `a.b.c`), for a message.
    std::string qualified_path(const std::vector<std::string>& parts) {
      std::string qualified;
      for (const std::string& segment : parts) {
        if (!qualified.empty()) {
          qualified += '.';
        }
        qualified += segment;
      }
      return qualified;
    }

    bool right_side_may_call(const TokenCursor& cursor) {
      const auto& tokens = cursor.tokens();
      int nesting = 0;
      for (std::size_t index = cursor.index(); index < tokens.size(); ++index) {
        const auto kind = tokens[index].type;
        if (kind == TokenType::LParen) {
          return true;
        }
        if (kind == TokenType::LBrace || kind == TokenType::LBracket) {
          ++nesting;
        }
        if (kind == TokenType::RParen || kind == TokenType::RBrace || kind == TokenType::RBracket) {
          if (nesting == 0) {
            break;
          }
          --nesting;
        }
        if (nesting == 0 &&
            (kind == TokenType::Comma || kind == TokenType::Semicolon || kind == TokenType::EndOfFile)) {
          break;
        }
      }
      return false;
    }

    /// The source type a catalog signature type spells. The two enums list the
    /// same types in the same order but alias nothing: the mapping is stated so
    /// a catalog edit that forgets a type is a compile error here, not a silent
    /// mismatch.
    SemType sem_of(primitives::SigType type) noexcept {
      switch (type) {
        case primitives::SigType::Void: return SemType::Void;
        case primitives::SigType::Bool: return SemType::Bool;
        case primitives::SigType::Char: return SemType::Char;
        case primitives::SigType::I8: return SemType::I8;
        case primitives::SigType::U8: return SemType::U8;
        case primitives::SigType::I16: return SemType::I16;
        case primitives::SigType::U16: return SemType::U16;
        case primitives::SigType::I32: return SemType::I32;
        case primitives::SigType::U32: return SemType::U32;
        case primitives::SigType::I64: return SemType::I64;
        case primitives::SigType::U64: return SemType::U64;
        // The catalog's one pointer is a byte pointer; a pointer is one word
        // whatever it points at, but its pointee still names what a load reads.
        case primitives::SigType::Ptr: return SemType::pointer(SemType::U8);
      }
      return SemType::Void;
    }

    bool is_radix_letter(char c) noexcept {
      return c == 'x' || c == 'X' || c == 'b' || c == 'B' || c == 'o' || c == 'O';
    }

    /// A `Number` token is a float when it has a fraction or an exponent. A radix
    /// literal is never one, even though `0xE` holds an `E`.
    bool is_float_literal(std::string_view text) noexcept {
      if (text.size() > 2 && text[0] == '0' && is_radix_letter(text[1])) {
        return false;
      }
      return text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
          text.find('E') != std::string_view::npos;
    }

    int radix_of(char c) noexcept {
      if (c == 'x' || c == 'X') {
        return 16;
      }
      if (c == 'o' || c == 'O') {
        return 8;
      }
      return 2;
    }

    /// The length of the type suffix on \p text, or 0 if it does not end in one.
    ///
    /// Longest match wins, so `i64` is not read as `i6` — there is no `i6`, but the same
    /// rule makes the suffix set open to a three-digit type later without touching this.
    std::size_t suffix_length(std::string_view text) noexcept {
      constexpr std::string_view suffixes[] = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"};
      std::size_t longest = 0;
      for (const std::string_view suffix : suffixes) {
        if (text.size() > suffix.size() && text.ends_with(suffix)) {
          longest = std::max(longest, suffix.size());
        }
      }
      return longest;
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
        if (c == '_') {
          continue;
        }
        // The tokenizer has already rejected a digit outside the base, so what is left
        // here is a letter that is not one -- a suffix this parser does not know. It is
        // caught rather than folded into the number, because `64ul` read as a base-10
        // digit sequence is `6721`, which looks like a plausible literal.
        if (!(c >= '0' && c <= '9') && !((c | 0x20) >= 'a' && (c | 0x20) <= 'f')) {
          return std::unexpected("'" + std::string {text.substr(index)} + "' is not a type");
        }
        const std::uint64_t digit =
            c <= '9' ? static_cast<std::uint64_t>(c - '0') : static_cast<std::uint64_t>(c | 0x20) - 'a' + 10;
        if (digit >= static_cast<std::uint64_t>(base)) {
          return std::unexpected("invalid digit '" + std::string {c} + "' in radix literal");
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / static_cast<std::uint64_t>(base)) {
          return std::unexpected("integer literal '" + std::string {text} + "' is too large for any type");
        }
        value = value * static_cast<std::uint64_t>(base) + digit;
      }
      return value;
    }

    /// The code point of a character literal, whose token text is the raw inner
    /// slice with the escape still spelled out.
    std::expected<std::uint64_t, std::string> parse_char(std::string_view raw) {
      if (raw.size() == 1) {
        return static_cast<std::uint64_t>(static_cast<unsigned char>(raw[0]));
      }
      if (raw[0] != '\\' || raw.size() < 2) {
        return std::unexpected("expected a character literal");
      }
      const char escape = raw[1];
      switch (escape) {
        case 'n': return std::uint64_t {'\n'};
        case 't': return std::uint64_t {'\t'};
        case 'r': return std::uint64_t {'\r'};
        case '0': return std::uint64_t {0};
        case 'b': return std::uint64_t {'\b'};
        case 'f': return std::uint64_t {'\f'};
        case 'v': return std::uint64_t {'\v'};
        case '\\': return std::uint64_t {'\\'};
        case '\'': return std::uint64_t {'\''};
        case '"': return std::uint64_t {'"'};
        case 'x':
        case 'u': break;
        default: return std::unexpected("invalid escape sequence '\\" + std::string {escape} + "'");
      }
      const std::size_t digits = escape == 'x' ? 2 : 4;
      if (raw.size() != digits + 2) {
        return std::unexpected("expected " + std::to_string(digits) + " hexadecimal digits after \\" +
                               std::string {escape});
      }
      std::uint64_t value = 0;
      for (std::size_t index = 2; index < raw.size(); ++index) {
        const char c = raw[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          return std::unexpected("expected " + std::to_string(digits) + " hexadecimal digits after \\" +
                                 std::string {escape});
        }
        value = value * 16 +
            (c <= '9' ? static_cast<std::uint64_t>(c - '0') : static_cast<std::uint64_t>(c | 0x20) - 'a' + 10);
      }
      return value;
    }

  }  // namespace

  ExpressionParser::result ExpressionParser::expression(std::optional<SemType> expected) {
    auto value = or_expression(expected);
    const auto& token = context().cursor.current();
    if (value && token.type == TokenType::Operator && token.text.size() > 2 &&
        token.text.find_first_not_of('>') == std::string::npos) {
      return std::unexpected(at("unknown operator '" + token.text + "'", token.location));
    }
    return value;
  }

  ExpressionParser::result ExpressionParser::const_expression(SemType expected) {
    const SourceLocation start = context().cursor.location();
    const bool was_const = const_mode_;
    const_mode_ = true;
    auto value = expression(expected);
    const_mode_ = was_const;
    if (!value) {
      return value;
    }
    if (!value->constant) {
      return std::unexpected(at("this must be a constant expression", start));
    }
    return value;
  }

  ExpressionParser::result ExpressionParser::or_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::logxor_expression, expected, {"||"}, false);
  }

  ExpressionParser::result ExpressionParser::logxor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::and_expression, expected, {"^^"}, false);
  }

  ExpressionParser::result ExpressionParser::and_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitor_expression, expected, {"&&"}, false);
  }

  ExpressionParser::result ExpressionParser::bitor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitxor_expression, expected, {"|"}, true);
  }

  ExpressionParser::result ExpressionParser::bitxor_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::bitand_expression, expected, {"^"}, true);
  }

  ExpressionParser::result ExpressionParser::bitand_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::equality_expression, expected, {"&"}, true);
  }

  ExpressionParser::result ExpressionParser::equality_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::relational_expression, expected, {"==", "!="}, false);
  }

  ExpressionParser::result ExpressionParser::relational_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::shift_expression, expected, {"<", "<=", ">", ">="}, false);
  }

  ExpressionParser::result ExpressionParser::shift_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::additive_expression, expected, {"<<", ">>"}, true);
  }

  ExpressionParser::result ExpressionParser::additive_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::multiplicative_expression, expected, {"+", "-"}, true);
  }

  ExpressionParser::result ExpressionParser::multiplicative_expression(std::optional<SemType> expected) {
    return binary_level(&ExpressionParser::cast_expression, expected, {"*", "/", "%"}, true);
  }

  ExpressionParser::result ExpressionParser::cast_expression(std::optional<SemType> expected) {
    // A cast's operand takes no expected type — the target says everything — so the
    // position's type is dropped when an `as` follows the operand, and reaches a
    // plain operand unchanged. `12888 as i8` reads `12888` as the `i32` it would be
    // anywhere else and then wraps it, rather than being range-checked against `i8`.
    const std::optional<SemType> operand_expected =
        context().cursor.keyword_follows(Keyword::As) ? std::nullopt : expected;
    result value = unary_expression(operand_expected);
    if (!value) {
      return value;
    }
    while (context().cursor.at_keyword(Keyword::As)) {
      const SourceLocation location = context().cursor.location();
      context().cursor.take();
      // The target may name a type parameter of the enclosing instantiation.
      auto target = context().read_type(context().cursor, "after 'as'");
      if (!target) {
        return std::unexpected(target.error());
      }
      value = cast_to(*value, *target, location);
      if (!value) {
        return value;
      }
    }
    return value;
  }

  ExpressionParser::result ExpressionParser::cast_to(const Value& value, SemType target, SourceLocation location) {
    if (value.type == SemType::Void) {
      return std::unexpected(at("cannot cast a value of type void", location));
    }
    if (value.type == target) {
      return value;
    }
    if (value.type.is_reference() || target.is_reference() || value.type.is_record() || target.is_record()) {
      return std::unexpected(at("casts cannot create or reinterpret records or safe references", location));
    }
    // `bool` has no type of its own in the IR — it is an `i8` 0/1 cell — so a cast TO
    // bool is the compare the machine already spells "nonzero" with, whatever integer
    // (or char, or bool, handled above) sits on the left.
    if (target == SemType::Bool) {
      const Value zero {value.type, Operand::instant(lower(value.type), 0), ConstInt::of(0, lower(value.type))};
      return apply(Op::CmpNe, SemType::Bool, value, zero, location);
    }
    const Type from = lower(value.type);
    const Type to = lower(target);
    // `char` and `u8` — and `bool` and `i8` — name one cell two ways: the bits are
    // already the target's, so only the source-level type changes.
    if (from == to) {
      Value result {target, value.operand, value.constant};
      result.origins = value.origins;
      return result;
    }
    if (value.constant) {
      const std::uint64_t bits = middleend::fold::convert(to, from, value.constant->bits);
      const ConstInt constant = ConstInt::bits_of(bits, to);
      return Value {target, Operand::instant(to, constant.bits), constant};
    }
    const std::uint64_t destination = context().builder.alloc(to);
    context().builder.emit(Op::Cast, {Operand::vreg(to, destination), value.as_operand()});
    Value result {target, Operand::vreg(to, destination), std::nullopt};
    result.origins = value.origins;
    return result;
  }

  ExpressionParser::result ExpressionParser::binary_level(operand_level operand, std::optional<SemType> expected,
                                                          std::initializer_list<std::string_view> operators,
                                                          bool thread_expected) {
    // The expected type belongs to the whole expression.
    // - For operators whose result type equals their operand type (arithmetic,
    //   bitwise, shifts): thread expected to the first operand ONLY when it is an
    //   integer type. This makes `let x: i8 = 127 + 1` fold in i8, but prevents
    //   a comparison's `bool` result type from threading into its operands.
    // - For comparisons and logical ops (result type != operand type): thread expected
    //   ONLY when lone (no operator at this level). This lets a lone literal like
    //   `let b: bool = true` work, but keeps `1 < 2` comparing two i32 values.
    const SourceLocation start = context().cursor.location();
    const bool lone = !context().cursor.operator_follows(operators);
    std::optional<SemType> first_expected;
    if (thread_expected && expected && is_integer(*expected)) {
      first_expected = expected;  // Thread only integer types for arithmetic/bitwise/shift
    } else if (lone && (!thread_expected || (expected && (expected->is_record() || expected->is_pointer())))) {
      first_expected = expected;  // Thread for comparisons/logical only when lone
    } else {
      first_expected = std::nullopt;
    }
    auto first = (this->*operand)(first_expected);
    if (!first) {
      return first;
    }
    Value left = *first;

    while (true) {
      std::string_view op;
      for (const std::string_view candidate : operators) {
        if (context().cursor.at_op(candidate)) {
          op = candidate;
          break;
        }
      }
      if (op.empty()) {
        break;
      }

      const SourceLocation location = context().cursor.location();
      context().cursor.take();

      if (!left.type.is_record() && (left.addressable || left.place_address) && right_side_may_call(context().cursor)) {
        auto snapshot = context().copy_value(left, location, true);
        if (!snapshot) {
          return snapshot;
        }
        left = *snapshot;
      }
      // Each operand takes the type of the operand before it, so `x + 1` types the
      // `1` as whatever `x` is.
      const auto right_type = op == "+" && left.type.is_pointer() ? SemType::U64 : left.type;
      auto parsed = (this->*operand)(right_type);
      if (!parsed) {
        return parsed;
      }
      Value right = *parsed;

      auto combined = combine(op, left, right, location);
      if (!combined) {
        return std::unexpected(combined.error());
      }
      left = *combined;
    }

    return left;
  }

  ExpressionParser::result ExpressionParser::combine(std::string_view op, Value& left, Value& right,
                                                     SourceLocation location) {
    const std::string quoted = "'" + std::string {op} + "'";

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
    if (op == "+" && left.type.is_pointer()) {
      if (right.type != SemType::U64) {
        return std::unexpected(at("pointer offset requires u64", location));
      }
      if (const_mode_) {
        return std::unexpected(at("pointer offset is not a constant expression", location));
      }
      const auto type = lower(left.type);
      const auto destination = Operand::vreg(type, context().builder.alloc(type));
      context().builder.emit(Op::Offset, {destination, left.as_operand(), right.as_operand()});
      Value result {left.type, destination};
      result.origins = left.origins;
      return result;
    }
    if (left.type != right.type) {
      return std::unexpected(at(quoted + " needs two operands of the same type, but got " +
                                    std::string {to_string(left.type)} + " and " + std::string {to_string(right.type)},
                                location));
    }

    if (left.type.is_record()) {
      return std::unexpected(at(quoted + " is not defined for " + to_string(left.type), location));
    }
    if (op == "==" || op == "!=") {
      return apply(op == "==" ? Op::CmpEq : Op::CmpNe, SemType::Bool, left, right, location);
    }
    // A `bool` is not an integer, so it has no order: it is a type of its own that
    // resolves to `i8` in the IR, where it is the same cell and the comparison would
    // be well defined. That is exactly why the rule is here and not left to the
    // machine. Equality is the exception, and it is answered above.
    if (!is_integer(left.type)) {
      return std::unexpected(at(quoted + " is not defined for " + std::string {to_string(left.type)}, location));
    }
    if (op == "<") {
      return apply(Op::CmpLt, SemType::Bool, left, right, location);
    }
    if (op == "<=") {
      return apply(Op::CmpLe, SemType::Bool, left, right, location);
    }
    if (op == ">") {
      return apply(Op::CmpGt, SemType::Bool, left, right, location);
    }
    if (op == ">=") {
      return apply(Op::CmpGe, SemType::Bool, left, right, location);
    }

    if (!is_integer(left.type)) {
      return std::unexpected(at(quoted + " is not defined for " + std::string {to_string(left.type)}, location));
    }

    if (op == "|") {
      return apply(Op::Or, left.type, left, right, location);
    }
    if (op == "^") {
      return apply(Op::Xor, left.type, left, right, location);
    }
    if (op == "&") {
      return apply(Op::And, left.type, left, right, location);
    }
    if (op == "+") {
      return apply(Op::Add, left.type, left, right, location);
    }
    if (op == "-") {
      return apply(Op::Sub, left.type, left, right, location);
    }
    if (op == "*") {
      return apply(Op::Mul, left.type, left, right, location);
    }
    // Signedness comes from the operand's type, which is a real type now: there is no
    // placeholder left for it to be guessed from.
    const bool arithmetic = is_signed(left.type);
    if (op == "/") {
      return apply(arithmetic ? Op::Sdiv : Op::Udiv, left.type, left, right, location);
    }
    if (op == "%") {
      return apply(arithmetic ? Op::Smod : Op::Umod, left.type, left, right, location);
    }
    if (op == "<<") {
      return apply(Op::Shl, left.type, left, right, location);
    }
    // A signed shift right is arithmetic; an unsigned one — which is every
    // `char`, a 0-255 byte — is logical.
    return apply(arithmetic ? Op::Sar : Op::Shr, left.type, left, right, location);
  }

  ExpressionParser::result ExpressionParser::apply(Op op, SemType result, const Value& left, const Value& right,
                                                   SourceLocation location) {
    const Type result_type = lower(result);
    if (left.constant && right.constant) {
      // The operands are the same type -- `combine` checked that -- so the fold is
      // done in it, whatever the result is: a comparison folds its `i32` operands and
      // lands an `i8`, and every other op's result is that same type back.
      auto folded = range::fold(op, lower(left.type), *left.constant, *right.constant);
      if (!folded) {
        return std::unexpected(at(folded.error(), location));
      }
      const ConstInt constant = ConstInt::bits_of(folded->bits, result_type);
      return Value {result, Operand::instant(result_type, constant.bits), constant};
    }

    const std::uint64_t destination = context().builder.alloc(result_type);
    context().builder.emit(op, {Operand::vreg(result_type, destination), left.as_operand(), right.as_operand()});
    return Value {result, Operand::vreg(result_type, destination), std::nullopt};
  }

  ExpressionParser::result ExpressionParser::apply_unary(Op op, SemType type, const Value& operand,
                                                         SourceLocation location) {
    const Type operand_type = lower(type);
    if (operand.constant) {
      if (op == Op::Neg) {
        // `-imin` is a value in the machine's rule and is not one in the source's,
        // which is the split `docs/core/types.md#constants` is about. The `-` on the
        // literal itself was read with the sign in view, so it is not this one.
        auto negated = range::negate(operand_type, *operand.constant);
        if (!negated) {
          return std::unexpected(at(negated.error(), location));
        }
        return Value {type, Operand::instant(operand_type, negated->bits), *negated};
      }
      // `~` maps every value of a type onto another value of it, so there is nothing
      // here to check and the machine's answer is the answer.
      const middleend::fold::Result folded = middleend::fold::unary(op, operand_type, operand.constant->bits);
      const ConstInt constant = ConstInt::bits_of(*folded.value, operand_type);
      return Value {type, Operand::instant(operand_type, constant.bits), constant};
    }

    const std::uint64_t destination = context().builder.alloc(operand_type);
    context().builder.emit(op, {Operand::vreg(operand_type, destination), operand.as_operand()});
    return Value {type, Operand::vreg(operand_type, destination), std::nullopt};
  }

  ExpressionParser::result ExpressionParser::integer_literal(const Token& token, std::optional<SemType> expected,
                                                             bool negated) {
    if (is_float_literal(token.text)) {
      return std::unexpected(at("float is not supported yet", token.location));
    }
    // A literal is contextual, but only among the integer types: a `bool` is not an
    // integer that 0 and 1 happen to fit, so `let b: bool = 1;` is a type error and
    // `true` / `false` are the only way to write one.
    if (expected && !is_integer(*expected)) {
      return std::unexpected(
          at("expected a value of type " + std::string {to_string(*expected)} + ", not an integer literal",
             token.location));
    }

    // A suffix names the type: `5i64`, `0xFFu8`. It is the type name rather than a
    // C-style `L`, because erax has no `long` and the width is already in the name.
    std::string_view digits = token.text;
    std::optional<SemType> suffixed;
    if (const std::size_t split = digits.size() - suffix_length(digits); split != digits.size()) {
      const std::string_view suffix = digits.substr(split);
      const std::optional<SemType> named = type_of(keyword_of(suffix));
      if (!named || !is_integer(*named)) {
        return std::unexpected(at("'" + std::string {suffix} + "' is not a type", token.location));
      }
      suffixed = named;
      digits = digits.substr(0, split);
    }

    auto parsed = parse_number(digits);
    if (!parsed) {
      return std::unexpected(at(parsed.error(), token.location));
    }

    // A suffix wins over the position, and disagreeing with it is an error rather
    // than a silent conversion: `let x: i32 = 5i64;` says `i32` and `i64` in one
    // expression. With no suffix, the position decides, and with no position either,
    // the literal is an `i32`.
    if (suffixed && expected && *suffixed != *expected) {
      return std::unexpected(at("'" + std::string {digits} + std::string {to_string(*suffixed)} + "' does not fit " +
                                    std::string {to_string(*expected)},
                                token.location));
    }
    // The value is read as a magnitude, so it is checked against the whole range of the
    // type: `255` fits a `u8`, `256` does not, and neither is quietly truncated. A
    // literal carries no sign of its own, so only the positive half of a signed type is
    // asked about -- unless a unary `-` is in front of it, which is how the negative
    // half is written at all.
    const SemType type = suffixed.value_or(expected.value_or(default_literal_type));
    const bool fits =
        negated ? range::negated_magnitude_fits(*parsed, lower(type)) : range::magnitude_fits(*parsed, lower(type));
    if (!fits) {
      return std::unexpected(
          at("'" + std::string {digits} + "' does not fit " + std::string {to_string(type)}, token.location));
    }
    // The value is the one the source names, so a literal under `-` is a negative one
    // rather than its magnitude with a `Neg` still to come.
    const ConstInt constant = ConstInt::bits_of(negated ? ~*parsed + 1 : *parsed, lower(type));
    return Value {type, Operand::instant(lower(type), constant.bits), constant};
  }

  ExpressionParser::result ExpressionParser::unary_expression(std::optional<SemType> expected) {
    TokenCursor& cursor = context().cursor;

    if (cursor.at_keyword(Keyword::Ref)) {
      const SourceLocation location = cursor.take().location;
      const bool mut = cursor.at_keyword(Keyword::Mut);
      if (mut) {
        cursor.take();
      }
      const bool conditional = mut && cursor.match_op("?");
      if (const_mode_) {
        return std::unexpected(at("a constant expression may not borrow a value", location));
      }
      auto value = unary_expression(std::nullopt);
      if (!value) {
        return value;
      }
      if (!conditional) {
        return context().borrow_value(*value, mut, location);
      }
      const auto root = context().conditional_receiver;
      const auto from_receiver = [&](const ReferenceOrigin& origin) {
        return root && origin.external && origin.local == *root;
      };
      const bool raw_projection =
          value->raw_storage && !value->origins.empty() && std::ranges::all_of(value->origins, from_receiver);
      if (!root || (!value->receiver_projection && !raw_projection)) {
        return std::unexpected(at("ref mut? requires a place projected through this", location));
      }
      auto borrowed = context().borrow_value(*value, false, location);
      if (borrowed) {
        borrowed->conditional_borrow = true;
      }
      return borrowed;
    }
    if (cursor.at_keyword(Keyword::Move)) {
      const SourceLocation location = cursor.take().location;
      if (const_mode_) {
        return std::unexpected(at("a constant expression may not move a value", location));
      }
      auto value = unary_expression(expected);
      if (!value) {
        return value;
      }
      if (!satisfies(value->type, Capability::Movable)) {
        return std::unexpected(at(to_string(value->type) + " is not Movable", location));
      }
      if (value->raw_storage && value->place_address) {
        Value moved = *value;
        moved.addressable = false;
        moved.place_address.reset();
        moved.raw_storage = false;
        moved.local_id.reset();
        context().track_temporary(moved);
        return moved;
      }
      if (!value->addressable || !value->local_id || value->place_address) {
        return std::unexpected(at("move needs a local variable or parameter", location));
      }
      LocalInfo* source = context().local_at(value->operand.value);
      if (source == nullptr) {
        return std::unexpected(at("move needs a local variable or parameter", location));
      }
      const Type type = lower(value->type);
      const auto destination = Operand::vreg(type, context().builder.alloc(type));
      context().builder.emit(Op::Mov, {destination, value->as_operand()});
      context().move_local(*source);
      source->assigned = false;
      source->moved = true;
      Value moved {value->type, destination};
      moved.moved_from = value->operand;
      moved.origins = value->origins;
      context().track_temporary(moved);
      return moved;
    }

    // `&x` takes the address of a cell — a local or a global, never a constant or
    // an intermediate — and yields a pointer to its type.
    if (cursor.at_op("&")) {
      const SourceLocation location = cursor.location();
      cursor.take();
      const std::optional<SemType> operand_expected =
          expected && expected->is_pointer() ? std::optional<SemType> {*expected->pointee} : std::nullopt;
      result operand = unary_expression(operand_expected);
      if (!operand) {
        return operand;
      }
      const Value value = *operand;
      if (value.type == SemType::Void) {
        return std::unexpected(at("cannot take the address of a value of type void", location));
      }
      if (value.constant) {
        return std::unexpected(at("cannot take the address of a constant", location));
      }
      if (value.operand.kind != middleend::OperandKind::VReg && value.operand.kind != middleend::OperandKind::Global) {
        return std::unexpected(at("cannot take the address of this expression", location));
      }
      if (!value.addressable || value.raw_storage) {
        return std::unexpected(at("cannot take the address of this expression", location));
      }
      const SemType pointer = SemType::pointer(value.type);
      const Type lowered = lower(pointer);
      if (value.place_address) {
        Value result {pointer, *value.place_address};
        result.origins = value.origins;
        return result;
      }
      const std::uint64_t destination = context().builder.alloc(lowered);
      context().builder.emit(Op::Addr, {Operand::vreg(lowered, destination), value.as_operand()});
      Value result {pointer, Operand::vreg(lowered, destination)};
      result.origins = value.origins;
      return result;
    }

    // `*p` reads the cell a pointer names; the result's type is the pointee's, which
    // also types the operand as a pointer to it when the position asks for one.
    if (cursor.at_op("*")) {
      const SourceLocation location = cursor.location();
      cursor.take();
      const std::optional<SemType> operand_expected =
          expected ? std::optional<SemType> {SemType::pointer(*expected)} : std::nullopt;
      result operand = unary_expression(operand_expected);
      if (!operand) {
        return operand;
      }
      return dereference(*operand, location);
    }

    std::string_view op;
    if (cursor.at_op("-")) {
      op = "-";
    }
    if (cursor.at_op("!")) {
      op = "!";
    }
    if (cursor.at_op("~")) {
      op = "~";
    }

    if (!op.empty()) {
      const SourceLocation location = cursor.location();
      cursor.take();
      // `!` is defined on `bool`, so that is the only type its operand can have.
      // For `-` and `~`, thread expected only when it is an integer type.
      const std::optional<SemType> operand_expected = op == "!" ?
          std::optional<SemType> {SemType::Bool} :
          (expected && is_integer(*expected) ? expected : std::nullopt);

      if (op == "-" && cursor.at(TokenType::Number)) {
        // A literal under `-` is read with the sign in view, because that is the
        // only way a negative value is ever written: there is no negative `i8`
        // literal without one. So the value that comes back is already the
        // negation, and there is no `Neg` left to apply.
        if (operand_expected && is_integer(*operand_expected) && !is_signed(*operand_expected)) {
          return std::unexpected(at("'-' is not defined for " + std::string {to_string(*operand_expected)}, location));
        }
        const Token token = cursor.current();
        cursor.take();
        return integer_literal(token, operand_expected, true);
      }
      result operand = unary_expression(operand_expected);
      if (!operand) {
        return operand;
      }
      const Value value = *operand;
      if (op == "!") {
        if (value.type != SemType::Bool) {
          return std::unexpected(
              at("'!' is defined on 'bool', but got " + std::string {to_string(value.type)}, location));
        }
        const Value zero {SemType::Bool, Operand::instant(Type::I8, 0), ConstInt::of(0, Type::I8)};
        return apply(Op::CmpEq, SemType::Bool, value, zero, location);
      }
      if (!is_integer(value.type)) {
        return std::unexpected(
            at("'" + std::string {op} + "' is not defined for " + std::string {to_string(value.type)}, location));
      }
      // An unsigned type has no negative side to negate into, so `-` on one is not an
      // arithmetic op at all; `~` is the same on both signednesses and stays.
      if (op == "-" && !is_signed(value.type)) {
        return std::unexpected(at("'-' is not defined for " + std::string {to_string(value.type)}, location));
      }
      return apply_unary(op == "-" ? Op::Neg : Op::Not, value.type, value, location);
    }
    auto value = primary(expected);
    if (!value) {
      return value;
    }
    return postfix(*value);
  }

  ExpressionParser::result ExpressionParser::call(const std::string& qualified,
                                                  const std::vector<FunctionInfo>* candidates, const TemplateInfo* tmpl,
                                                  SourceLocation location, std::vector<Value> arguments,
                                                  bool implicit_receiver) {
    if (const_mode_) {
      return std::unexpected(at("a constant expression may not call '" + qualified + "'", location));
    }

    // One concrete declaration types each argument the way it declares the
    // parameter. Several declarations are an overload set, and a template deduces
    // its type arguments — in both of those every argument is typed on its own
    // (a lone literal is `i32`), with no conversion in between.
    const bool contextual = candidates != nullptr && candidates->size() == 1 && tmpl == nullptr;
    const FunctionInfo* primary = contextual ? &(*candidates)[0] : nullptr;

    if (!context().cursor.at(TokenType::RParen)) {
      while (true) {
        // An argument past the last parameter has no type to take, so it is
        // left untyped and the count check below is what rejects it.
        const std::optional<SemType> expected = contextual && arguments.size() < primary->params.size() ?
            std::optional<SemType> {primary->params[arguments.size()]} :
            std::nullopt;
        auto argument = expression(expected);
        if (!argument) {
          return argument;
        }
        if (expected && argument->type != *expected) {
          return std::unexpected(at("argument " + std::to_string(arguments.size() + 1) + " of '" + qualified + "' is " +
                                        std::string {to_string(*expected)} + ", not " +
                                        std::string {to_string(argument->type)},
                                    location));
        }
        auto copied = context().copy_value(*argument, location, true);
        if (!copied) {
          return copied;
        }
        arguments.push_back(*copied);
        if (!context().cursor.match(TokenType::Comma)) {
          break;
        }
      }
    }
    std::vector<SemType> types;
    types.reserve(arguments.size());
    for (const Value& argument : arguments) {
      types.push_back(argument.type);
    }

    // An exact concrete match beats a template: `print('x')` is the concrete
    // `print(char)`, not an instantiation.
    const FunctionInfo* function = nullptr;
    if (candidates != nullptr) {
      for (const FunctionInfo& candidate : *candidates) {
        bool matches = candidate.params == types;
        if (implicit_receiver && candidate.params.size() == types.size() && !types.empty() &&
            candidate.params[0].is_reference() && types[0].is_reference() &&
            *candidate.params[0].pointee == *types[0].pointee &&
            (!candidate.params[0].mutable_reference || types[0].mutable_reference)) {
          matches = std::equal(candidate.params.begin() + 1, candidate.params.end(), types.begin() + 1);
        }
        if (matches && (!function || (implicit_receiver && candidate.params[0].mutable_reference))) {
          function = &candidate;
        }
      }
    }
    // No concrete declaration took the arguments: either deduce the template's
    // type arguments, or report why none of the declarations match.
    std::vector<SemType> type_args;
    bool deduced = false;
    if (function == nullptr && tmpl != nullptr) {
      std::optional<ParseError> deduction_error = deduce(tmpl, types, location, type_args);
      if (deduction_error) {
        if (candidates == nullptr || candidates->size() == 1) {
          return std::unexpected(*deduction_error);
        }
      } else {
        deduced = true;
      }
    }
    if (function == nullptr && !deduced) {
      if (implicit_receiver && candidates != nullptr && !candidates->empty() && !arguments.empty()) {
        const auto& receiver = arguments.front().type;
        const bool requires_mutable = std::all_of(candidates->begin(), candidates->end(), [](const FunctionInfo& f) {
          return !f.params.empty() && f.params.front().is_reference() && f.params.front().mutable_reference;
        });
        if (requires_mutable && (!receiver.is_reference() || !receiver.mutable_reference)) {
          return std::unexpected(at("method '" + qualified + "' needs mutable access", location));
        }
      }
      if (candidates != nullptr && candidates->size() > 1) {
        std::string listed;
        for (const SemType& type : types) {
          if (!listed.empty()) {
            listed += ", ";
          }
          listed.append(to_string(type));
        }
        return std::unexpected(
            at("no overload of '" + qualified + "' matches the arguments (" + listed + ")", location));
      }
      if (candidates != nullptr) {
        // The single concrete declaration's argument errors were already reported
        // where the arguments were read; what remains is the count.
        function = &(*candidates)[0];
        if (arguments.size() != function->params.size()) {
          return std::unexpected(at("'" + qualified + "' takes " + std::to_string(function->params.size()) +
                                        " arguments, but " + std::to_string(arguments.size()) + " were given",
                                    location));
        }
      }
    }
    if (context().cursor.take().type != TokenType::RParen) {
      return std::unexpected(
          at("expected ')' after the arguments of '" + qualified + "'", context().cursor.location()));
    }

    // A template call names the instantiation it deduced, which the item pass will
    // turn into a function of its own before the program ships.
    SemType return_type;
    std::size_t callee = 0;
    if (deduced) {
      auto resolved = context().resolve_type(tmpl->return_type, type_args, location);
      if (!resolved) {
        return std::unexpected(resolved.error());
      }
      return_type = *resolved;
      const auto instantiated = context().instantiate(tmpl, type_args, location);
      if (!instantiated) {
        return std::unexpected(instantiated.error());
      }
      callee = *instantiated;
    } else {
      return_type = function->return_type;
      callee = function->id;
    }
    if (implicit_receiver && function && !arguments.empty()) {
      arguments[0].type = function->params[0];
    }
    last_callee_ = callee;
    const Type lowered = lower(return_type);
    const std::uint64_t destination = lowered == Type::Void ? 0 : context().builder.alloc(lowered);
    std::vector<Operand> args;
    args.push_back(lowered == Type::Void ? Operand::empty() : Operand::vreg(lowered, destination));
    args.push_back(Operand::function(lowered, callee));
    for (const Value& argument : arguments) {
      if (auto valid = context().validate_origins(argument, location); !valid) {
        return std::unexpected(valid.error());
      }
      args.push_back(argument.as_operand());
      context().transfer_value(argument);
    }
    context().builder.emit(Op::Call, std::move(args));
    if (auto propagated = context().propagate_call_origins(arguments, location); !propagated) {
      return std::unexpected(propagated.error());
    }
    if (!implicit_receiver && (deduced ? tmpl->invalidates_references : function->invalidates_references)) {
      for (const auto& argument : arguments) {
        if (is_mutable_record_reference(argument.type)) {
          context().invalidate_interior(argument);
        }
      }
    }
    if (implicit_receiver) {
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (is_mutable_record_reference(argument.type)) {
          context().invalidate_interior(argument);
        }
      }
    }

    if (lowered == Type::Void) {
      return Value {SemType::Void, Operand::empty(), std::nullopt};
    }
    Value result {return_type, Operand::vreg(lowered, destination)};
    if (return_type.is_reference() || return_type.is_pointer() || type_has_references(return_type)) {
      for (const auto& argument : arguments) {
        result.origins.insert(result.origins.end(), argument.origins.begin(), argument.origins.end());
        if (return_type.is_reference() && argument.type.is_reference() && argument.type.pointee->is_record() &&
            *return_type.pointee != *argument.type.pointee) {
          const auto interior = context().interior_origins(argument);
          result.origins.insert(result.origins.end(), interior.begin(), interior.end());
        }
      }
      result.object_mutable = return_type.is_reference() && return_type.mutable_reference;
    }
    context().track_temporary(result);
    return result;
  }

  /// Deduces the template \p tmpl's type arguments from argument types \p types,
  /// or returns the first error the deduction meets. A concrete parameter demands
  /// its type exactly; a type parameter binds the argument's type, all its
  /// occurrences agreeing, and must satisfy the parameter's capability bounds.
  std::optional<ParseError> ExpressionParser::deduce(const TemplateInfo* tmpl, const std::vector<SemType>& types,
                                                     SourceLocation location, std::vector<SemType>& type_args) {
    if (types.size() != tmpl->params.size()) {
      return ParseError {"'" + tmpl->name + "' takes " + std::to_string(tmpl->params.size()) + " arguments, but " +
                             std::to_string(types.size()) + " were given",
                         location};
    }
    type_args.assign(tmpl->type_params.size(), SemType::Void);
    std::vector<bool> bound(tmpl->type_params.size(), false);
    const auto match = [&](const auto& self, const TemplateType& pattern, const SemType& actual,
                           std::size_t index) -> std::optional<ParseError> {
      if (pattern.pointee) {
        if (pattern.reference ? (!actual.is_reference() || actual.mutable_reference != pattern.mutable_reference) :
                                !actual.is_pointer()) {
          return ParseError {"argument " + std::to_string(index + 1) + " of '" + tmpl->name +
                                 (pattern.reference ? "' needs a matching reference" : "' needs a pointer"),
                             location};
        }
        return self(self, *pattern.pointee, *actual.pointee, index);
      }
      if (pattern.record_template) {
        if (!actual.is_record() || actual.record->template_origin != pattern.record_template ||
            actual.record->type_args.size() != pattern.arguments.size()) {
          return ParseError {"argument " + std::to_string(index + 1) + " of '" + tmpl->name + "' needs " +
                                 pattern.record_template->name,
                             location};
        }
        for (std::size_t slot = 0; slot < pattern.arguments.size(); ++slot) {
          if (auto error = self(self, pattern.arguments[slot], actual.record->type_args[slot], index)) {
            return error;
          }
        }
        return std::nullopt;
      }
      if (pattern.param >= 0) {
        const std::size_t slot = static_cast<std::size_t>(pattern.param);
        if (bound[slot] && type_args[slot] != actual) {
          return ParseError {"cannot deduce '" + tmpl->type_params[slot].name + "' of '" + tmpl->name +
                                 "': the arguments give " + to_string(type_args[slot]) + " and " + to_string(actual),
                             location};
        }
        type_args[slot] = actual;
        bound[slot] = true;
      } else if (actual != pattern.type) {
        return ParseError {"argument " + std::to_string(index + 1) + " of '" + tmpl->name + "' is " +
                               to_string(pattern.type) + ", not " + to_string(actual),
                           location};
      }
      return std::nullopt;
    };
    for (std::size_t index = 0; index < types.size(); ++index) {
      if (auto error = match(match, tmpl->params[index], types[index], index)) {
        return error;
      }
    }
    for (std::size_t slot = 0; slot < tmpl->type_params.size(); ++slot) {
      if (!bound[slot]) {
        continue;
      }
      for (const Capability capability : tmpl->type_params[slot].bounds) {
        if (!satisfies(type_args[slot], capability)) {
          return ParseError {std::string {to_string(type_args[slot])} + " does not satisfy " +
                                 std::string {to_string(capability)} + " (required by '" +
                                 tmpl->type_params[slot].name + "' of '" + tmpl->name + "')",
                             location};
        }
        if (capability == Capability::Clonable && type_args[slot].is_record()) {
          auto clone = context().ensure_clone(type_args[slot].record, location);
          if (!clone) {
            return clone.error();
          }
        }
      }
    }
    return std::nullopt;
  }

  ExpressionParser::result ExpressionParser::typed_allocation(SourceLocation location) {
    auto& ctx = context();
    if (const_mode_) {
      return std::unexpected(at("allocation is not a constant expression", location));
    }
    auto types = ctx.read_type_arguments(ctx.cursor);
    if (!types) {
      return std::unexpected(types.error());
    }
    if (types->size() != 1 || types->front() == SemType::Void) {
      return std::unexpected(at("alloc requires one non-void element type", location));
    }
    if (auto open = expect(ctx.cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    auto count = expression(SemType::U64);
    if (!count) {
      return count;
    }
    if (count->type != SemType::U64) {
      return std::unexpected(at("alloc count must have type u64", location));
    }
    if (auto close = expect(ctx.cursor, TokenType::RParen); !close) {
      return std::unexpected(close.error());
    }
    const auto raw_type = primitives::lower(primitives::SigType::Ptr);
    const auto raw = Operand::vreg(raw_type, ctx.builder.alloc(raw_type));
    ctx.builder.emit(Op::Primitive, {raw, count->as_operand()},
                     static_cast<std::uint32_t>(primitives::Prim::AllocCells));
    return cast_to(Value {SemType::pointer(SemType::U8), raw}, SemType::pointer(types->front()), location);
  }

  ExpressionParser::result ExpressionParser::primitive_call(const std::string& name, SourceLocation location) {
    if (const_mode_) {
      return std::unexpected(at("a constant expression may not call a primitive", location));
    }
    const primitives::Signature* signature = primitives::by_name(name);
    if (!signature) {
      return std::unexpected(at("no primitive named '" + name + "'", location));
    }

    std::vector<Operand> args;
    std::vector<ReferenceOrigin> pointer_origins;
    if (!context().cursor.at(TokenType::RParen)) {
      while (true) {
        // A fixed parameter types its argument the way a call's does, so a lone
        // literal takes the parameter's type; a variadic value has none to take
        // and is left untyped, exactly as an argument past the last parameter is.
        const std::optional<SemType> expected =
            signature->id != primitives::Prim::Free && args.size() < signature->param_count ?
            std::optional<SemType> {sem_of(signature->params[args.size()])} :
            std::nullopt;
        auto argument = expression(expected);
        if (!argument) {
          return argument;
        }
        if (argument->type == SemType::Void) {
          return std::unexpected(at("'" + name + "' cannot take a value of type void", location));
        }
        if (expected && argument->type != *expected) {
          return std::unexpected(at("argument " + std::to_string(args.size() + 1) + " of '" + name + "' is " +
                                        std::string {to_string(*expected)} + ", not " +
                                        std::string {to_string(argument->type)},
                                    location));
        }
        if (argument->type.is_record() || argument->type.is_reference()) {
          return std::unexpected(at("primitives cannot consume records or safe references", location));
        }
        if (argument->type.is_pointer()) {
          pointer_origins.insert(pointer_origins.end(), argument->origins.begin(), argument->origins.end());
        }
        if (signature->id == primitives::Prim::Free && args.empty()) {
          if (!argument->type.is_pointer()) {
            return std::unexpected(at("free requires a raw pointer", location));
          }
          argument = cast_to(*argument, SemType::pointer(SemType::U8), location);
          if (!argument) {
            return argument;
          }
        }
        auto snapshot = context().copy_value(*argument, location, true);
        if (!snapshot) {
          return snapshot;
        }
        args.push_back(snapshot->as_operand());
        if (!context().cursor.match(TokenType::Comma)) {
          break;
        }
      }
    }
    if (context().cursor.take().type != TokenType::RParen) {
      return std::unexpected(at("expected ')' after the arguments of '" + name + "'", context().cursor.location()));
    }
    if (!signature->variadic_values && args.size() != signature->param_count) {
      return std::unexpected(at("'" + name + "' takes " + std::to_string(signature->param_count) + " arguments, but " +
                                    std::to_string(args.size()) + " were given",
                                location));
    }

    // The result slot comes first: `EMPTY` when the primitive returns nothing,
    // a fresh cell of its return type otherwise.
    const Type return_type = primitives::lower(signature->return_type);
    const std::uint64_t destination = return_type == Type::Void ? 0 : context().builder.alloc(return_type);
    args.insert(args.begin(), return_type == Type::Void ? Operand::empty() : Operand::vreg(return_type, destination));
    context().builder.emit(Op::Primitive, std::move(args), static_cast<std::uint32_t>(signature->id));

    if (return_type == Type::Void) {
      return Value {SemType::Void, Operand::empty(), std::nullopt};
    }
    Value result {sem_of(signature->return_type), Operand::vreg(return_type, destination), std::nullopt};
    if (result.type.is_pointer()) {
      result.origins = std::move(pointer_origins);
    }
    return result;
  }

  ExpressionParser::result ExpressionParser::dereference(const Value& value, SourceLocation location) {
    if (!value.type.is_pointer() && !value.type.is_reference()) {
      return std::unexpected(at("'*' needs a pointer, but got " + std::string {to_string(value.type)}, location));
    }
    SemType pointee = *value.type.pointee;
    if (value.type.is_reference() && !value.type.mutable_reference && pointee.is_reference() &&
        pointee.mutable_reference) {
      pointee = SemType::reference(*pointee.pointee, false);
    }
    const Type lowered = lower(pointee);
    const std::uint64_t destination = context().builder.alloc(lowered);
    context().builder.emit(Op::Load, {Operand::vreg(lowered, destination), value.as_operand()});
    Value result {pointee, Operand::vreg(lowered, destination), std::nullopt, true};
    result.place_address = value.as_operand();
    result.object_mutable = value.type.is_pointer() || value.type.mutable_reference;
    result.raw_storage = value.type.is_pointer();
    result.receiver_projection = value.receiver_projection && !pointee.is_reference();
    result.origins = value.origins;
    return result;
  }

  ExpressionParser::result ExpressionParser::string_literal(const Token& token) {
    auto* record = context().find_root_record("std.string");
    if (!record) {
      return std::unexpected(at("string literals require std.string", token.location));
    }
    if (const_mode_) {
      return std::unexpected(at("a string literal requires runtime allocation", token.location));
    }
    const SemType type = SemType::record_type(record);
    const SemType receiver = SemType::reference(type, true);
    const auto method = [&](const char* name, const std::vector<SemType>& params,
                            SemType return_type) -> const FunctionInfo* {
      if (auto group = record->methods.find(name); group != record->methods.end()) {
        for (const auto& entry : group->second) {
          if (entry.is_public && entry.function.params == params && entry.function.return_type == return_type) {
            return &entry.function;
          }
        }
      }
      return nullptr;
    };
    const auto* factory = method("new", {}, type);
    const auto* reserve = method("reserve", {receiver, SemType::U64}, SemType::Void);
    const auto* push = method("push", {receiver, SemType::Char}, SemType::Void);
    if (!factory || !reserve || !push) {
      return std::unexpected(at("string literals require std.string.new, reserve and push", token.location));
    }

    std::vector<std::uint64_t> bytes;
    std::string_view raw = token.text;
    while (!raw.empty()) {
      if (raw.front() != '\\') {
        bytes.push_back(static_cast<unsigned char>(raw.front()));
        raw.remove_prefix(1);
        continue;
      }
      const char escape = raw[1];  // The tokenizer checks that the escape has at least one hex digit.
      const std::size_t size = escape == 'x' ? 4 : escape == 'u' ? 6 : 2;
      auto code = parse_char(raw.substr(0, size));
      if (!code) {
        return std::unexpected(at(code.error(), token.location));
      }
      raw.remove_prefix(size);
      if (escape != 'u' || *code < 0x80) {
        bytes.push_back(*code);
      } else if (*code < 0x800) {
        bytes.push_back(0xc0 | (*code >> 6));
        bytes.push_back(0x80 | (*code & 0x3f));
      } else {
        if (*code >= 0xd800 && *code <= 0xdfff) {
          return std::unexpected(at("a Unicode escape must name a non-surrogate code point", token.location));
        }
        bytes.push_back(0xe0 | (*code >> 12));
        bytes.push_back(0x80 | ((*code >> 6) & 0x3f));
        bytes.push_back(0x80 | (*code & 0x3f));
      }
    }

    auto& builder = context().builder;
    const auto slot = Operand::vreg(lower(type), builder.alloc(lower(type)));
    builder.emit(Op::Call, {slot, Operand::function(lower(type), factory->id)});
    Value value {type, slot};
    context().track_temporary(value);
    if (!bytes.empty()) {
      const auto address = Operand::vreg(lower(receiver), builder.alloc(lower(receiver)));
      builder.emit(Op::Addr, {address, slot});
      builder.emit(Op::Call,
                   {Operand::empty(), Operand::function(Type::Void, reserve->id), address,
                    Operand::instant(Type::U64, bytes.size())});
      for (auto byte : bytes) {
        builder.emit(Op::Call,
                     {Operand::empty(), Operand::function(Type::Void, push->id), address,
                      Operand::instant(lower(SemType::Char), byte)});
      }
    }
    return value;
  }

  ExpressionParser::result ExpressionParser::primary(std::optional<SemType> expected, bool negated) {
    TokenCursor& cursor = context().cursor;
    const SourceLocation location = cursor.location();
    const Token token = cursor.current();

    switch (token.type) {
      case TokenType::LBrace: return object_literal(expected);
      case TokenType::Number: {
        cursor.take();
        return integer_literal(token, expected, negated);
      }
      case TokenType::Char: {
        cursor.take();
        auto code = parse_char(token.text);
        if (!code) {
          return std::unexpected(at(code.error(), token.location));
        }
        if (*code > 255) {
          return std::unexpected(at("character literal is not a 0-255 code point", token.location));
        }
        const ConstInt constant = ConstInt::of(static_cast<std::int64_t>(*code), Type::U8);
        return Value {SemType::Char, Operand::instant(Type::U8, constant.bits), constant};
      }
      case TokenType::String: cursor.take(); return string_literal(token);
      case TokenType::Identifier: break;
      case TokenType::LParen: {
        cursor.take();
        auto inner = or_expression(expected);
        if (!inner) {
          return inner;
        }
        if (cursor.take().type != TokenType::RParen) {
          return std::unexpected(at("expected ')'", cursor.location()));
        }
        return inner;
      }
      default: return std::unexpected(at("expected an expression", location));
    }

    // An identifier is a name: a primitive call, a call, a local, or a global.
    const std::string name = token.text;
    const Keyword keyword = keyword_of(name);
    if (keyword == Keyword::Null) {
      cursor.take();
      if (!expected || !expected->is_pointer()) {
        return std::unexpected(at("null requires a contextual raw pointer type", location));
      }
      const auto type = lower(*expected);
      return Value {*expected, Operand::instant(type, 0), ConstInt::of(0, type)};
    }
    if (keyword == Keyword::True || keyword == Keyword::False) {
      cursor.take();
      // A `bool` is a value of its own type: the expected type does not reach it,
      // so a position wanting an integer is left to report the mismatch itself.
      const std::int64_t value = keyword == Keyword::True ? 1 : 0;
      const ConstInt constant = ConstInt::of(value, Type::I8);
      return Value {SemType::Bool, Operand::instant(Type::I8, constant.bits), constant};
    }
    if (keyword == Keyword::Primitive) {
      // `primitive name(…)` is the only thing the keyword introduces
      // (`docs/primitive.md#calling`). A dot would be the start of a namespace,
      // and v1 has none: a name is one identifier.
      cursor.take();
      if (cursor.at(TokenType::Dot)) {
        return std::unexpected(at("'.' is not part of a name in v1", location));
      }
      if (!cursor.at(TokenType::Identifier)) {
        return std::unexpected(at("expected a primitive name after 'primitive'", cursor.location()));
      }
      const Token callee = cursor.take();
      if (cursor.at(TokenType::Dot)) {
        return std::unexpected(at("'.' is not part of a name in v1", location));
      }
      if (!cursor.at(TokenType::LParen)) {
        return std::unexpected(
            at("expected '(' after primitive '" + std::string {callee.text} + "'", cursor.location()));
      }
      cursor.take();
      return primitive_call(callee.text, location);
    }
    if (keyword != Keyword::None) {
      return std::unexpected(at("'" + name + "' is a reserved word", location));
    }

    if (const LocalInfo* local = context().find_local(name)) {
      cursor.take();
      if (cursor.at(TokenType::LParen)) {
        return std::unexpected(at("value '" + name + "' is not callable", location));
      }
      if (const_mode_) {
        return std::unexpected(at("a constant expression may not read a local", location));
      }
      auto value = context().local_value(*local, location);
      if (value && context().conditional_receiver == local->flow_id) {
        value->receiver_projection = true;
      }
      return value;
    }
    if (name == "alloc" && cursor.peek().type == TokenType::Operator && cursor.peek().text == "<" &&
        !context().find_global(name)) {
      cursor.take();
      return typed_allocation(location);
    }
    {
      auto scan = cursor;
      auto named = context().read_type(scan);
      if (named && named->is_record() && scan.at(TokenType::Dot)) {
        scan.take();
        auto member = expect_identifier(scan);
        if (!member) {
          return std::unexpected(member.error());
        }
        if (scan.at(TokenType::LParen)) {
          cursor.seek(scan.index() + 1);
          return record_call(*named->record, *member, location);
        }
      }
    }

    // An identifier begins a name. A dotted name is a namespace-qualified reference
    // (`std.print`): the leading segments are a namespace path and the last is the
    // name itself. There are no members, so no other dotted name exists.
    std::vector<std::string> parts;
    parts.push_back(name);
    cursor.take();
    while (cursor.at(TokenType::Dot)) {
      const GlobalInfo* object = nullptr;
      if (parts.size() == 1) {
        object = context().find_global(parts.front());
      } else {
        const std::vector<std::string> path(parts.begin(), parts.end() - 1);
        auto space = context().resolve_namespace(path, location);
        if (space) {
          object = context().find_global_in(**space, parts.back());
        }
      }
      if (object) {
        Value value {object->sem_type, Operand::global(object->type, object->id), std::nullopt, true};
        value.object_mutable = object->mutable_object;
        value.global_storage = true;
        return value;
      }
      cursor.take();
      auto segment = expect_identifier(cursor);
      if (!segment) {
        return std::unexpected(segment.error());
      }
      parts.push_back(*segment);
    }
    const std::string qualified = qualified_path(parts);

    // A bare name searches the current namespace outward; a qualified name means
    // exactly the namespace it spells. The `NameSpace` even a bare name resolves to
    // is fixed once, so the lookups below agree.
    const NameSpace* ns = nullptr;
    if (parts.size() > 1) {
      const std::vector<std::string> path(parts.begin(), parts.end() - 1);
      auto resolved = context().resolve_namespace(path, location);
      if (!resolved) {
        return std::unexpected(resolved.error());
      }
      ns = *resolved;
    }
    const std::string& leaf = parts.back();
    const std::vector<FunctionInfo>* candidates =
        ns != nullptr ? context().find_functions_in(*ns, leaf) : context().find_functions(leaf);
    const TemplateInfo* tmpl = ns != nullptr ? context().find_template_in(*ns, leaf) : context().find_template(leaf);

    if (cursor.at(TokenType::LParen)) {
      if (parts.size() == 1 && leaf == "free" && candidates == nullptr && tmpl == nullptr &&
          !context().find_global(leaf)) {
        cursor.take();
        return primitive_call("free", location);
      }
      if (candidates == nullptr && tmpl == nullptr) {
        return std::unexpected(at("call to undeclared function '" + qualified + "'", location));
      }
      cursor.take();
      return call(qualified, candidates, tmpl, location);
    }
    if (candidates != nullptr || tmpl != nullptr) {
      return std::unexpected(at("'" + qualified + "' is a function and can only be called", location));
    }

    const GlobalInfo* global = ns != nullptr ? context().find_global_in(*ns, leaf) : context().find_global(leaf);
    if (global != nullptr) {
      if (const_mode_) {
        if (!global->value) {
          return std::unexpected(at("'" + qualified + "' is declared after this initializer", location));
        }
        return Value {global->sem_type, Operand::global(global->type, global->id), global->value, true};
      }
      Value value {global->sem_type, Operand::global(global->type, global->id), std::nullopt, true};
      value.object_mutable = global->mutable_object;
      value.global_storage = true;
      return value;
    }

    // A name may name a namespace rather than a value: say so, rather than call it
    // undeclared.
    const NameSpace* nested = ns != nullptr ? context().find_namespace(*ns, leaf) : context().find_namespace(leaf);
    if (nested != nullptr) {
      return std::unexpected(at("'" + qualified + "' is a namespace, not a value", location));
    }
    return std::unexpected(at("undeclared identifier '" + qualified + "'", location));
  }

}  // namespace erax::frontend
