#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "ParseContext.hpp"
#include "SemType.hpp"
#include "Value.hpp"

namespace erax::frontend {

  /// Lowers expressions straight to JIR, in one block, with no AST and no
  /// exceptions: every step returns `std::expected` and errors travel with `?`.
  ///
  /// \p expected is the contextual type of the position being filled — the
  /// declared type of a `let`, the target type of an assignment, a parameter's
  /// type in a call. An inferred local supplies no expected type. Arithmetic
  /// threads a contextual integer type to its first lone operand; comparisons
  /// and logical operators infer their own operand types.
  class ExpressionParser {
public:
    explicit ExpressionParser(ParseContext& context) noexcept : context_(&context) {}

    using result = std::expected<Value, ParseError>;

    /// An expression in a position that expects \p expected (or none, in which
    /// case an unsuffixed literal is an `i32`).
    result expression(std::optional<SemType> expected);
    /// An expression that must fold: no calls, no local reads, nothing that
    /// depends on a cell's value at run time.
    result const_expression(SemType expected);

private:
    result or_expression(std::optional<SemType> expected);
    result logxor_expression(std::optional<SemType> expected);
    result and_expression(std::optional<SemType> expected);
    result bitor_expression(std::optional<SemType> expected);
    result bitxor_expression(std::optional<SemType> expected);
    result bitand_expression(std::optional<SemType> expected);
    result equality_expression(std::optional<SemType> expected);
    result relational_expression(std::optional<SemType> expected);
    result shift_expression(std::optional<SemType> expected);
    result additive_expression(std::optional<SemType> expected);
    result multiplicative_expression(std::optional<SemType> expected);
    result cast_expression(std::optional<SemType> expected);
    result unary_expression(std::optional<SemType> expected);
    result primary(std::optional<SemType> expected, bool negated = false);
    result postfix(Value value);
    result object_literal(std::optional<SemType> expected);
    result record_call(RecordInfo& record, const std::string& name, SourceLocation location,
                       std::optional<Value> receiver = std::nullopt);

    /// One level of a left-associative binary operator loop: parse an operand,
    /// then fold in every following operator of that level.
    using operand_level = result (ExpressionParser::*)(std::optional<SemType>);
    result binary_level(operand_level operand, std::optional<SemType> expected,
                        std::initializer_list<std::string_view> operators, bool thread_expected = false);

    /// Checks the two operands of \p op against each other, picks the JIR op and
    /// the result type, and folds or emits.
    result combine(std::string_view op, Value& left, Value& right, SourceLocation location);

    /// Applies a JIR binary op: two constants fold, anything else emits
    /// `op dst a b`.
    result apply(middleend::Op op, SemType result, const Value& left, const Value& right, SourceLocation location);

    /// Applies a JIR unary op, folding when the operand is a constant.
    result apply_unary(middleend::Op op, SemType type, const Value& operand, SourceLocation location);

    /// One `as target` of a cast chain: converts \p value's type to \p target,
    /// wrapping (`docs/core/types.md#casts`). Same type both ways changes nothing;
    /// a to-`bool` cast is the nonzero compare rather than a `CAST`, since `bool`
    /// is an `i8` in the IR.
    result cast_to(const Value& value, SemType target, SourceLocation location);

    /// Reads an integer literal, typed by \p expected or by its own suffix. A lone
    /// literal takes the type of the position it fills; a literal inside an operator
    /// does not, and is an `i32` unless it says otherwise.
    ///
    /// \p negated says a unary `-` is in front of it, which is the one place a
    /// negative literal is written: `-128i8` is `imin`, and no `i8` literal on its own
    /// is negative.
    result integer_literal(const Token& token, std::optional<SemType> expected, bool negated);

    /// A call of the function or template \p name (spelled \p qualified in a
    /// message) with the arguments at the cursor. \p candidates and \p tmpl are the
    /// declarations the name resolved to — the current namespace outward, or the
    /// one namespace a qualified name named.
    result call(const std::string& qualified, const std::vector<FunctionInfo>* candidates, const TemplateInfo* tmpl,
                SourceLocation location, std::vector<Value> arguments = {}, bool implicit_receiver = false);
    std::size_t last_callee_ = 0;

    /// Deduces the template's type arguments from the argument types read so far,
    /// or returns the first error the deduction meets.
    std::optional<ParseError> deduce(const TemplateInfo* tmpl, const std::vector<SemType>& types,
                                     SourceLocation location, std::vector<SemType>& type_args);

    /// A `primitive name(…)` call: \p name is looked up in the closed catalog
    /// (`docs/primitive.md#catalog`), the arguments are checked against its
    /// signature, and it lowers to one `PRIM` carrying the primitive's id.
    result typed_allocation(SourceLocation location);
    result primitive_call(const std::string& name, SourceLocation location);

    ParseContext& context() noexcept {
      return *context_;
    }
    ParseContext* context_;

    /// Set for the duration of a `const_expression`, which forbids calls and
    /// local reads and requires every fold to succeed.
    bool const_mode_ = false;
  };

}  // namespace erax::frontend
