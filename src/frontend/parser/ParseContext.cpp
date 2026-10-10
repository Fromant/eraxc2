#include "ParseContext.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace erax::frontend {

  namespace {

    struct KeywordName {
      std::string_view text;
      Keyword keyword;
    };

    // The complete list from `docs/core/language.md#reserved-words`.
    constexpr KeywordName keywords[] = {
        {"fn", Keyword::Fn},
        {"let", Keyword::Let},
        {"namespace", Keyword::Namespace},
        {"if", Keyword::If},
        {"else", Keyword::Else},
        {"while", Keyword::While},
        {"do", Keyword::Do},
        {"for", Keyword::For},
        {"break", Keyword::Break},
        {"continue", Keyword::Continue},
        {"return", Keyword::Return},
        {"true", Keyword::True},
        {"false", Keyword::False},
        {"i8", Keyword::I8},
        {"u8", Keyword::U8},
        {"i16", Keyword::I16},
        {"u16", Keyword::U16},
        {"i32", Keyword::I32},
        {"u32", Keyword::U32},
        {"i64", Keyword::I64},
        {"u64", Keyword::U64},
        {"bool", Keyword::Bool},
        {"char", Keyword::Char},
        {"primitive", Keyword::Primitive},
        {"as", Keyword::As},
        {"extends", Keyword::Extends},
    };

    std::string expected_text(TokenType type) {
      switch (type) {
        case TokenType::Identifier: return "an identifier";
        case TokenType::Number: return "a number";
        case TokenType::String: return "a string";
        case TokenType::Char: return "a character";
        case TokenType::Operator: return "an operator";
        case TokenType::Semicolon: return "';'";
        case TokenType::Colon: return "':'";
        case TokenType::Comma: return "','";
        case TokenType::Dot: return "'.'";
        case TokenType::LParen: return "'('";
        case TokenType::RParen: return "')'";
        case TokenType::LBracket: return "'['";
        case TokenType::RBracket: return "']'";
        case TokenType::LBrace: return "'{'";
        case TokenType::RBrace: return "'}'";
        case TokenType::EndOfFile: return "end of file";
      }
      return "a token";
    }

  }  // namespace

  Keyword keyword_of(std::string_view text) noexcept {
    for (const KeywordName& entry : keywords) {
      if (entry.text == text) {
        return entry.keyword;
      }
    }
    return Keyword::None;
  }

  std::string_view to_string(Keyword keyword) noexcept {
    for (const KeywordName& entry : keywords) {
      if (entry.keyword == keyword) {
        return entry.text;
      }
    }
    return "<none>";
  }

  std::optional<SemType> type_of(Keyword keyword) noexcept {
    switch (keyword) {
      case Keyword::Bool: return SemType::Bool;
      case Keyword::Char: return SemType::Char;
      case Keyword::I8: return SemType::I8;
      case Keyword::U8: return SemType::U8;
      case Keyword::I16: return SemType::I16;
      case Keyword::U16: return SemType::U16;
      case Keyword::I32: return SemType::I32;
      case Keyword::U32: return SemType::U32;
      case Keyword::I64: return SemType::I64;
      case Keyword::U64: return SemType::U64;
      default: return std::nullopt;
    }
  }

  std::string_view to_string(Capability capability) noexcept {
    switch (capability) {
      case Capability::Ordered: return "Ordered";
      case Capability::Negatable: return "Negatable";
      case Capability::Arithmetic: return "Arithmetic";
    }
    return "<capability>";
  }

  std::optional<Capability> capability_of(std::string_view text) noexcept {
    if (text == "Ordered") {
      return Capability::Ordered;
    }
    if (text == "Negatable") {
      return Capability::Negatable;
    }
    if (text == "Arithmetic") {
      return Capability::Arithmetic;
    }
    return std::nullopt;
  }

  bool satisfies(SemType type, Capability capability) noexcept {
    switch (capability) {
      // `Ordered` (`< <= > >=`) and `Arithmetic` (`+ - * / %`) accept the integers
      // and `char`, which is exactly the set `is_integer` names; `bool` is neither.
      case Capability::Ordered:
      case Capability::Arithmetic: return is_integer(type);
      // Unary `-` needs a negative side, which only a signed integer has.
      case Capability::Negatable: return is_integer(type) && is_signed(type);
    }
    return false;
  }

  const Token& TokenCursor::peek(std::size_t ahead) const noexcept {
    const std::size_t last = tokens_->size() - 1;
    const std::size_t index = index_ + ahead;
    return (*tokens_)[index < last ? index : last];
  }

  const Token& TokenCursor::take() noexcept {
    // The token that was consumed, not the one after it: reading a name or a
    // closing bracket is what the caller asked for.
    const Token& consumed = (*tokens_)[index_];
    const std::size_t last = tokens_->size() - 1;
    if (index_ < last) {
      ++index_;
    }
    return consumed;
  }

  bool TokenCursor::match(TokenType type) {
    if (!at(type)) {
      return false;
    }
    take();
    return true;
  }

  bool TokenCursor::match_op(std::string_view text) {
    if (!at_op(text)) {
      return false;
    }
    take();
    return true;
  }

  bool TokenCursor::else_follows() const {
    std::size_t index = index_ + 1;
    if (index >= tokens_->size() || (*tokens_)[index].type != TokenType::LBrace) {
      return false;
    }
    int depth = 0;
    while (index < tokens_->size()) {
      const TokenType type = (*tokens_)[index].type;
      if (type == TokenType::LBrace) {
        ++depth;
      } else if (type == TokenType::RBrace && --depth == 0) {
        ++index;
        break;
      }
      ++index;
    }
    if (index >= tokens_->size()) {
      return false;
    }
    const Token& token = (*tokens_)[index];
    return token.type == TokenType::Identifier && keyword_of(token.text) == Keyword::Else;
  }

  /// The index just past the `( … )` group that starts at \p index, or the end of the
  /// stream if it is never closed.
  std::size_t TokenCursor::past_group(std::size_t index) const noexcept {
    int depth = 0;
    for (; index < tokens_->size(); ++index) {
      if ((*tokens_)[index].type == TokenType::LParen) {
        ++depth;
      } else if ((*tokens_)[index].type == TokenType::RParen && --depth == 0) {
        return index + 1;
      }
    }
    return tokens_->size();
  }

  std::size_t TokenCursor::past_operand(std::size_t index) const noexcept {
    // A prefix operator belongs to the operand in front of it, so `-128` is one operand
    // and not a subtraction whose left side is missing. An infix `-` always follows a
    // whole operand, so one at the cursor can only be a prefix.
    while (index < tokens_->size() && (*tokens_)[index].type == TokenType::Operator) {
      const std::string_view text = (*tokens_)[index].text;
      // `*` and `&` are prefix here (`*p`, `&x`): an infix one always follows a
      // whole operand, so one where an operand starts can only be a prefix.
      if (text != "-" && text != "!" && text != "~" && text != "*" && text != "&") {
        break;
      }
      ++index;
    }
    // What follows the operand is the one token after it, so the operand's own extent is
    // stepped over exactly: a `( … )` group, a literal or a name, and a call's arguments
    // after that name. Anything else, including a `;`, is where the operand ends.
    if (index < tokens_->size() && (*tokens_)[index].type == TokenType::LParen) {
      index = past_group(index);
    } else if (index < tokens_->size() && (*tokens_)[index].type == TokenType::Identifier) {
      // A name may be namespace-qualified: each `.` identifier is part of the
      // same operand (`std.print`), and a call's `( … )` follows the last one.
      while (index + 2 < tokens_->size() && (*tokens_)[index].type == TokenType::Identifier &&
             (*tokens_)[index + 1].type == TokenType::Dot && (*tokens_)[index + 2].type == TokenType::Identifier) {
        index += 2;
      }
      ++index;
      if (index < tokens_->size() && (*tokens_)[index].type == TokenType::LParen) {
        index = past_group(index);
      }
    } else if (index < tokens_->size()) {
      ++index;
    }
    return index;
  }

  bool TokenCursor::operator_follows(std::span<const std::string_view> operators) const {
    const std::size_t index = past_operand(index_);
    if (index >= tokens_->size() || (*tokens_)[index].type != TokenType::Operator) {
      return false;
    }
    return std::ranges::find(operators, (*tokens_)[index].text) != operators.end();
  }

  bool TokenCursor::keyword_follows(Keyword keyword) const {
    const std::size_t index = past_operand(index_);
    return index < tokens_->size() && (*tokens_)[index].type == TokenType::Identifier &&
        keyword_of((*tokens_)[index].text) == keyword;
  }

  void TokenCursor::skip_to_sync() {
    int depth = 0;
    while (!at_end()) {
      if (at(TokenType::LBrace) || at(TokenType::LParen)) {
        ++depth;
      } else if (at(TokenType::RParen)) {
        if (depth == 0) {
          return;
        }
        --depth;
      } else if (at(TokenType::RBrace)) {
        if (depth == 0) {
          take();
          return;
        }
        --depth;
      } else if (at(TokenType::Semicolon) && depth == 0) {
        take();
        return;
      }
      take();
    }
  }

  std::expected<std::string, ParseError> expect_identifier(TokenCursor& cursor) {
    // A name is wanted here, whatever is in front of us: naming the token that is there
    // would say "expected a number" where a `1` is written instead of what a name is.
    if (!cursor.at(TokenType::Identifier)) {
      return std::unexpected(ParseError {"expected " + expected_text(TokenType::Identifier), cursor.location()});
    }
    const std::string& text = cursor.current().text;
    if (keyword_of(text) != Keyword::None) {
      return std::unexpected(ParseError {"'" + text + "' is a reserved word", cursor.location()});
    }
    return cursor.take().text;
  }

  std::expected<const Token*, ParseError> expect(TokenCursor& cursor, TokenType type) {
    if (!cursor.at(type)) {
      return std::unexpected(ParseError {"expected " + expected_text(type), cursor.location()});
    }
    return &cursor.take();
  }

  std::expected<void, ParseError> expect_op(TokenCursor& cursor, std::string_view text) {
    if (!cursor.at_op(text)) {
      return std::unexpected(ParseError {"expected '" + std::string {text} + "'", cursor.location()});
    }
    cursor.take();
    return {};
  }

  std::expected<void, ParseError> expect_keyword(TokenCursor& cursor, Keyword keyword) {
    if (!cursor.at_keyword(keyword)) {
      return std::unexpected(ParseError {"expected '" + std::string {to_string(keyword)} + "'", cursor.location()});
    }
    cursor.take();
    return {};
  }

  std::expected<void, ParseError> expect_semicolon(TokenCursor& cursor) {
    if (auto semicolon = expect(cursor, TokenType::Semicolon); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    return {};
  }

  std::expected<SemType, ParseError> expect_type(TokenCursor& cursor, std::string_view context) {
    const std::string wanted = context.empty() ? "expected a type" : "expected a type " + std::string {context};
    if (cursor.at(TokenType::Identifier)) {
      // `ptr T` is a type, spelled as a prefix so a nested `ptr ptr i32` needs no
      // brackets and `ptr i32` never meets the `>>` operator problem. `ptr` is only
      // special in type position; a variable may still be named `ptr`.
      if (cursor.current().text == "ptr") {
        cursor.take();
        auto pointee = expect_type(cursor, context);
        if (!pointee) {
          return pointee;
        }
        return SemType::pointer(*pointee);
      }
      if (const std::optional<SemType> type = type_of(keyword_of(cursor.current().text)); type) {
        cursor.take();
        return *type;
      }
      if (cursor.current().text == "string") {
        return std::unexpected(ParseError {"string is not supported yet", cursor.location()});
      }
    }
    return std::unexpected(ParseError {wanted, cursor.location()});
  }

  const LocalInfo* ParseContext::find_local(const std::string& name) const noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto found = scope->locals.find(name);
      if (found != scope->locals.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  LocalInfo* ParseContext::find_local(const std::string& name) noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto found = scope->locals.find(name);
      if (found != scope->locals.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  const GlobalInfo* ParseContext::find_global(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      const auto found = (*ns)->globals.find(name);
      if (found != (*ns)->globals.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  GlobalInfo& ParseContext::global_of(const std::string& name) noexcept {
    return current_namespace().globals.at(name);
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      const auto found = (*ns)->functions.find(name);
      if (found != (*ns)->functions.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  const TemplateInfo* ParseContext::find_template(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      const auto found = (*ns)->templates.find(name);
      if (found != (*ns)->templates.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  const FunctionInfo* ParseContext::find_function(const std::string& name,
                                                  const std::vector<SemType>& params) const noexcept {
    const std::vector<FunctionInfo>* candidates = find_functions(name);
    if (candidates == nullptr) {
      return nullptr;
    }
    for (const FunctionInfo& candidate : *candidates) {
      if (candidate.params == params) {
        return &candidate;
      }
    }
    return nullptr;
  }

  const GlobalInfo* ParseContext::find_global_in(const NameSpace& ns, const std::string& name) const noexcept {
    const auto found = ns.globals.find(name);
    return found != ns.globals.end() ? &found->second : nullptr;
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions_in(const NameSpace& ns,
                                                                   const std::string& name) const noexcept {
    const auto found = ns.functions.find(name);
    return found != ns.functions.end() ? &found->second : nullptr;
  }

  const TemplateInfo* ParseContext::find_template_in(const NameSpace& ns, const std::string& name) const noexcept {
    const auto found = ns.templates.find(name);
    return found != ns.templates.end() ? &found->second : nullptr;
  }

  const FunctionInfo* ParseContext::find_function_in(const NameSpace& ns, const std::string& name,
                                                     const std::vector<SemType>& params) const noexcept {
    const std::vector<FunctionInfo>* candidates = find_functions_in(ns, name);
    if (candidates == nullptr) {
      return nullptr;
    }
    for (const FunctionInfo& candidate : *candidates) {
      if (candidate.params == params) {
        return &candidate;
      }
    }
    return nullptr;
  }

  const NameSpace* ParseContext::find_namespace(const NameSpace& ns, const std::string& name) const noexcept {
    const auto found = ns.namespaces.find(name);
    return found != ns.namespaces.end() ? found->second.get() : nullptr;
  }

  const NameSpace* ParseContext::find_namespace(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      if (const NameSpace* found = find_namespace(**ns, name); found != nullptr) {
        return found;
      }
    }
    return nullptr;
  }

  bool ParseContext::is_declared_in_current(const std::string& name) const noexcept {
    const NameSpace& ns = current_namespace();
    return ns.globals.contains(name) || ns.functions.contains(name) || ns.templates.contains(name) ||
        ns.namespaces.contains(name);
  }

  NameSpace* ParseContext::find_namespace_in_current(const std::string& name) noexcept {
    const auto found = current_namespace().namespaces.find(name);
    return found != current_namespace().namespaces.end() ? found->second.get() : nullptr;
  }

  const NameSpace* ParseContext::find_namespace_in_current(const std::string& name) const noexcept {
    const auto found = current_namespace().namespaces.find(name);
    return found != current_namespace().namespaces.end() ? found->second.get() : nullptr;
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions_in_current(const std::string& name) const noexcept {
    return find_functions_in(current_namespace(), name);
  }

  const TemplateInfo* ParseContext::find_template_in_current(const std::string& name) const noexcept {
    return find_template_in(current_namespace(), name);
  }

  const GlobalInfo* ParseContext::find_global_in_current(const std::string& name) const noexcept {
    return find_global_in(current_namespace(), name);
  }

  const FunctionInfo* ParseContext::find_function_in_current(const std::string& name,
                                                             const std::vector<SemType>& params) const noexcept {
    return find_function_in(current_namespace(), name, params);
  }

  std::expected<NameSpace*, ParseError> ParseContext::resolve_namespace(const std::vector<std::string>& path,
                                                                        SourceLocation location) const {
    NameSpace* ns = nullptr;
    std::string qualified;
    for (std::size_t index = 0; index < path.size(); ++index) {
      if (!qualified.empty()) {
        qualified += '.';
      }
      qualified += path[index];
      if (index == 0) {
        // The first segment searches outward from the current namespace, so a
        // sibling namespace of the current one is reachable unqualified.
        ns = nullptr;
        for (auto candidate = namespace_stack_.rbegin(); candidate != namespace_stack_.rend(); ++candidate) {
          const auto found = (*candidate)->namespaces.find(path[0]);
          if (found != (*candidate)->namespaces.end()) {
            ns = found->second.get();
            break;
          }
        }
      } else {
        const auto found = ns->namespaces.find(path[index]);
        ns = found != ns->namespaces.end() ? found->second.get() : nullptr;
      }
      if (ns == nullptr) {
        return std::unexpected(ParseError {"unknown namespace '" + qualified + "'", location});
      }
    }
    return ns;
  }

  std::vector<std::string> ParseContext::set_namespace_path(const std::vector<std::string>& path) {
    std::vector<std::string> previous = namespace_stack_.back()->path;
    namespace_stack_.assign(1, &root_namespace_);
    for (const std::string& segment : path) {
      NameSpace* next = find_namespace_in_current(segment);
      if (next == nullptr) {
        break;
      }
      namespace_stack_.push_back(next);
    }
    return previous;
  }

  std::expected<NameSpace*, ParseError> ParseContext::declare_namespace(const std::string& name,
                                                                        SourceLocation location) {
    if (is_declared_in_current(name)) {
      return std::unexpected(ParseError {"'" + name + "' is already declared", location});
    }
    NameSpace& child = *current_namespace().namespaces.emplace(name, std::make_unique<NameSpace>()).first->second;
    child.path = current_namespace().path;
    child.path.push_back(name);
    return &child;
  }

  bool ParseContext::declares(const std::string& name) const noexcept {
    return !scopes_.empty() && scopes_.back().locals.contains(name);
  }

  std::size_t ParseContext::declare_global(const std::string& name, GlobalInfo info) {
    const std::size_t id = global_count++;
    info.id = id;
    current_namespace().globals[name] = info;
    return id;
  }

  std::size_t ParseContext::declare_function(const std::string& name, FunctionInfo info) {
    const std::size_t id = function_count++;
    info.id = id;
    current_namespace().functions[name].push_back(std::move(info));
    return id;
  }

  void ParseContext::declare_template(const std::string& name, TemplateInfo info) {
    info.ns_path = current_namespace().path;
    current_namespace().templates[name] = std::move(info);
  }

  std::expected<std::size_t, ParseError> ParseContext::instantiate(const TemplateInfo* tmpl,
                                                                   const std::vector<SemType>& type_args,
                                                                   SourceLocation location) {
    std::string key;
    for (const std::string& segment : tmpl->ns_path) {
      key += segment;
      key += '.';
    }
    key += tmpl->name;
    for (const SemType& type : type_args) {
      key += ':';
      key += to_string(type);
    }
    const auto found = instantiation_ids_.find(key);
    if (found != instantiation_ids_.end()) {
      return instantiations_[found->second].id;
    }
    if (instantiations_.size() >= max_instantiations) {
      return std::unexpected(
          ParseError {"template instantiation of '" + tmpl->name + "' does not terminate", location});
    }
    // Source functions take ids `0..function_count-1` as the declaration pass gives
    // them out, and every instantiation is appended to the program after all of
    // them. The two are numbered in the same order they are first requested, so the
    // id a call is written with is the index the function ends up at.
    const std::size_t id = function_count + instantiations_.size();
    instantiation_ids_.emplace(std::move(key), instantiations_.size());
    instantiations_.push_back(Instantiation {id, tmpl, type_args, false});
    return id;
  }

  void ParseContext::set_type_substitution(std::unordered_map<std::string, SemType> substitution) {
    type_substitution_ = std::move(substitution);
  }

  std::expected<SemType, ParseError> ParseContext::read_type(TokenCursor& cursor, std::string_view context) const {
    // While an instantiation's body is parsed, a type parameter names the deduced
    // concrete type; everywhere else the substitution is empty and this is the
    // ordinary type read. A pointer recurses, so `ptr T` in a template body reads
    // the deduced type for its pointee.
    if (cursor.at(TokenType::Identifier)) {
      if (cursor.current().text == "ptr") {
        cursor.take();
        auto pointee = read_type(cursor, context);
        if (!pointee) {
          return pointee;
        }
        return SemType::pointer(*pointee);
      }
      const auto found = type_substitution_.find(cursor.current().text);
      if (found != type_substitution_.end()) {
        cursor.take();
        return found->second;
      }
    }
    return expect_type(cursor, context);
  }

  std::expected<void, ParseError> ParseContext::declare_local(const std::string& name, LocalInfo info) {
    Scope& scope = scopes_.back();
    if (scope.locals.contains(name)) {
      return std::unexpected(ParseError {"'" + name + "' is already declared in this scope", info.location});
    }
    scope.locals.emplace(name, info);
    return {};
  }

  void ParseContext::push_scope() {
    scopes_.push_back(Scope {});
    // Every cell from here on belongs to this scope, and goes back to the frame's
    // free list when it closes.
    scopes_.back().vreg_high_water = builder.next_vreg();
  }

  void ParseContext::pop_scope() {
    // Nothing in a closed scope is live afterwards, so its cells are reusable.
    // The frame keeps its high-water mark: its size is a compile-time constant.
    builder.recycle(scopes_.back().vreg_high_water);
    scopes_.pop_back();
  }

  AssignedSnapshot ParseContext::assigned_snapshot() {
    return assigned_snapshot_in(scopes_.size());
  }

  AssignedSnapshot ParseContext::assigned_snapshot_in(std::size_t scopes) {
    AssignedSnapshot snapshot;
    const std::size_t depth = scopes < scopes_.size() ? scopes : scopes_.size();
    for (std::size_t index = 0; index < depth; ++index) {
      for (auto& entry : scopes_[index].locals) {
        snapshot.flags.emplace_back(&entry.second, entry.second.assigned);
      }
    }
    return snapshot;
  }

  void ParseContext::merge_assigned(const AssignedSnapshot& before) {
    for (const auto& entry : before.flags) {
      entry.first->assigned = entry.first->assigned && entry.second;
    }
  }

  void ParseContext::set_assigned(const AssignedSnapshot& snapshot) noexcept {
    for (const auto& entry : snapshot.flags) {
      entry.first->assigned = entry.second;
    }
  }

  void ParseContext::reset_frame(std::vector<middleend::Type> params) {
    scopes_.clear();
    loops_.clear();
    builder = middleend::CfgBuilder {std::move(params)};
  }

}  // namespace erax::frontend
