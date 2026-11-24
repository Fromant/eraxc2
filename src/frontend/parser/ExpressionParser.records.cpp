#include "ExpressionParser.hpp"

#include <algorithm>
#include <utility>

#include "primitives/Primitives.hpp"

namespace erax::frontend {

  using middleend::Op;
  using middleend::Operand;
  using middleend::Type;

  namespace {
    ParseError record_error(std::string message, SourceLocation location) {
      return {std::move(message), location};
    }

    void append_origins(Value& destination, const Value& source) {
      for (const auto& origin : source.origins) {
        if (std::find(destination.origins.begin(), destination.origins.end(), origin) == destination.origins.end()) {
          destination.origins.push_back(origin);
        }
      }
    }
  }

  ExpressionParser::result ExpressionParser::object_literal(std::optional<SemType> expected) {
    auto& ctx = context();
    auto& cursor = ctx.cursor;
    const auto location = cursor.location();
    if (const_mode_ || !expected || !expected->is_record() || !ctx.in_factory ||
        ctx.active_record != expected->record) {
      return std::unexpected(record_error("object literals are only allowed in the type's factory", location));
    }
    RecordInfo& record = *expected->record;
    cursor.take();
    std::vector<std::optional<Value>> fields(record.fields.size());
    std::vector<std::size_t> order;
    while (!cursor.at(TokenType::RBrace)) {
      const auto field_location = cursor.location();
      auto name = expect_identifier(cursor);
      if (!name) {
        return std::unexpected(name.error());
      }
      const auto found = std::find_if(record.fields.begin(), record.fields.end(), [&](const FieldInfo& field) {
        return field.name == *name;
      });
      if (found == record.fields.end()) {
        return std::unexpected(record_error("unknown field '" + *name + "' of '" + record.name + "'", field_location));
      }
      const auto index = static_cast<std::size_t>(found - record.fields.begin());
      if (fields[index]) {
        return std::unexpected(record_error("field '" + *name + "' is initialized twice", field_location));
      }
      if (auto colon = expect(cursor, TokenType::Colon); !colon) {
        return std::unexpected(colon.error());
      }
      auto value = expression(found->type);
      if (!value) {
        return value;
      }
      if (value->type != found->type) {
        return std::unexpected(
            record_error("field '" + *name + "' needs " + to_string(found->type) + ", not " + to_string(value->type),
                         field_location));
      }
      auto copied = ctx.copy_value(*value, field_location, true);
      if (!copied) {
        return copied;
      }
      fields[index] = *copied;
      order.push_back(index);
      if (!cursor.match(TokenType::Comma)) {
        break;
      }
    }
    if (auto close = expect(cursor, TokenType::RBrace); !close) {
      return std::unexpected(close.error());
    }
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (!fields[index]) {
        return std::unexpected(record_error("field '" + record.fields[index].name + "' is not initialized", location));
      }
    }
    const auto bytes_pointer = Type::pointer(Type::U8);
    const auto storage = Operand::vreg(bytes_pointer, ctx.builder.alloc(bytes_pointer));
    ctx.builder.emit(Op::Primitive, {storage, Operand::instant(Type::U64, std::max<std::size_t>(1, fields.size()) * 8)},
                     static_cast<std::uint32_t>(primitives::Prim::Alloc));
    const auto destination = Operand::vreg(lower(*expected), ctx.builder.alloc(lower(*expected)));
    ctx.builder.emit(Op::Cast, {destination, storage});
    Value result {*expected, destination};
    result.object_mutable = true;
    for (const auto index : order) {
      if (auto valid = ctx.validate_origins(*fields[index], location); !valid) {
        return std::unexpected(valid.error());
      }
      const auto address = Operand::vreg(bytes_pointer, ctx.builder.alloc(bytes_pointer));
      ctx.builder.emit(Op::Offset, {address, storage, Operand::instant(Type::U64, index)});
      const auto typed = Type::pointer(lower(record.fields[index].type));
      const auto field_address = Operand::vreg(typed, ctx.builder.alloc(typed));
      ctx.builder.emit(Op::Cast, {field_address, address});
      ctx.builder.emit(Op::Store, {field_address, fields[index]->as_operand()});
      append_origins(result, *fields[index]);
      ctx.transfer_value(*fields[index]);
    }
    ctx.track_temporary(result);
    return result;
  }

  ExpressionParser::result ExpressionParser::record_call(RecordInfo& record, const std::string& name,
                                                         SourceLocation location, std::optional<Value> receiver) {
    auto& ctx = context();
    std::optional<std::size_t> generated_clone;
    if (name == "clone") {
      auto clone = ctx.ensure_clone(&record, location);
      if (!clone) {
        return std::unexpected(clone.error());
      }
      generated_clone = *clone;
    }
    const auto found = record.methods.find(name);
    if (found == record.methods.end() && generated_clone && receiver) {
      if (auto close = expect(ctx.cursor, TokenType::RParen); !close) {
        return std::unexpected(close.error());
      }
      Value place = *receiver;
      place.addressable = true;
      return ctx.copy_value(place, location);
    }
    if (found == record.methods.end()) {
      return std::unexpected(record_error("unknown method '" + record.name + "." + name + "'", location));
    }
    std::vector<FunctionInfo> candidates;
    bool mut = false;
    for (const auto& method : found->second) {
      if (!method.is_public && ctx.active_record != &record) {
        continue;
      }
      const bool instance = !method.param_names.empty() && method.param_names.front() == "this";
      if (instance != receiver.has_value()) {
        continue;
      }
      if (instance && !method.function.params.empty() && method.function.params.front().mutable_reference &&
          receiver->object_mutable) {
        mut = true;
      }
    }
    bool invalidates = false;
    for (const auto& method : found->second) {
      if (!method.is_public && ctx.active_record != &record) {
        continue;
      }
      const bool instance = !method.param_names.empty() && method.param_names.front() == "this";
      if (instance != receiver.has_value()) {
        continue;
      }
      auto candidate = method.function;
      if (method.receiver_result && receiver->object_mutable) {
        candidate.return_type = SemType::reference(*candidate.return_type.pointee, true);
      }
      if (!instance) {
        candidate.invalidates_references = std::ranges::any_of(candidate.params, [](const SemType& type) {
          return is_mutable_record_reference(type);
        });
      }
      candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) {
      return std::unexpected(
          record_error("method '" + record.name + "." + name + "' is private or needs a different receiver", location));
    }
    std::vector<Value> arguments;
    if (receiver) {
      // A temporary may receive a method call within its full expression, but a
      // reference returned from it cannot escape that expression.
      Value place = *receiver;
      place.addressable = true;
      auto borrowed = ctx.borrow_value(place, mut, location, true);
      if (!borrowed) {
        return borrowed;
      }
      arguments.push_back(*borrowed);
    }
    auto result =
        call(record.name + "." + name, &candidates, nullptr, location, std::move(arguments), receiver.has_value());
    if (!result) {
      return result;
    }
    if (receiver && receiver->global_storage && type_has_references(result->type)) {
      return std::unexpected(record_error("safe references to global storage are not supported", location));
    }
    for (const auto& method : found->second) {
      if (method.function.id == last_callee_) {
        invalidates = method.invalidates_references;
      }
    }
    if (receiver && invalidates) {
      ctx.invalidate_interior(*receiver);
    }
    if (receiver && result->type.is_reference()) {
      if (!receiver->addressable) {
        return std::unexpected(record_error("cannot retain a reference returned from a temporary receiver", location));
      }
      for (const auto& origin : ctx.interior_origins(*receiver)) {
        if (std::find(result->origins.begin(), result->origins.end(), origin) == result->origins.end()) {
          result->origins.push_back(origin);
        }
      }
    }
    return result;
  }

  ExpressionParser::result ExpressionParser::postfix(Value value) {
    auto& ctx = context();
    auto& cursor = ctx.cursor;
    while (cursor.at(TokenType::Dot) && cursor.peek().type == TokenType::Identifier) {
      const auto location = cursor.take().location;
      while (value.type.is_reference()) {
        if (auto valid = ctx.validate_origins(value, location); !valid) {
          return std::unexpected(valid.error());
        }
        auto type = *value.type.pointee;
        if (!value.type.mutable_reference && type.is_reference() && type.mutable_reference) {
          type = SemType::reference(*type.pointee, false);
        }
        const auto cell = Operand::vreg(lower(type), ctx.builder.alloc(lower(type)));
        ctx.builder.emit(Op::Load, {cell, value.as_operand()});
        Value read {type, cell, std::nullopt, true};
        read.place_address = value.as_operand();
        read.object_mutable = value.type.mutable_reference;
        read.origins = value.origins;
        read.receiver_projection = value.receiver_projection && !type.is_reference();
        value = std::move(read);
      }
      if (!value.type.is_record()) {
        return std::unexpected(record_error("member access needs a structure, not " + to_string(value.type), location));
      }
      RecordInfo& record = *value.type.record;
      auto name = expect_identifier(cursor);
      if (!name) {
        return std::unexpected(name.error());
      }
      if (cursor.match(TokenType::LParen)) {
        auto called = record_call(record, *name, location, value);
        if (!called) {
          return called;
        }
        value = *called;
        continue;
      }
      const auto field = std::find_if(record.fields.begin(), record.fields.end(), [&](const FieldInfo& candidate) {
        return candidate.name == *name;
      });
      if (field == record.fields.end()) {
        return std::unexpected(record_error("unknown field '" + record.name + "." + *name + "'", location));
      }
      if (!field->is_public && ctx.active_record != &record) {
        return std::unexpected(record_error("field '" + record.name + "." + *name + "' is private", location));
      }
      const auto address = Operand::vreg(lower(value.type), ctx.builder.alloc(lower(value.type)));
      ctx.builder.emit(Op::Offset,
                       {address, value.as_operand(), Operand::instant(Type::U64, field - record.fields.begin())});
      auto type = field->type;
      if (type.is_reference() && type.mutable_reference && !value.object_mutable) {
        type = SemType::reference(*type.pointee, false);
      }
      const auto pointer_type = Type::pointer(lower(type));
      const auto pointer = Operand::vreg(pointer_type, ctx.builder.alloc(pointer_type));
      ctx.builder.emit(Op::Cast, {pointer, address});
      const auto cell = Operand::vreg(lower(type), ctx.builder.alloc(lower(type)));
      ctx.builder.emit(Op::Load, {cell, pointer});
      Value member {type, cell, std::nullopt, true};
      member.place_address = pointer;
      member.object_mutable = value.object_mutable;
      member.origins = value.origins;
      member.place_origins = ctx.interior_origins(value);
      member.global_storage = value.global_storage;
      member.receiver_projection = value.receiver_projection && !type.is_reference();
      value = std::move(member);
    }
    return value;
  }

}  // namespace erax::frontend
