#include "ProgramParser.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

#include "primitives/Primitives.hpp"

namespace erax::frontend {

  using middleend::CfgBuilder;
  using middleend::Op;
  using middleend::Operand;
  using middleend::Type;

  namespace {
    std::string qualified_path(const std::vector<std::string>& parts) {
      std::string result;
      for (const auto& part : parts) {
        if (!result.empty()) {
          result += '.';
        }
        result += part;
      }
      return result;
    }

    std::string qualified_name(const NameSpace& space, const std::string& name) {
      auto parts = space.path;
      parts.push_back(name);
      return qualified_path(parts);
    }
  }

  ProgramParser::step ProgramParser::discover_records(TokenCursor& cursor, std::vector<std::string> path, bool nested) {
    while (!cursor.at_end() && !(nested && cursor.at(TokenType::RBrace))) {
      if (cursor.at_keyword(Keyword::Namespace)) {
        cursor.take();
        auto name = expect_identifier(cursor);
        if (!name) {
          return std::unexpected(name.error());
        }
        if (auto open = expect(cursor, TokenType::LBrace); !open) {
          return std::unexpected(open.error());
        }
        auto child_path = path;
        child_path.push_back(*name);
        if (auto found = discover_records(cursor, std::move(child_path), true); !found) {
          return found;
        }
        if (cursor.at_end()) {
          return std::unexpected(ParseError {"unterminated namespace body", cursor.location()});
        }
        if (auto close = expect(cursor, TokenType::RBrace); !close) {
          return std::unexpected(close.error());
        }
      } else if (cursor.current().text == "struct") {
        cursor.take();
        const auto location = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name) {
          return std::unexpected(name.error());
        }
        auto qualified = path;
        qualified.push_back(*name);
        auto declared = context_.declare_record(qualified_path(qualified), location);
        if (!declared) {
          return std::unexpected(declared.error());
        }
        auto& record = **declared;
        record.ns_path = path;
        if (cursor.match_op("<")) {
          do {
            auto param_name = expect_identifier(cursor);
            if (!param_name) {
              return std::unexpected(param_name.error());
            }
            TypeParam param {*param_name, {}};
            if (std::ranges::any_of(record.type_params, [&](const TypeParam& p) {
                  return p.name == param.name;
                })) {
              return std::unexpected(ParseError {"duplicate structure type parameter '" + param.name + "'", location});
            }
            if (cursor.at_keyword(Keyword::Extends)) {
              cursor.take();
              do {
                const auto bound = capability_of(cursor.current().text);
                if (!bound) {
                  return std::unexpected(ParseError {"expected a capability", cursor.location()});
                }
                param.bounds.push_back(*bound);
                cursor.take();
              } while (cursor.match_op("&"));
            }
            record.type_params.push_back(std::move(param));
          } while (cursor.match(TokenType::Comma));
          if (!cursor.take_type_close()) {
            return std::unexpected(ParseError {"expected '>'", cursor.location()});
          }
        }
        record.body_index = cursor.index();
        if (auto skipped = skip_body(cursor); !skipped) {
          return skipped;
        }
      } else if (cursor.at_keyword(Keyword::Fn)) {
        while (!cursor.at(TokenType::LBrace) && !cursor.at_end()) {
          cursor.take();
        }
        if (cursor.at_end()) {
          return {};
        }
        if (auto skipped = skip_body(cursor); !skipped) {
          return {};
        }
      } else {
        cursor.take();
      }
    }
    return {};
  }

  ProgramParser::step ProgramParser::declare_record(TokenCursor& cursor) {
    cursor.take();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    const auto qualified = qualified_name(context_.current_namespace(), *name);
    RecordInfo* record = context_.find_record(qualified);
    if (!record) {
      return std::unexpected(ParseError {"internal error: missing structure declaration", cursor.location()});
    }
    if (context_.find_global_in_current(*name) || context_.find_functions_in_current(*name) ||
        context_.find_template_in_current(*name) || context_.find_namespace_in_current(*name)) {
      return std::unexpected(ParseError {"'" + *name + "' is already declared", record->location});
    }
    if (!record->type_params.empty()) {
      while (!cursor.at(TokenType::LBrace) && !cursor.at_end()) {
        cursor.take();
      }
      record->members_complete = true;
      return skip_body(cursor);
    }
    return record_members(*record, cursor);
  }

  ProgramParser::step ProgramParser::record_members(RecordInfo& record, TokenCursor& cursor) {
    if (auto open = expect(cursor, TokenType::LBrace); !open) {
      return std::unexpected(open.error());
    }
    RecordInfo* previous_record = context_.active_record;
    context_.active_record = &record;
    const auto finish = [&](step result) {
      context_.active_record = previous_record;
      return result;
    };
    std::unordered_map<std::string, std::vector<std::string>> called_methods;
    while (!cursor.at(TokenType::RBrace)) {
      if (cursor.at_end()) {
        return finish(std::unexpected(ParseError {"unterminated structure body", record.location}));
      }
      bool is_public = false;
      if (cursor.current().text == "public" || cursor.current().text == "private") {
        is_public = cursor.take().text == "public";
      }
      if (cursor.current().text == "destructor") {
        const auto location = cursor.take().location;
        if (record.destructor) {
          return finish(std::unexpected(ParseError {"a structure has only one destructor", location}));
        }
        MethodInfo method;
        method.function = FunctionInfo {
            {SemType::reference(SemType::record_type(&record), true)}, SemType::Void, context_.function_count++};
        method.param_names = {"this"};
        method.body_index = cursor.index();
        method.location = location;
        record.destructor = method;
        record.auto_clone = false;
        if (auto skipped = skip_body(cursor); !skipped) {
          return finish(skipped);
        }
        continue;
      }
      const bool factory = cursor.current().text == "new";
      if (factory || cursor.at_keyword(Keyword::Fn)) {
        if (!factory) {
          cursor.take();
        }
        auto parsed = signature(cursor);
        if (!parsed) {
          return finish(std::unexpected(parsed.error()));
        }
        const auto& found = *parsed;
        if (!found.type_params.empty()) {
          return finish(std::unexpected(ParseError {"method type parameters are not supported", found.location}));
        }
        if (std::ranges::any_of(record.fields, [&](const FieldInfo& f) {
              return f.name == found.name;
            })) {
          return finish(std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location}));
        }
        MethodInfo method;
        method.param_names = found.param_names;
        method.body_index = cursor.index();
        method.factory = factory;
        method.is_public = is_public;
        method.location = found.location;
        for (const auto& param : found.params) {
          auto resolved = context_.resolve_type(param, {}, found.location);
          if (!resolved) {
            return finish(std::unexpected(resolved.error()));
          }
          method.function.params.push_back(*resolved);
        }
        auto result_type = context_.resolve_type(found.return_type, {}, found.location);
        if (!result_type) {
          return finish(std::unexpected(result_type.error()));
        }
        method.function.return_type = *result_type;
        if (factory && *result_type != SemType::record_type(&record)) {
          return finish(std::unexpected(ParseError {"a factory must return its structure type", found.location}));
        }
        for (std::size_t i = 0; i < method.param_names.size(); ++i) {
          if (method.param_names[i] == "this" &&
              (i != 0 || factory || !method.function.params[i].is_reference() ||
               *method.function.params[i].pointee != SemType::record_type(&record))) {
            return finish(std::unexpected(
                ParseError {"this must be the first reference parameter of an instance method", found.location}));
          }
        }
        auto& overloads = record.methods[found.name];
        if (std::ranges::any_of(overloads, [&](const MethodInfo& m) {
              return m.function.params == method.function.params;
            })) {
          return finish(std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location}));
        }
        if (found.name == "clone") {
          if (!overloads.empty() || factory ||
              method.function.params != std::vector<SemType> {SemType::reference(SemType::record_type(&record))} ||
              method.function.return_type != SemType::record_type(&record)) {
            return finish(std::unexpected(
                ParseError {"clone must take this: ref structure and return the structure", found.location}));
          }
          // The body of a generic clone is checked only when copying is requested.
          record.auto_clone = false;
        } else {
          method.function.id = context_.function_count++;
        }
        // Storage-invalidating methods carry an effect checked at reference uses.
        auto scan = cursor;
        int depth = 0;
        do {
          if (scan.at(TokenType::LBrace)) {
            ++depth;
          }
          if (scan.at(TokenType::RBrace)) {
            --depth;
          }
          if (scan.at_keyword(Keyword::Move) && scan.peek().type == TokenType::Operator && scan.peek().text == "*") {
            method.invalidates_references = true;
          }
          if (scan.at_keyword(Keyword::Ref) && scan.peek().text == "mut" && scan.peek(2).text == "this") {
            method.invalidates_references = true;
          }
          if (scan.current().text == "this" && scan.peek().type != TokenType::Dot && !method.function.params.empty() &&
              method.function.params[0].is_reference() && method.function.params[0].mutable_reference) {
            method.invalidates_references = true;
          }
          if (scan.current().text == "this" && scan.peek().type == TokenType::Dot &&
              scan.peek(2).type == TokenType::Identifier && scan.peek(3).type == TokenType::LParen) {
            called_methods[found.name].push_back(scan.peek(2).text);
          }
          if (scan.current().text == "this" && scan.peek().type == TokenType::Dot &&
              scan.peek(2).type == TokenType::Identifier && scan.peek(3).type == TokenType::Dot &&
              !method.function.params.empty() && method.function.params[0].mutable_reference) {
            method.invalidates_references = true;
          }
          if (scan.at_keyword(Keyword::Primitive) && scan.peek().text == "free") {
            method.invalidates_references = true;
          }
          scan.take();
        } while (!scan.at_end() && depth != 0);
        overloads.push_back(std::move(method));
        if (auto skipped = skip_body(cursor); !skipped) {
          return finish(skipped);
        }
      } else {
        auto name = expect_identifier(cursor);
        if (!name) {
          return finish(std::unexpected(name.error()));
        }
        if (record.methods.contains(*name) || std::ranges::any_of(record.fields, [&](const FieldInfo& f) {
              return f.name == *name;
            })) {
          return finish(std::unexpected(ParseError {"'" + *name + "' is already declared", cursor.location()}));
        }
        if (auto colon = expect(cursor, TokenType::Colon); !colon) {
          return finish(std::unexpected(colon.error()));
        }
        auto type = context_.read_type(cursor);
        if (!type) {
          return finish(std::unexpected(type.error()));
        }
        if (*type == SemType::Void) {
          return finish(std::unexpected(ParseError {"a field cannot have type void", cursor.location()}));
        }
        record.fields.push_back(FieldInfo {*name, *type, is_public});
        if (auto semi = expect_semicolon(cursor); !semi) {
          return finish(std::unexpected(semi.error()));
        }
      }
    }
    cursor.take();
    record.drop_id = context_.function_count++;
    record.members_complete = true;
    record.complete = true;
    // Replacing an owned record field ends the old value's lifetime even when
    // its destructor is generated and no raw free appears in this method.
    for (auto& [name, methods] : record.methods) {
      for (auto& method : methods) {
        auto scan = cursor;
        scan.seek(method.body_index);
        int depth = 0;
        do {
          if (scan.at(TokenType::LBrace)) {
            ++depth;
          }
          if (scan.at(TokenType::RBrace)) {
            --depth;
          }
          if (scan.current().text == "this" && scan.peek().type == TokenType::Dot) {
            auto path = scan;
            path.take();
            SemType current = SemType::record_type(&record);
            while (path.match(TokenType::Dot) && path.at(TokenType::Identifier)) {
              while (current.is_reference()) {
                current = *current.pointee;
              }
              if (!current.is_record()) {
                break;
              }
              const auto field_name = path.take().text;
              const auto found = std::ranges::find(current.record->fields, field_name, &FieldInfo::name);
              if (found == current.record->fields.end()) {
                break;
              }
              current = found->type;
              if (path.at_op("=") && current.is_record()) {
                method.invalidates_references = true;
              }
            }
          }
          scan.take();
        } while (!scan.at_end() && depth != 0);
      }
    }
    // A method inherits invalidation only from the methods it actually calls.
    bool changed;
    do {
      changed = false;
      for (auto& [name, methods] : record.methods) {
        for (auto& method : methods) {
          if (method.invalidates_references) {
            continue;
          }
          for (const auto& called : called_methods[name]) {
            auto found = record.methods.find(called);
            if (found != record.methods.end() && std::ranges::any_of(found->second, [](const MethodInfo& callee) {
                  return callee.invalidates_references;
                })) {
              method.invalidates_references = true;
              changed = true;
              break;
            }
          }
        }
      }
    } while (changed);
    return finish({});
  }

  std::expected<RecordInfo*, ParseError> ProgramParser::instantiate_record(RecordInfo* source,
                                                                           std::vector<SemType> args,
                                                                           SourceLocation location) {
    if (source->type_params.size() != args.size()) {
      return std::unexpected(ParseError {"wrong number of structure type arguments", location});
    }
    std::string key = source->name + "<";
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i) {
        key += ",";
      }
      key += to_string(args[i]);
      for (const auto bound : source->type_params[i].bounds) {
        if (!satisfies(args[i], bound)) {
          return std::unexpected(
              ParseError {to_string(args[i]) + " does not satisfy " + std::string(to_string(bound)), location});
        }
      }
    }
    key += ">";
    RecordInfo* record = nullptr;
    if (const auto found = record_instances_.find(key); found != record_instances_.end()) {
      record = found->second;
      if (record->members_complete || materializing_records_.contains(record)) {
        return record;
      }
    } else {
      if (record_instances_.size() >= ParseContext::max_instantiations) {
        return std::unexpected(ParseError {"too many structure instantiations", location});
      }
      auto declared = context_.declare_record(key, location);
      if (!declared) {
        return std::unexpected(declared.error());
      }
      record = *declared;
      record->ns_path = source->ns_path;
      record->template_origin = source;
      record->type_args = args;
      record->body_index = source->body_index;
      record_instances_.emplace(key, record);
    }
    if (!source->members_complete && !declarations_complete_) {
      return record;
    }
    materializing_records_.insert(record);
    const auto saved_substitution = context_.type_substitution();
    std::unordered_map<std::string, SemType> substitution;
    for (std::size_t i = 0; i < args.size(); ++i) {
      substitution.emplace(source->type_params[i].name, args[i]);
    }
    context_.set_type_substitution(std::move(substitution));
    const auto saved_namespace = context_.set_namespace_path(record->ns_path);
    TokenCursor cursor(context_.cursor.tokens());
    cursor.seek(record->body_index);
    auto members = record_members(*record, cursor);
    context_.set_namespace_path(saved_namespace);
    context_.set_type_substitution(saved_substitution);
    materializing_records_.erase(record);
    if (!members) {
      return std::unexpected(members.error());
    }
    if (declarations_complete_) {
      if (auto valid = validate_record_ownership(); !valid) {
        return std::unexpected(valid.error());
      }
    }
    return record;
  }

  ProgramParser::step ProgramParser::lower_method(RecordInfo& record, MethodInfo& method, bool destructor) {
    const auto saved_cursor = context_.cursor;
    const auto saved_namespace = context_.set_namespace_path(record.ns_path);
    const auto saved_substitution = context_.type_substitution();
    std::unordered_map<std::string, SemType> substitution;
    if (record.template_origin) {
      for (std::size_t i = 0; i < record.type_args.size(); ++i) {
        substitution.emplace(record.template_origin->type_params[i].name, record.type_args[i]);
      }
    }
    context_.set_type_substitution(std::move(substitution));
    context_.active_record = &record;
    context_.in_factory = method.factory;
    context_.cursor.seek(method.body_index);
    current_function_ = record.name + (destructor ? ".destructor" : ".method");
    current_return_type_ = method.function.return_type;
    std::vector<Type> params;
    for (const auto& type : method.function.params) {
      params.push_back(lower(type));
    }
    context_.reset_frame(params);
    context_.push_scope();
    for (std::size_t i = 0; i < params.size(); ++i) {
      LocalInfo local {params[i], method.function.params[i], i, true, method.location};
      local.parameter = true;
      local.mutable_object = method.function.params[i].is_reference() && method.function.params[i].mutable_reference;
      if (auto declared = context_.declare_local(method.param_names[i], local); !declared) {
        return std::unexpected(declared.error());
      }
    }
    auto body = block();
    if (!body) {
      return std::unexpected(body.error());
    }
    if (body->falls_through) {
      if (current_return_type_ != SemType::Void) {
        return std::unexpected(ParseError {"control reaches the end of a non-void method", method.location});
      }
      if (!context_.builder.terminated()) {
        context_.emit_cleanup(0);
        context_.builder.emit(Op::Ret, {});
      }
    }
    context_.pop_scope();
    if (auto checked = context_.check_local_flow(); !checked) {
      return std::unexpected(checked.error());
    }
    context_.builder.prune();
    middleend::Function function {params, lower(method.function.return_type), context_.builder.frame_size(),
                                  context_.builder.take()};
    if (program_.functions.size() <= method.function.id) {
      program_.functions.resize(method.function.id + 1);
    }
    program_.functions[method.function.id] = std::move(function);
    generated_members_.insert(method.function.id);
    context_.cursor = saved_cursor;
    context_.set_namespace_path(saved_namespace);
    context_.set_type_substitution(saved_substitution);
    context_.active_record = nullptr;
    context_.in_factory = false;
    return {};
  }

  ProgramParser::step ProgramParser::generate_drop(RecordInfo& record) {
    const auto record_type = lower(SemType::record_type(&record));
    CfgBuilder builder(std::vector<Type> {record_type});
    const auto object = Operand::vreg(record_type, 0);
    if (record.destructor) {
      const Type reference = Type::pointer(record_type);
      const auto address = Operand::vreg(reference, builder.alloc(reference));
      builder.emit(Op::Addr, {address, object});
      builder.emit(Op::Call,
                   {Operand::empty(), Operand::function(Type::Void, record.destructor->function.id), address});
    }
    for (std::size_t index = record.fields.size(); index > 0; --index) {
      const auto& field = record.fields[index - 1];
      if (!field.type.is_record()) {
        continue;
      }
      if (!field.type.record->drop_id) {
        return std::unexpected(ParseError {"incomplete owned field type", record.location});
      }
      const auto raw_address = Operand::vreg(record_type, builder.alloc(record_type));
      builder.emit(Op::Offset, {raw_address, object, Operand::instant(Type::U64, index - 1)});
      const Type field_pointer_type = Type::pointer(lower(field.type));
      const auto address = Operand::vreg(field_pointer_type, builder.alloc(field_pointer_type));
      builder.emit(Op::Cast, {address, raw_address});
      const auto value = Operand::vreg(lower(field.type), builder.alloc(lower(field.type)));
      builder.emit(Op::Load, {value, address});
      builder.emit(Op::Call, {Operand::empty(), Operand::function(Type::Void, *field.type.record->drop_id), value});
    }
    const Type bytes = Type::pointer(Type::U8);
    const auto pointer = Operand::vreg(bytes, builder.alloc(bytes));
    builder.emit(Op::Cast, {pointer, object});
    builder.emit(Op::Primitive, {Operand::empty(), pointer}, static_cast<std::uint32_t>(primitives::Prim::Free));
    builder.emit(Op::Ret, {});
    const auto id = *record.drop_id;
    if (program_.functions.size() <= id) {
      program_.functions.resize(id + 1);
    }
    program_.functions[id] = middleend::Function {{record_type}, Type::Void, builder.frame_size(), builder.take()};
    generated_members_.insert(id);
    return {};
  }

  ProgramParser::step ProgramParser::generate_clone(RecordInfo& record) {
    const auto record_type = lower(SemType::record_type(&record));
    const Type reference = Type::pointer(record_type);
    CfgBuilder builder(std::vector<Type> {reference});
    const auto object = Operand::vreg(record_type, builder.alloc(record_type));
    builder.emit(Op::Load, {object, Operand::vreg(reference, 0)});
    const Type byte_pointer = Type::pointer(Type::U8);
    const auto allocated = Operand::vreg(byte_pointer, builder.alloc(byte_pointer));
    builder.emit(Op::Primitive,
                 {allocated, Operand::instant(Type::U64, std::max<std::size_t>(1, record.fields.size()) * 8)},
                 static_cast<std::uint32_t>(primitives::Prim::Alloc));
    const auto result = Operand::vreg(record_type, builder.alloc(record_type));
    builder.emit(Op::Cast, {result, allocated});
    for (std::size_t index = 0; index < record.fields.size(); ++index) {
      const auto& field = record.fields[index];
      const Type type = lower(field.type);
      const Type pointer_type = Type::pointer(type);
      const auto source_raw = Operand::vreg(record_type, builder.alloc(record_type));
      const auto target_raw = Operand::vreg(record_type, builder.alloc(record_type));
      builder.emit(Op::Offset, {source_raw, object, Operand::instant(Type::U64, index)});
      builder.emit(Op::Offset, {target_raw, result, Operand::instant(Type::U64, index)});
      const auto source = Operand::vreg(pointer_type, builder.alloc(pointer_type));
      const auto target = Operand::vreg(pointer_type, builder.alloc(pointer_type));
      builder.emit(Op::Cast, {source, source_raw});
      builder.emit(Op::Cast, {target, target_raw});
      const auto copied = Operand::vreg(type, builder.alloc(type));
      if (field.type.is_record()) {
        auto clone = context_.ensure_clone(field.type.record, record.location);
        if (!clone) {
          return std::unexpected(clone.error());
        }
        builder.emit(Op::Call, {copied, Operand::function(type, *clone), source});
      } else {
        builder.emit(Op::Load, {copied, source});
      }
      builder.emit(Op::Store, {target, copied});
    }
    builder.emit(Op::Ret, {result});
    const auto id = *record.clone_id;
    if (program_.functions.size() <= id) {
      program_.functions.resize(id + 1);
    }
    program_.functions[id] = middleend::Function {{reference}, record_type, builder.frame_size(), builder.take()};
    generated_members_.insert(id);
    return {};
  }

  ProgramParser::step ProgramParser::validate_record_ownership() {
    std::unordered_set<RecordInfo*> visiting;
    std::unordered_set<RecordInfo*> visited;
    const auto visit = [&](const auto& self, RecordInfo* record) -> step {
      if (!record->members_complete || !record->type_params.empty() || visited.contains(record)) {
        return {};
      }
      if (!visiting.insert(record).second) {
        return std::unexpected(
            ParseError {"owned structure fields form a cycle through '" + record->name + "'", record->location});
      }
      for (const auto& field : record->fields) {
        if (field.type.is_record()) {
          if (auto child = self(self, field.type.record); !child) {
            return child;
          }
        }
      }
      visiting.erase(record);
      visited.insert(record);
      return {};
    };
    for (const auto& owner : context_.records()) {
      auto* record = owner.get();
      if (auto valid = visit(visit, record); !valid) {
        return valid;
      }
      if (record->template_origin) {
        for (std::size_t i = 0; i < record->type_args.size(); ++i) {
          for (const auto capability : record->template_origin->type_params[i].bounds) {
            if (!satisfies(record->type_args[i], capability)) {
              return std::unexpected(ParseError {
                  to_string(record->type_args[i]) + " does not satisfy " + std::string(to_string(capability)),
                  record->location});
            }
            if (capability == Capability::Clonable && record->type_args[i].is_record() &&
                record->type_args[i].record->complete) {
              auto clone = context_.ensure_clone(record->type_args[i].record, record->location);
              if (!clone) {
                return std::unexpected(clone.error());
              }
            }
          }
        }
      }
    }
    return {};
  }

  ProgramParser::step ProgramParser::lower_record_methods() {
    bool progress;
    do {
      progress = false;
      for (std::size_t index = 0; index < context_.records().size(); ++index) {
        auto& record = *context_.records()[index];
        if (!record.type_params.empty() || !record.members_complete) {
          continue;
        }
        std::vector<MethodInfo*> methods;
        for (auto& [name, overloads] : record.methods) {
          for (auto& method : overloads) {
            if (name == "clone" && !record.clone_id) {
              continue;
            }
            methods.push_back(&method);
          }
        }
        if (record.destructor) {
          methods.push_back(&*record.destructor);
        }
        std::ranges::sort(methods, {}, [](const MethodInfo* m) {
          return m->function.id;
        });
        for (auto* method : methods) {
          if (generated_members_.contains(method->function.id)) {
            continue;
          }
          if (auto lowered = lower_method(record, *method, record.destructor && method == &*record.destructor);
              !lowered) {
            return lowered;
          }
          progress = true;
        }
        if (record.drop_id && !generated_members_.contains(*record.drop_id)) {
          if (auto generated = generate_drop(record); !generated) {
            return generated;
          }
          progress = true;
        }
        if (record.clone_id && !record.methods.contains("clone") && !generated_members_.contains(*record.clone_id)) {
          if (auto generated = generate_clone(record); !generated) {
            return generated;
          }
          progress = true;
        }
      }
    } while (progress);
    return {};
  }

  void ProgramParser::generate_entrypoint_cleanup() {
    if (owned_globals_.empty()) {
      return;
    }
    CfgBuilder builder;
    const auto result = Operand::vreg(Type::I32, builder.alloc(Type::I32));
    builder.emit(Op::Call, {result, Operand::function(Type::I32, entrypoint_)});
    for (auto global = owned_globals_.rbegin(); global != owned_globals_.rend(); ++global) {
      const auto [id, record] = *global;
      const auto storage = Operand::global(lower(SemType::record_type(record)), id);
      builder.emit(Op::Call, {Operand::empty(), Operand::function(Type::Void, *record->drop_id), storage});
      builder.emit(Op::Mov, {storage, Operand::instant(storage.type, 0)});
    }
    builder.emit(Op::Ret, {result});
    entrypoint_ = context_.function_count++;
    if (program_.functions.size() <= entrypoint_) {
      program_.functions.resize(entrypoint_ + 1);
    }
    program_.functions[entrypoint_] = middleend::Function {{}, Type::I32, builder.frame_size(), builder.take()};
  }

}  // namespace erax::frontend
