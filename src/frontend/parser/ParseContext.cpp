#include "ParseContext.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

#include "primitives/Primitives.hpp"

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
        {"const", Keyword::Const},
        {"mut", Keyword::Mut},
        {"move", Keyword::Move},
        {"ref", Keyword::Ref},
        {"struct", Keyword::Struct},
        {"public", Keyword::Public},
        {"private", Keyword::Private},
        {"destructor", Keyword::Destructor},
        {"namespace", Keyword::Namespace},
        {"import", Keyword::Import},
        {"export", Keyword::Export},
        {"if", Keyword::If},
        {"else", Keyword::Else},
        {"while", Keyword::While},
        {"do", Keyword::Do},
        {"for", Keyword::For},
        {"break", Keyword::Break},
        {"continue", Keyword::Continue},
        {"return", Keyword::Return},
        {"null", Keyword::Null},
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
      case Capability::Clonable: return "Clonable";
      case Capability::Movable: return "Movable";
    }
    return "<capability>";
  }

  std::optional<Capability> capability_of(std::string_view text) noexcept {
    if (text == "Clonable") {
      return Capability::Clonable;
    }
    if (text == "Movable") {
      return Capability::Movable;
    }
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
    std::vector<const RecordInfo*> visiting;
    const auto supports = [&](const auto& self, const SemType& candidate) -> bool {
      if (candidate == SemType::Void) {
        return false;
      }
      if (capability == Capability::Clonable || capability == Capability::Movable) {
        if (!candidate.is_record()) {
          return true;
        }
        const auto* record = candidate.record;
        if (!record) {
          return false;
        }
        if (std::ranges::find(visiting, record) != visiting.end()) {
          return true;
        }
        visiting.push_back(record);
        bool result = true;
        if (capability == Capability::Clonable && record->methods.contains("clone")) {
          // A custom copy need not copy every type argument. Its body, including
          // any actual field copies, is checked when copying is requested.
        } else if (capability == Capability::Clonable && (!record->auto_clone || record->destructor)) {
          result = false;
        } else {
          for (const auto& field : record->fields) {
            result = result && self(self, field.type);
          }
        }
        visiting.pop_back();
        return result;
      }
      if (capability == Capability::Negatable) {
        return is_integer(candidate) && is_signed(candidate);
      }
      return is_integer(candidate);
    };
    return supports(supports, type);
  }

  const Token& TokenCursor::peek(std::size_t ahead) const noexcept {
    if (ahead == 0) {
      return current();
    }
    const std::size_t last = tokens_->size() - 1;
    const std::size_t index = index_ + ahead - (residual_ ? 1 : 0);
    return (*tokens_)[index < last ? index : last];
  }

  const Token& TokenCursor::take() noexcept {
    if (residual_) {
      last_split_ = *residual_;
      residual_.reset();
      return last_split_;
    }
    const Token& consumed = (*tokens_)[index_];
    const std::size_t last = tokens_->size() - 1;
    if (index_ < last) {
      ++index_;
    }
    return consumed;
  }

  bool TokenCursor::take_type_close() {
    if (current().type != TokenType::Operator || current().text.empty() || current().text.front() != '>') {
      return false;
    }
    Token token = take();
    if (token.text.size() > 1) {
      token.text.erase(0, 1);
      ++token.location.col;
      ++token.location.offset;
      residual_ = std::move(token);
    }
    return true;
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
    if (index < tokens_->size() && (*tokens_)[index].type == TokenType::Identifier &&
        keyword_of((*tokens_)[index].text) == Keyword::Move) {
      return past_operand(index + 1);
    }
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
    while (index < tokens_->size()) {
      if ((*tokens_)[index].type == TokenType::LBracket) {
        std::size_t depth = 1;
        for (++index; index < tokens_->size() && depth; ++index) {
          if ((*tokens_)[index].type == TokenType::LBracket) {
            ++depth;
          }
          if ((*tokens_)[index].type == TokenType::RBracket) {
            --depth;
          }
        }
      } else if (index + 1 < tokens_->size() && (*tokens_)[index].type == TokenType::Dot &&
                 (*tokens_)[index + 1].type == TokenType::Identifier) {
        index += 2;
        if (index < tokens_->size() && (*tokens_)[index].type == TokenType::LParen) {
          index = past_group(index);
        }
      } else {
        break;
      }
    }
    return index;
  }

  bool TokenCursor::moves_from_indirect_storage() const noexcept {
    if (!at_keyword(Keyword::Move)) {
      return false;
    }
    const auto end = past_operand(index_ + 1);
    for (auto index = index_ + 1; index < end; ++index) {
      const auto& token = (*tokens_)[index];
      if (token.type == TokenType::LBracket || (token.type == TokenType::Operator && token.text == "*")) {
        return true;
      }
    }
    return false;
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
      if (auto* found = find_global_in(**ns, name)) {
        return found;
      }
    }
    return nullptr;
  }

  GlobalInfo& ParseContext::global_of(const std::string& name) noexcept {
    return current_namespace().globals.at(name);
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      if (auto* found = find_functions_in(**ns, name)) {
        return found;
      }
    }
    return nullptr;
  }

  const TemplateInfo* ParseContext::find_template(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      if (auto* found = find_template_in(**ns, name)) {
        return found;
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

  namespace {
    // Follow aliases to the original declaration and enforce export visibility
    // when a lookup uses a namespace imported from another file.
    std::pair<const NameSpace*, const std::string&> member_source(const NameSpace& scope, const std::string& name) {
      if (scope.viewed_namespace) {
        return {scope.viewed_namespace->exports.contains(name) ? scope.viewed_namespace : nullptr, name};
      }
      if (const auto binding = scope.imports.find(name); binding != scope.imports.end()) {
        return {binding->second.source, binding->second.name};
      }
      return {&scope, name};
    }

    template<typename T>
    const T* find_member(const NameSpace& scope, const std::string& name,
                         const std::unordered_map<std::string, T> NameSpace::*members) {
      const auto [source, key] = member_source(scope, name);
      if (!source) {
        return nullptr;
      }
      const auto& table = source->*members;
      const auto found = table.find(key);
      return found != table.end() ? &found->second : nullptr;
    }
  }

  const GlobalInfo* ParseContext::find_global_in(const NameSpace& ns, const std::string& name) const noexcept {
    return find_member(ns, name, &NameSpace::globals);
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions_in(const NameSpace& ns,
                                                                   const std::string& name) const noexcept {
    return find_member(ns, name, &NameSpace::functions);
  }

  const TemplateInfo* ParseContext::find_template_in(const NameSpace& ns, const std::string& name) const noexcept {
    return find_member(ns, name, &NameSpace::templates);
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
    const auto* found = find_member(ns, name, &NameSpace::namespaces);
    if (!found) {
      return nullptr;
    }
    return ns.viewed_namespace || ns.imports.contains(name) ? (*found)->public_view.get() : found->get();
  }

  const NameSpace* ParseContext::find_namespace(const std::string& name) const noexcept {
    for (auto ns = namespace_stack_.rbegin(); ns != namespace_stack_.rend(); ++ns) {
      if (*ns == &root_namespace_ && in_file_scope() && name != "std") {
        break;
      }
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

  std::expected<const NameSpace*, ParseError> ParseContext::resolve_namespace(const std::vector<std::string>& path,
                                                                              SourceLocation location) const {
    const NameSpace* ns = nullptr;
    std::string qualified;
    for (std::size_t index = 0; index < path.size(); ++index) {
      if (!qualified.empty()) {
        qualified += '.';
      }
      qualified += path[index];
      ns = index == 0 ? find_namespace(path[index]) : find_namespace(*ns, path[index]);
      if (ns == nullptr) {
        return std::unexpected(ParseError {"unknown namespace '" + qualified + "'", location});
      }
    }
    return ns;
  }

  const NameSpace* ParseContext::find_module(const std::string& name) const noexcept {
    const auto found = root_namespace_.namespaces.find(name);
    return found != root_namespace_.namespaces.end() ? found->second.get() : nullptr;
  }

  bool ParseContext::in_file_scope() const noexcept {
    const auto& path = current_namespace().path;
    return !path.empty() && path.front().starts_with('/');
  }

  bool ParseContext::at_file_root() const noexcept {
    return current_namespace().path.empty() || (in_file_scope() && current_namespace().path.size() == 1);
  }

  bool ParseContext::at_entry_root() const noexcept {
    return at_file_root() && (!in_file_scope() || current_namespace().path.front() == "/0");
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
    child.public_view = std::make_unique<NameSpace>();
    child.public_view->viewed_namespace = &child;
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
    const std::size_t id = function_count++;
    instantiation_ids_.emplace(std::move(key), instantiations_.size());
    instantiations_.push_back(Instantiation {id, tmpl, type_args, false});
    return id;
  }

  void ParseContext::set_type_substitution(std::unordered_map<std::string, SemType> substitution) {
    type_substitution_ = std::move(substitution);
  }

  std::expected<RecordInfo*, ParseError> ParseContext::declare_record(const std::string& qualified,
                                                                      SourceLocation location) {
    if (record_names_.contains(qualified)) {
      return std::unexpected(ParseError {"'" + qualified + "' is already declared", location});
    }
    auto record = std::make_unique<RecordInfo>();
    record->name = qualified;
    record->location = location;
    const auto argument_start = qualified.find('<');
    const auto base = qualified.substr(0, argument_start);
    const auto last = base.rfind('.');
    if (last != std::string::npos) {
      std::size_t begin = 0;
      while (begin < last) {
        const auto dot = base.find('.', begin);
        record->ns_path.push_back(base.substr(begin, dot - begin));
        if (dot == last) {
          break;
        }
        begin = dot + 1;
      }
    }
    auto* result = record.get();
    records_.push_back(std::move(record));
    record_names_.emplace(qualified, result);
    return result;
  }

  RecordInfo* ParseContext::find_root_record(const std::string& qualified) const noexcept {
    auto found = record_names_.find(qualified);
    return found == record_names_.end() ? nullptr : found->second;
  }

  RecordInfo* ParseContext::find_record_in(const NameSpace& ns, const std::string& name) const noexcept {
    const auto [source, key] = member_source(ns, name);
    if (!source) {
      return nullptr;
    }
    std::string qualified;
    for (const auto& segment : source->path) {
      qualified += segment + '.';
    }
    return find_root_record(qualified + key);
  }

  RecordInfo* ParseContext::find_record(const std::string& name) const noexcept {
    const auto dot = name.find('.');
    const auto first = name.substr(0, dot);
    for (auto scope = namespace_stack_.rbegin(); scope != namespace_stack_.rend(); ++scope) {
      if (*scope == &root_namespace_ && in_file_scope() && first != "std") {
        break;
      }
      if (dot == std::string::npos) {
        if (auto* found = find_record_in(**scope, name)) {
          return found;
        }
        continue;
      }
      if ((*scope)->imports.contains(first)) {
        const NameSpace* ns = find_namespace(**scope, first);
        auto start = dot + 1;
        while (ns) {
          const auto next = name.find('.', start);
          if (next == std::string::npos) {
            return find_record_in(*ns, name.substr(start));
          }
          ns = find_namespace(*ns, name.substr(start, next - start));
          start = next + 1;
        }
        return nullptr;
      }
      // Discovered structures can be referenced before their enclosing
      // namespace has been visited by the declaration pass.
      if (auto* found = find_record_in(**scope, name)) {
        return found;
      }
      if ((*scope)->namespaces.contains(first)) {
        return nullptr;
      }
    }
    return nullptr;
  }

  std::expected<std::vector<SemType>, ParseError> ParseContext::read_type_arguments(TokenCursor& cursor) const {
    if (auto open = expect_op(cursor, "<"); !open) {
      return std::unexpected(open.error());
    }
    std::vector<SemType> arguments;
    do {
      auto argument = read_type(cursor);
      if (!argument) {
        return std::unexpected(argument.error());
      }
      arguments.push_back(*argument);
    } while (cursor.match(TokenType::Comma));
    if (!cursor.take_type_close()) {
      return std::unexpected(ParseError {"expected '>' after type arguments", cursor.location()});
    }
    return arguments;
  }

  std::expected<SemType, ParseError> ParseContext::resolve_type(const TemplateType& type,
                                                                const std::vector<SemType>& args,
                                                                SourceLocation location) const {
    if (type.pointee) {
      auto pointee = resolve_type(*type.pointee, args, location);
      if (!pointee) {
        return pointee;
      }
      return type.reference ? SemType::reference(*pointee, type.mutable_reference) : SemType::pointer(*pointee);
    }
    if (type.param >= 0) {
      return args[static_cast<std::size_t>(type.param)];
    }
    if (type.record_template) {
      std::vector<SemType> arguments;
      for (const auto& pattern : type.arguments) {
        auto argument = resolve_type(pattern, args, location);
        if (!argument) {
          return argument;
        }
        arguments.push_back(*argument);
      }
      if (!record_instantiator) {
        return std::unexpected(ParseError {"record instantiation is unavailable", location});
      }
      auto record = record_instantiator(type.record_template, std::move(arguments), location);
      if (!record) {
        return std::unexpected(record.error());
      }
      return SemType::record_type(*record);
    }
    return type.type;
  }

  std::expected<SemType, ParseError> ParseContext::read_type(TokenCursor& cursor, std::string_view context) const {
    if (cursor.at(TokenType::Identifier)) {
      const std::string name = cursor.current().text;
      if (name == "ptr" || name == "ref") {
        cursor.take();
        const bool mut = name == "ref" && cursor.at_keyword(Keyword::Mut);
        if (mut) {
          cursor.take();
        }
        auto pointee = read_type(cursor, context);
        if (!pointee) {
          return pointee;
        }
        return name == "ref" ? SemType::reference(*pointee, mut) : SemType::pointer(*pointee);
      }
      if (const auto found = type_substitution_.find(name);
          found != type_substitution_.end() && !cursor.peek().text.starts_with("<")) {
        cursor.take();
        return found->second;
      }
      auto scan = cursor;
      std::string qualified = scan.take().text;
      RecordInfo* record = find_record(qualified);
      auto end = scan;
      while (scan.at(TokenType::Dot) && scan.peek().type == TokenType::Identifier) {
        scan.take();
        qualified += '.' + scan.take().text;
        if (auto* found = find_record(qualified)) {
          record = found;
          end = scan;
        }
      }
      if (record) {
        cursor = end;
        if (cursor.at_op("<")) {
          auto arguments = read_type_arguments(cursor);
          if (!arguments) {
            return std::unexpected(arguments.error());
          }
          if (!record_instantiator) {
            return std::unexpected(ParseError {"record instantiation is unavailable", cursor.location()});
          }
          auto instance = record_instantiator(record, std::move(*arguments), cursor.location());
          if (!instance) {
            return std::unexpected(instance.error());
          }
          return SemType::record_type(*instance);
        }
        if (!record->type_params.empty()) {
          return std::unexpected(
              ParseError {"generic type '" + record->name + "' needs type arguments", cursor.location()});
        }
        return SemType::record_type(record);
      }
    }
    return expect_type(cursor, context);
  }

  std::expected<void, ParseError> ParseContext::declare_local(const std::string& name, LocalInfo info) {
    Scope& scope = scopes_.back();
    if (scope.locals.contains(name)) {
      return std::unexpected(ParseError {"'" + name + "' is already declared in this scope", info.location});
    }
    info.flow_id = local_names_.size();
    local_names_.push_back(name);
    origin_generations_.push_back(0);
    interior_generations_.push_back(0);
    origin_alive_.push_back(true);
    if (info.parameter && type_has_references(info.sem_type) && info.origins.empty()) {
      info.origins.push_back(make_origin(info.flow_id, true, false));
    }
    if (info.sem_type.is_record()) {
      info.live_flag = middleend::Operand::vreg(middleend::Type::I8, builder.alloc(middleend::Type::I8));
      builder.emit(middleend::Op::Mov, {*info.live_flag, middleend::Operand::instant(middleend::Type::I8, 0)});
      if (!info.assigned) {
        builder.emit(middleend::Op::Mov,
                     {middleend::Operand::vreg(info.type, info.vreg), middleend::Operand::instant(info.type, 0)});
      }
    }
    scope.declaration_order.push_back(name);
    scope.locals.emplace(name, info);
    if (info.assigned) {
      initialize_local(info);
    }
    return {};
  }

  FunctionInfo* ParseContext::function_by_id(std::size_t id) noexcept {
    const auto find = [&](const auto& self, NameSpace& ns) -> FunctionInfo* {
      for (auto& [name, overloads] : ns.functions) {
        for (auto& function : overloads) {
          if (function.id == id) {
            return &function;
          }
        }
      }
      for (auto& [name, child] : ns.namespaces) {
        if (auto* function = self(self, *child)) {
          return function;
        }
      }
      return nullptr;
    };
    if (auto* function = find(find, root_namespace_)) {
      return function;
    }
    for (auto& record : records_) {
      for (auto& [name, overloads] : record->methods) {
        for (auto& method : overloads) {
          if (method.function.id == id) {
            return &method.function;
          }
        }
      }
      if (record->destructor && record->destructor->function.id == id) {
        return &record->destructor->function;
      }
    }
    return nullptr;
  }

  void ParseContext::push_scope() {
    scopes_.push_back(Scope {});
    // Every cell from here on belongs to this scope, and goes back to the frame's
    // free list when it closes.
    scopes_.back().vreg_high_water = builder.next_vreg();
  }

  LocalInfo* ParseContext::local_at(std::uint64_t vreg) noexcept {
    for (auto& scope : scopes_) {
      for (auto& [name, local] : scope.locals) {
        if (local.vreg == vreg) {
          return &local;
        }
      }
    }
    return nullptr;
  }

  void ParseContext::pop_scope() {
    // Nothing in a closed scope is live afterwards, so its cells are reusable.
    // The frame keeps its high-water mark: its size is a compile-time constant.
    for (const auto& [name, local] : scopes_.back().locals) {
      record_invalidation(local.flow_id, false);
      origin_alive_[local.flow_id] = false;
    }
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
        snapshot.flags.push_back({&entry.second, entry.second.assigned, entry.second.moved, entry.second.origins,
                                  origin_generations_[entry.second.flow_id],
                                  interior_generations_[entry.second.flow_id]});
      }
    }
    return snapshot;
  }

  void ParseContext::merge_assigned(const AssignedSnapshot& before) {
    for (const auto& entry : before.flags) {
      entry.local->assigned = entry.local->assigned && entry.assigned;
      entry.local->moved = entry.local->moved || entry.moved;
      for (const auto& origin : entry.origins) {
        if (std::ranges::find(entry.local->origins, origin) == entry.local->origins.end()) {
          entry.local->origins.push_back(origin);
        }
      }
      origin_generations_[entry.local->flow_id] = std::max(origin_generations_[entry.local->flow_id], entry.generation);
      interior_generations_[entry.local->flow_id] =
          std::max(interior_generations_[entry.local->flow_id], entry.interior_generation);
    }
  }

  void ParseContext::set_assigned(const AssignedSnapshot& snapshot) noexcept {
    for (const auto& entry : snapshot.flags) {
      entry.local->assigned = entry.assigned;
      entry.local->moved = entry.moved;
      entry.local->origins = entry.origins;
      origin_generations_[entry.local->flow_id] = entry.generation;
      interior_generations_[entry.local->flow_id] = entry.interior_generation;
    }
  }

  void ParseContext::read_local(const LocalInfo& local, SourceLocation location) {
    local_events_.push_back({(builder.terminated() ? builder.open_block() : builder.current()), local.flow_id,
                             LocalEvent::Kind::Read, location});
  }

  void ParseContext::initialize_local(const LocalInfo& local) {
    if (local.live_flag) {
      builder.emit(middleend::Op::Mov, {*local.live_flag, middleend::Operand::instant(middleend::Type::I8, 1)});
    }
    local_events_.push_back({(builder.terminated() ? builder.open_block() : builder.current()), local.flow_id,
                             LocalEvent::Kind::Initialize, local.location});
  }

  void ParseContext::move_local(LocalInfo& local) {
    if (local.live_flag) {
      builder.emit(middleend::Op::Mov, {*local.live_flag, middleend::Operand::instant(middleend::Type::I8, 0)});
    }
    record_invalidation(local.flow_id, false);
    ++origin_generations_[local.flow_id];
    ++interior_generations_[local.flow_id];
    local.origins.clear();
    local_events_.push_back({(builder.terminated() ? builder.open_block() : builder.current()), local.flow_id,
                             LocalEvent::Kind::Move, local.location});
  }

  std::expected<void, ParseError> ParseContext::check_local_flow() const {
    if (cleanup_error_) {
      return std::unexpected(*cleanup_error_);
    }
    const auto& blocks = builder.cfg().blocks;
    if (blocks.empty()) {
      return {};
    }
    std::vector<bool> reachable(blocks.size(), false);
    std::vector<std::size_t> pending {0};
    std::vector<std::vector<std::size_t>> predecessors(blocks.size());
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (reachable[id]) {
        continue;
      }
      reachable[id] = true;
      const auto edge = [&](std::size_t target) {
        predecessors[target].push_back(id);
        pending.push_back(target);
      };
      if (blocks[id].next) {
        edge(*blocks[id].next);
      }
      if (blocks[id].branch) {
        edge(blocks[id].branch->target);
      }
    }
    std::vector<std::vector<const LocalEvent*>> events(blocks.size());
    for (const auto& event : local_events_) {
      events[event.block].push_back(&event);
    }
    const std::size_t count = local_names_.size();
    std::vector<std::vector<bool>> incoming(blocks.size(), std::vector<bool>(count, true));
    auto outgoing = incoming;
    bool changed;
    do {
      changed = false;
      for (std::size_t id = 0; id < blocks.size(); ++id) {
        if (!reachable[id]) {
          continue;
        }
        std::vector<bool> state(count, id != 0);
        for (const auto parent : predecessors[id]) {
          for (std::size_t local = 0; local < count; ++local) {
            state[local] = state[local] && outgoing[parent][local];
          }
        }
        incoming[id] = state;
        for (const auto* event : events[id]) {
          if (event->kind != LocalEvent::Kind::Read) {
            state[event->local] = event->kind == LocalEvent::Kind::Initialize;
          }
        }
        if (state != outgoing[id]) {
          outgoing[id] = std::move(state);
          changed = true;
        }
      }
    } while (changed);
    for (std::size_t id = 0; id < blocks.size(); ++id) {
      if (!reachable[id]) {
        continue;
      }
      auto state = incoming[id];
      for (const auto* event : events[id]) {
        if (event->kind == LocalEvent::Kind::Read) {
          if (!state[event->local]) {
            return std::unexpected(
                ParseError {"'" + local_names_[event->local] + "' may be uninitialized or moved on a control-flow path",
                            event->location});
          }
        } else {
          state[event->local] = event->kind == LocalEvent::Kind::Initialize;
        }
      }
    }
    if (!loans_.empty()) {
      std::vector<std::vector<const LoanEvent*>> reference_events(blocks.size());
      for (const auto& event : loan_events_) {
        reference_events[event.block].push_back(&event);
      }
      std::vector<std::vector<bool>> loan_in(blocks.size(), std::vector<bool>(loans_.size(), true));
      auto loan_out = loan_in;
      const auto apply = [&](const LoanEvent& event, std::vector<bool>& state) {
        if (event.kind == LoanEvent::Kind::Borrow) {
          state[event.subject] = true;
        } else if (event.kind == LoanEvent::Kind::Invalidate || event.kind == LoanEvent::Kind::InvalidateInterior) {
          for (std::size_t loan = 0; loan < loans_.size(); ++loan) {
            if (loans_[loan].root == event.subject && loans_[loan].external == event.external &&
                (event.kind == LoanEvent::Kind::Invalidate || loans_[loan].interior)) {
              state[loan] = false;
            }
          }
        }
      };
      do {
        changed = false;
        for (std::size_t id = 0; id < blocks.size(); ++id) {
          if (!reachable[id]) {
            continue;
          }
          std::vector<bool> state(loans_.size(), id != 0);
          for (const auto parent : predecessors[id]) {
            for (std::size_t loan = 0; loan < loans_.size(); ++loan) {
              state[loan] = state[loan] && loan_out[parent][loan];
            }
          }
          loan_in[id] = state;
          for (const auto* event : reference_events[id]) {
            apply(*event, state);
          }
          if (state != loan_out[id]) {
            loan_out[id] = std::move(state);
            changed = true;
          }
        }
      } while (changed);
      for (std::size_t id = 0; id < blocks.size(); ++id) {
        if (!reachable[id]) {
          continue;
        }
        auto state = loan_in[id];
        for (const auto* event : reference_events[id]) {
          if (event->kind == LoanEvent::Kind::Read && !state[event->subject]) {
            return std::unexpected(ParseError {"reference may be invalidated on a control-flow path", event->location});
          }
          apply(*event, state);
        }
      }
    }
    return {};
  }

  void ParseContext::reset_frame(std::vector<middleend::Type> params) {
    scopes_.clear();
    loops_.clear();
    builder = middleend::CfgBuilder {std::move(params)};
    local_events_.clear();
    local_names_.clear();
    deferred_local_reads = false;
    temporaries_.clear();
    loans_.clear();
    loan_events_.clear();
    origin_generations_.clear();
    interior_generations_.clear();
    origin_alive_.clear();
    cleanup_error_.reset();
  }

}  // namespace erax::frontend
