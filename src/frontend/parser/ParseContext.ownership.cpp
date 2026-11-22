#include "ParseContext.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace erax::frontend {

  LocalInfo* ParseContext::local_by_id(std::size_t id) noexcept {
    for (auto& scope : scopes_) {
      for (auto& [name, local] : scope.locals) {
        if (local.flow_id == id) {
          return &local;
        }
      }
    }
    return nullptr;
  }

  std::expected<Value, ParseError> ParseContext::local_value(const LocalInfo& local, SourceLocation location) {
    if (!local.assigned && !deferred_local_reads) {
      return std::unexpected(
          ParseError {"'" + local_names_[local.flow_id] +
                          (local.moved ? "' is used after it was moved" : "' is read before it is assigned"),
                      location});
    }
    Value value {local.sem_type, middleend::Operand::vreg(local.type, local.vreg), std::nullopt, true};
    value.object_mutable = local.mutable_object;
    value.origins = local.origins;
    value.local_id = local.flow_id;
    value.live_flag = local.live_flag;
    if (auto valid = validate_origins(value, location); !valid) {
      return std::unexpected(valid.error());
    }
    read_local(local, location);
    return value;
  }

  ReferenceOrigin ParseContext::make_origin(std::size_t root, bool external, bool interior) {
    ReferenceOrigin origin {root, interior ? interior_generations_[root] : origin_generations_[root], external,
                            interior, loans_.size()};
    loans_.push_back({root, interior, external});
    loan_events_.push_back(
        {LoanEvent::Kind::Borrow, builder.terminated() ? builder.open_block() : builder.current(), origin.loan, {}});
    return origin;
  }

  void ParseContext::record_invalidation(std::size_t root, bool interior_only, bool external) {
    if (!builder.terminated()) {
      loan_events_.push_back({interior_only ? LoanEvent::Kind::InvalidateInterior : LoanEvent::Kind::Invalidate,
                              builder.current(),
                              root,
                              {},
                              external});
    }
  }

  std::expected<void, ParseError> ParseContext::validate_origins(const Value& value, SourceLocation location) {
    const auto check = [&](const std::vector<ReferenceOrigin>& origins) -> std::expected<void, ParseError> {
      for (const auto& origin : origins) {
        if (!origin.external &&
            (origin.local >= origin_alive_.size() || !origin_alive_[origin.local] ||
             origin.generation !=
                 (origin.interior ? interior_generations_[origin.local] : origin_generations_[origin.local]))) {
          return std::unexpected(ParseError {"reference is used after its target was invalidated", location});
        }
        if (origin.loan < loans_.size()) {
          loan_events_.push_back({LoanEvent::Kind::Read,
                                  builder.terminated() ? builder.open_block() : builder.current(), origin.loan,
                                  location});
        }
      }
      return {};
    };
    if (auto checked = check(value.origins); !checked) {
      return checked;
    }
    return check(value.place_origins);
  }

  std::expected<void, ParseError> ParseContext::check_return(const Value& value, SourceLocation location) {
    if (!type_has_references(value.type)) {
      return {};
    }
    if (auto valid = validate_origins(value, location); !valid) {
      return valid;
    }
    for (const auto& origin : value.origins) {
      if (!origin.external) {
        return std::unexpected(ParseError {"cannot return a value that references a local object", location});
      }
    }
    return {};
  }

  std::expected<void, ParseError> ParseContext::bind_origins(LocalInfo& local, const Value& value,
                                                             SourceLocation location) {
    if (auto valid = validate_origins(value, location); !valid) {
      return valid;
    }
    local.origins = value.origins;
    return {};
  }

  std::expected<void, ParseError> ParseContext::store_origins(const Value& place, const Value& value,
                                                              SourceLocation location) {
    if (place.raw_storage || !type_has_references(value.type)) {
      return {};
    }
    if (auto valid = validate_origins(value, location); !valid) {
      return valid;
    }
    auto roots = place.place_origins;
    if (place.type.is_reference()) {
      roots.insert(roots.end(), place.origins.begin(), place.origins.end());
    }
    if (place.local_id) {
      roots.push_back({*place.local_id, 0, false});
    }
    for (const auto& root : roots) {
      if (root.external) {
        for (const auto& origin : value.origins) {
          if (!origin.external) {
            return std::unexpected(
                ParseError {"cannot store a reference to a local object in an external object", location});
          }
        }
        continue;
      }
      auto* owner = local_by_id(root.local);
      if (!owner || !type_has_references(owner->sem_type)) {
        continue;
      }
      for (const auto& origin : value.origins) {
        if (!origin.external && origin.local == root.local) {
          return std::unexpected(ParseError {"cannot store a reference to an object inside itself", location});
        }
        if (std::ranges::find(owner->origins, origin) == owner->origins.end()) {
          owner->origins.push_back(origin);
        }
      }
    }
    return {};
  }

  std::expected<void, ParseError> ParseContext::propagate_call_origins(const std::vector<Value>& arguments,
                                                                       SourceLocation location) {
    for (std::size_t destination = 0; destination < arguments.size(); ++destination) {
      const auto& place = arguments[destination];
      if (!place.type.is_reference() || !place.type.mutable_reference || !type_has_references(*place.type.pointee)) {
        continue;
      }
      Value contents;
      contents.type = *place.type.pointee;
      for (std::size_t source = 0; source < arguments.size(); ++source) {
        if (source == destination || !type_has_references(arguments[source].type)) {
          continue;
        }
        for (const auto& origin : arguments[source].origins) {
          if (std::ranges::find(contents.origins, origin) == contents.origins.end()) {
            contents.origins.push_back(origin);
          }
        }
      }
      if (auto stored = store_origins(place, contents, location); !stored) {
        return stored;
      }
    }
    return {};
  }

  std::vector<ReferenceOrigin> ParseContext::interior_origins(const Value& value) {
    std::vector<ReferenceOrigin> origins;
    const auto append = [&](const ReferenceOrigin& origin) {
      if (origin.local < origin_alive_.size()) {
        origins.push_back(make_origin(origin.local, origin.external, true));
      }
    };
    for (const auto& origin : value.place_origins) {
      append(origin);
    }
    for (const auto& origin : value.origins) {
      append(origin);
    }
    if (value.local_id && !value.type.is_reference()) {
      origins.push_back(make_origin(*value.local_id, false, true));
    }
    return origins;
  }

  void ParseContext::invalidate_interior(const Value& value) {
    std::vector<std::pair<std::size_t, bool>> roots;
    if (value.local_id && !value.type.is_reference()) {
      roots.emplace_back(*value.local_id, false);
    }
    for (const auto& origin : value.origins) {
      roots.emplace_back(origin.local, origin.external);
    }
    for (const auto& origin : value.place_origins) {
      roots.emplace_back(origin.local, origin.external);
    }
    std::ranges::sort(roots);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    for (const auto& [root, external] : roots) {
      if (root < interior_generations_.size()) {
        record_invalidation(root, true, external);
        if (!external) {
          ++interior_generations_[root];
        }
      }
    }
  }

  void ParseContext::invalidate_value(const Value& value) {
    if (value.local_id && *value.local_id < origin_generations_.size()) {
      record_invalidation(*value.local_id, false);
      ++origin_generations_[*value.local_id];
      ++interior_generations_[*value.local_id];
    }
  }

  void ParseContext::invalidate_referent(const Value& reference) {
    std::vector<ReferenceOrigin> roots = reference.origins;
    roots.insert(roots.end(), reference.place_origins.begin(), reference.place_origins.end());
    for (const auto& origin : roots) {
      if (origin.local >= origin_generations_.size()) {
        continue;
      }
      record_invalidation(origin.local, origin.interior, origin.external);
      if (!origin.external) {
        ++interior_generations_[origin.local];
        if (!origin.interior) {
          ++origin_generations_[origin.local];
        }
      }
    }
  }

  std::expected<Value, ParseError> ParseContext::borrow_value(const Value& value, bool mutable_reference,
                                                              SourceLocation location, bool ephemeral) {
    if (value.global_storage && !ephemeral) {
      return std::unexpected(ParseError {"safe references to global storage are not supported", location});
    }
    if (auto valid = validate_origins(value, location); !valid) {
      return std::unexpected(valid.error());
    }
    if (!value.addressable && !value.place_address) {
      return std::unexpected(ParseError {"ref needs an existing storage place", location});
    }
    if (mutable_reference && !value.object_mutable) {
      return std::unexpected(ParseError {"ref mut needs mutable access", location});
    }
    const auto type = SemType::reference(value.type, mutable_reference);
    const auto lowered = lower(type);
    middleend::Operand address;
    if (value.place_address) {
      address = *value.place_address;
      if (address.type != lowered) {
        address = middleend::Operand::vreg(lowered, builder.alloc(lowered));
        builder.emit(middleend::Op::Cast, {address, *value.place_address});
      }
    } else {
      address = middleend::Operand::vreg(lowered, builder.alloc(lowered));
      builder.emit(middleend::Op::Addr, {address, value.as_operand()});
    }
    Value result {type, address};
    result.object_mutable = mutable_reference;
    result.origins = value.origins;
    result.origins.insert(result.origins.end(), value.place_origins.begin(), value.place_origins.end());
    if (value.local_id) {
      result.origins.push_back(make_origin(*value.local_id, false, false));
    }
    return result;
  }

  std::expected<std::size_t, ParseError> ParseContext::ensure_clone(RecordInfo* record, SourceLocation location) {
    if (!record || !satisfies(SemType::record_type(record), Capability::Clonable)) {
      return std::unexpected(
          ParseError {"type '" + (record ? record->name : std::string("<record>")) + "' is not Clonable", location});
    }
    if (record->clone_id) {
      return *record->clone_id;
    }
    if (!clone_instantiator) {
      return std::unexpected(ParseError {"copy implementation is unavailable for '" + record->name + "'", location});
    }
    return clone_instantiator(record, location);
  }

  std::expected<Value, ParseError> ParseContext::copy_value(const Value& value, SourceLocation location,
                                                            bool materialize) {
    if (auto valid = validate_origins(value, location); !valid) {
      return std::unexpected(valid.error());
    }
    if (!value.type.is_record()) {
      Value result = value;
      if (!type_has_references(result.type) && !result.type.is_pointer()) {
        result.origins.clear();
      }
      result.place_origins.clear();
      result.place_address.reset();
      result.local_id.reset();
      result.global_storage = false;
      result.live_flag.reset();
      result.addressable = false;
      if (materialize && (value.addressable || value.place_address) && !value.constant && value.type != SemType::Void) {
        result.operand = middleend::Operand::vreg(lower(value.type), builder.alloc(lower(value.type)));
        builder.emit(middleend::Op::Mov, {result.operand, value.as_operand()});
      }
      return result;
    }
    if (!value.addressable && !value.place_address) {
      return value;
    }
    auto clone = ensure_clone(value.type.record, location);
    if (!clone) {
      return std::unexpected(clone.error());
    }
    auto receiver = borrow_value(value, false, location, value.global_storage);
    if (!receiver) {
      return receiver;
    }
    const auto lowered = lower(value.type);
    const auto destination = middleend::Operand::vreg(lowered, builder.alloc(lowered));
    builder.emit(middleend::Op::Call,
                 {destination, middleend::Operand::function(lowered, *clone), receiver->as_operand()});
    Value result {value.type, destination};
    if (type_has_references(value.type)) {
      // User cloning can point a copied reference field into the source object.
      // Retain the receiver's storage dependency as well as its contained loans.
      result.origins = receiver->origins;
    }
    track_temporary(result);
    return result;
  }

  void ParseContext::track_temporary(Value& value) {
    if (!value.type.is_record() || value.live_flag) {
      return;
    }
    value.live_flag = middleend::Operand::vreg(middleend::Type::I8, builder.alloc(middleend::Type::I8));
    builder.emit(middleend::Op::Mov, {*value.live_flag, middleend::Operand::instant(middleend::Type::I8, 1)});
    temporaries_.push_back(value);
  }

  void ParseContext::transfer_value(const Value& value) {
    if (value.live_flag) {
      builder.emit(middleend::Op::Mov, {*value.live_flag, middleend::Operand::instant(middleend::Type::I8, 0)});
    }
  }

  void ParseContext::drop_value(const Value& value) {
    if (!value.type.is_record() || !value.type.record || !value.type.record->drop_id) {
      return;
    }
    const auto call = [&] {
      builder.emit(
          middleend::Op::Call,
          {middleend::Operand::empty(),
           middleend::Operand::function(middleend::Type::Void, *value.type.record->drop_id), value.as_operand()});
      if (value.live_flag) {
        builder.emit(middleend::Op::Mov, {*value.live_flag, middleend::Operand::instant(middleend::Type::I8, 0)});
      }
    };
    if (!value.live_flag) {
      call();
      return;
    }
    const auto destroy = builder.new_block();
    const auto next = builder.new_block();
    builder.link_branch(
        destroy, {middleend::Op::CmpNe, *value.live_flag, middleend::Operand::instant(middleend::Type::I8, 0)}, next);
    builder.set_current(destroy);
    call();
    builder.link_next(next);
    builder.set_current(next);
  }

  void ParseContext::cleanup_temporaries() {
    if (builder.terminated()) {
      temporaries_.clear();
      return;
    }
    for (auto item = temporaries_.rbegin(); item != temporaries_.rend(); ++item) {
      drop_value(*item);
    }
    temporaries_.clear();
  }

  bool ParseContext::has_owned_values() const {
    if (!temporaries_.empty()) {
      return true;
    }
    for (const auto& scope : scopes_) {
      for (const auto& [name, local] : scope.locals) {
        if (local.sem_type.is_record()) {
          return true;
        }
      }
    }
    return false;
  }

  void ParseContext::emit_cleanup(std::size_t retained_scopes) {
    if (builder.terminated()) {
      return;
    }
    cleanup_temporaries();
    for (std::size_t index = scopes_.size(); index > retained_scopes; --index) {
      auto& scope = scopes_[index - 1];
      for (auto name = scope.declaration_order.rbegin(); name != scope.declaration_order.rend(); ++name) {
        const auto& local = scope.locals.at(*name);
        Value value {local.sem_type, middleend::Operand::vreg(local.type, local.vreg)};
        value.live_flag = local.live_flag;
        value.origins = local.origins;
        if (value.type.is_record() && type_has_references(value.type)) {
          if (auto valid = validate_origins(value, local.location); !valid && !cleanup_error_) {
            cleanup_error_ = valid.error();
          }
        }
        drop_value(value);
      }
    }
  }

}  // namespace erax::frontend
