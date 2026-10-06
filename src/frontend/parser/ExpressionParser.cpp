#include "ExpressionParser.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "ConstantRange.hpp"
#include "middleend/jir/Fold.hpp"

namespace erax::frontend {

  using middleend::Op;
  using middleend::Operand;
  using middleend::Type;

  namespace {

    ParseError at(std::string message, SourceLocation location) {
      return ParseError {std::move(message), location};
    }

    bool is_radix_letter(char c) noexcept {
      return c == 'x' || c == 'X' || c == 'b' || c == 'B' || c == 'o' || c == 'O';
    }

    /// A `Number` token is a float when it has a fraction or an exponent. A radix
    /// literal is never one, even though `0xE` holds an `E`.
    bool is_float_literal(std::string_view text) noexcept {
      if (text.size() > 2 && text[0] == '0' && is_radix_letter(text[1]))
        return false;
      return text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
          text.find('E') != std::string_view::npos;
    }

    int radix_of(char c) noexcept {
      if (c == 'x' || c == 'X')
        return 16;
      if (c == 'o' || c == 'O')
        return 8;
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
        if (c == '_')
          continue;
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
      if (raw.size() == 1)
        return static_cast<std::uint64_t>(static_cast<unsigned char>(raw[0]));
      if (raw[0] != '\\' || raw.size() < 2)
        return std::unexpected("expected a character literal");
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
        value = value * 16 +
            (c <= '9' ? static_cast<std::uint64_t>(c - '0') : static_cast<std::uint64_t>(c | 0x20) - 'a' + 10);
      }
      return value;
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
    if (!value)
      return value;
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
    return binary_level(&ExpressionParser::unary_expression, expected, {"*", "/", "%"}, true);
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
    } else if (!thread_expected && lone) {
      first_expected = expected;  // Thread for comparisons/logical only when lone
    } else {
      first_expected = std::nullopt;
    }
    auto first = (this->*operand)(first_expected);
    if (!first)
      return first;
    Value left = *first;

    while (true) {
      std::string_view op;
      for (const std::string_view candidate : operators) {
        if (context().cursor.at_op(candidate)) {
          op = candidate;
          break;
        }
      }
      if (op.empty())
        break;

      const SourceLocation location = context().cursor.location();
      context().cursor.take();

      // Each operand takes the type of the operand before it, so `x + 1` types the
      // `1` as whatever `x` is.
      auto parsed = (this->*operand)(left.type);
      if (!parsed)
        return parsed;
      Value right = *parsed;

      auto combined = combine(op, left, right, location);
      if (!combined)
        return std::unexpected(combined.error());
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
    if (left.type != right.type) {
      return std::unexpected(at(quoted + " needs two operands of the same type, but got " +
                                    std::string {to_string(left.type)} + " and " + std::string {to_string(right.type)},
                                location));
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
    if (op == "<")
      return apply(Op::CmpLt, SemType::Bool, left, right, location);
    if (op == "<=")
      return apply(Op::CmpLe, SemType::Bool, left, right, location);
    if (op == ">")
      return apply(Op::CmpGt, SemType::Bool, left, right, location);
    if (op == ">=")
      return apply(Op::CmpGe, SemType::Bool, left, right, location);

    if (!is_integer(left.type)) {
      return std::unexpected(at(quoted + " is not defined for " + std::string {to_string(left.type)}, location));
    }

    if (op == "|")
      return apply(Op::Or, left.type, left, right, location);
    if (op == "^")
      return apply(Op::Xor, left.type, left, right, location);
    if (op == "&")
      return apply(Op::And, left.type, left, right, location);
    if (op == "+")
      return apply(Op::Add, left.type, left, right, location);
    if (op == "-")
      return apply(Op::Sub, left.type, left, right, location);
    if (op == "*")
      return apply(Op::Mul, left.type, left, right, location);
    // Signedness comes from the operand's type, which is a real type now: there is no
    // placeholder left for it to be guessed from.
    const bool arithmetic = is_signed(left.type);
    if (op == "/")
      return apply(arithmetic ? Op::Sdiv : Op::Udiv, left.type, left, right, location);
    if (op == "%")
      return apply(arithmetic ? Op::Smod : Op::Umod, left.type, left, right, location);
    if (op == "<<")
      return apply(Op::Shl, left.type, left, right, location);
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
      if (!folded)
        return std::unexpected(at(folded.error(), location));
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
        if (!negated)
          return std::unexpected(at(negated.error(), location));
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
    if (!parsed)
      return std::unexpected(at(parsed.error(), token.location));

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
    std::string_view op;
    if (cursor.at_op("-"))
      op = "-";
    if (cursor.at_op("!"))
      op = "!";
    if (cursor.at_op("~"))
      op = "~";

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
      if (!operand)
        return operand;
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
        const std::optional<SemType> expected = arguments.size() < function->params.size() ?
            std::optional<SemType> {function->params[arguments.size()]} :
            std::nullopt;
        auto argument = expression(expected);
        if (!argument)
          return argument;
        if (expected && argument->type != *expected) {
          return std::unexpected(at("argument " + std::to_string(arguments.size() + 1) + " of '" + name + "' is " +
                                        std::string {to_string(*expected)} + ", not " +
                                        std::string {to_string(argument->type)},
                                    location));
        }
        arguments.push_back(*argument);
        if (!context().cursor.match(TokenType::Comma))
          break;
      }
    }
    if (arguments.size() != function->params.size()) {
      return std::unexpected(at("'" + name + "' takes " + std::to_string(function->params.size()) + " arguments, but " +
                                    std::to_string(arguments.size()) + " were given",
                                location));
    }
    if (context().cursor.take().type != TokenType::RParen) {
      return std::unexpected(at("expected ')' after the arguments of '" + name + "'", context().cursor.location()));
    }

    const Type return_type = lower(function->return_type);
    const std::uint64_t destination = return_type == Type::Void ? 0 : context().builder.alloc(return_type);
    std::vector<Operand> args;
    args.push_back(return_type == Type::Void ? Operand::empty() : Operand::vreg(return_type, destination));
    args.push_back(Operand::function(return_type, function->id));
    for (const Value& argument : arguments)
      args.push_back(argument.as_operand());
    context().builder.emit(Op::Call, std::move(args));

    if (return_type == Type::Void)
      return Value {SemType::Void, Operand::empty(), std::nullopt};
    return Value {function->return_type, Operand::vreg(return_type, destination), std::nullopt};
  }

  ExpressionParser::result ExpressionParser::print_call(SourceLocation location) {
    if (const_mode_) {
      return std::unexpected(at("a constant expression may not call 'print'", location));
    }

    std::vector<Operand> args;
    if (!context().cursor.at(TokenType::RParen)) {
      while (true) {
        // A printed value has no expected type to take, so it is left untyped —
        // exactly as an argument past the last parameter is: the value's own type
        // is what prints.
        auto argument = expression(std::nullopt);
        if (!argument)
          return argument;
        if (argument->type == SemType::Void) {
          return std::unexpected(at("'print' cannot print a value of type void", location));
        }
        args.push_back(argument->as_operand());
        if (!context().cursor.match(TokenType::Comma))
          break;
      }
    }
    if (context().cursor.take().type != TokenType::RParen) {
      return std::unexpected(at("expected ')' after the arguments of 'print'", context().cursor.location()));
    }

    context().builder.emit(Op::Print, std::move(args));
    // `print` is an expression of type void, which is the one type a statement
    // position accepts without complaint.
    return Value {SemType::Void, Operand::empty(), std::nullopt};
  }

  ExpressionParser::result ExpressionParser::primary(std::optional<SemType> expected, bool negated) {
    TokenCursor& cursor = context().cursor;
    const SourceLocation location = cursor.location();
    const Token token = cursor.current();

    switch (token.type) {
      case TokenType::Number: {
        cursor.take();
        return integer_literal(token, expected, negated);
      }
      case TokenType::Char: {
        cursor.take();
        auto code = parse_char(token.text);
        if (!code)
          return std::unexpected(at(code.error(), token.location));
        if (*code > 255) {
          return std::unexpected(at("character literal is not a 0-255 code point", token.location));
        }
        const ConstInt constant = ConstInt::of(static_cast<std::int64_t>(*code), Type::U8);
        return Value {SemType::Char, Operand::instant(Type::U8, constant.bits), constant};
      }
      case TokenType::String: return std::unexpected(at("string is not supported yet", location));
      case TokenType::Identifier: break;
      case TokenType::LParen: {
        cursor.take();
        auto inner = or_expression(expected);
        if (!inner)
          return inner;
        if (cursor.take().type != TokenType::RParen) {
          return std::unexpected(at("expected ')'", cursor.location()));
        }
        return inner;
      }
      default: return std::unexpected(at("expected an expression", location));
    }

    // An identifier is a name: a call, a local, or a global.
    const std::string name = token.text;
    const Keyword keyword = keyword_of(name);
    if (keyword == Keyword::True || keyword == Keyword::False) {
      cursor.take();
      // A `bool` is a value of its own type: the expected type does not reach it,
      // so a position wanting an integer is left to report the mismatch itself.
      const std::int64_t value = keyword == Keyword::True ? 1 : 0;
      const ConstInt constant = ConstInt::of(value, Type::I8);
      return Value {SemType::Bool, Operand::instant(Type::I8, constant.bits), constant};
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
    if (name == "print") {
      // No `fn print` may be declared, so the whole name is the builtin's: a call
      // lowers to `PRINT`, and a bare `print` is the same error a function name
      // that exists gets.
      if (!cursor.at(TokenType::LParen)) {
        return std::unexpected(at("'print' is a builtin function and can only be called", location));
      }
      cursor.take();
      return print_call(location);
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
      return Value {local->sem_type, Operand::vreg(local->type, local->vreg), std::nullopt};
    }
    if (const GlobalInfo* global = context().find_global(name)) {
      if (const_mode_) {
        if (!global->value) {
          return std::unexpected(at("'" + name + "' is declared after this initializer", location));
        }
        return Value {global->sem_type, Operand::global(global->type, global->id), global->value};
      }
      return Value {global->sem_type, Operand::global(global->type, global->id), std::nullopt};
    }
    return std::unexpected(at("undeclared identifier '" + name + "'", location));
  }

}  // namespace erax::frontend
