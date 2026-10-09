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
      if (entry.text == text)
        return entry.keyword;
    }
    return Keyword::None;
  }

  std::string_view to_string(Keyword keyword) noexcept {
    for (const KeywordName& entry : keywords) {
      if (entry.keyword == keyword)
        return entry.text;
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
    if (text == "Ordered")
      return Capability::Ordered;
    if (text == "Negatable")
      return Capability::Negatable;
    if (text == "Arithmetic")
      return Capability::Arithmetic;
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
    if (index_ < last)
      ++index_;
    return consumed;
  }

  bool TokenCursor::match(TokenType type) {
    if (!at(type))
      return false;
    take();
    return true;
  }

  bool TokenCursor::match_op(std::string_view text) {
    if (!at_op(text))
      return false;
    take();
    return true;
  }

  bool TokenCursor::else_follows() const {
    std::size_t index = index_ + 1;
    if (index >= tokens_->size() || (*tokens_)[index].type != TokenType::LBrace)
      return false;
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
    if (index >= tokens_->size())
      return false;
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
      if (text != "-" && text != "!" && text != "~")
        break;
      ++index;
    }
    // What follows the operand is the one token after it, so the operand's own extent is
    // stepped over exactly: a `( … )` group, a literal or a name, and a call's arguments
    // after that name. Anything else, including a `;`, is where the operand ends.
    if (index < tokens_->size() && (*tokens_)[index].type == TokenType::LParen) {
      index = past_group(index);
    } else if (index < tokens_->size() && (*tokens_)[index].type == TokenType::Identifier) {
      ++index;
      if (index < tokens_->size() && (*tokens_)[index].type == TokenType::LParen)
        index = past_group(index);
    } else if (index < tokens_->size()) {
      ++index;
    }
    return index;
  }

  bool TokenCursor::operator_follows(std::span<const std::string_view> operators) const {
    const std::size_t index = past_operand(index_);
    if (index >= tokens_->size() || (*tokens_)[index].type != TokenType::Operator)
      return false;
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
        if (depth == 0)
          return;
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
      if (found != scope->locals.end())
        return &found->second;
    }
    return nullptr;
  }

  LocalInfo* ParseContext::find_local(const std::string& name) noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto found = scope->locals.find(name);
      if (found != scope->locals.end())
        return &found->second;
    }
    return nullptr;
  }

  const GlobalInfo* ParseContext::find_global(const std::string& name) const noexcept {
    const auto found = globals_.find(name);
    return found != globals_.end() ? &found->second : nullptr;
  }

  GlobalInfo& ParseContext::global_of(const std::string& name) noexcept {
    return globals_.at(name);
  }

  const std::vector<FunctionInfo>* ParseContext::find_functions(const std::string& name) const noexcept {
    const auto found = functions_.find(name);
    return found != functions_.end() ? &found->second : nullptr;
  }

  const TemplateInfo* ParseContext::find_template(const std::string& name) const noexcept {
    const auto found = templates_.find(name);
    return found != templates_.end() ? &found->second : nullptr;
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

  bool ParseContext::declares(const std::string& name) const noexcept {
    return !scopes_.empty() && scopes_.back().locals.contains(name);
  }

  std::size_t ParseContext::declare_global(const std::string& name, GlobalInfo info) {
    const std::size_t id = global_count++;
    info.id = id;
    globals_[name] = info;
    return id;
  }

  std::size_t ParseContext::declare_function(const std::string& name, FunctionInfo info) {
    const std::size_t id = function_count++;
    info.id = id;
    functions_[name].push_back(std::move(info));
    return id;
  }

  void ParseContext::declare_template(const std::string& name, TemplateInfo info) {
    templates_[name] = std::move(info);
  }

  std::expected<std::size_t, ParseError> ParseContext::instantiate(const std::string& name,
                                                                   const std::vector<SemType>& type_args,
                                                                   SourceLocation location) {
    std::string key = name;
    for (const SemType type : type_args) {
      key += ':';
      key += to_string(type);
    }
    const auto found = instantiation_ids_.find(key);
    if (found != instantiation_ids_.end())
      return instantiations_[found->second].id;
    if (instantiations_.size() >= max_instantiations) {
      return std::unexpected(ParseError {"template instantiation of '" + name + "' does not terminate", location});
    }
    // Source functions take ids `0..function_count-1` as the declaration pass gives
    // them out, and every instantiation is appended to the program after all of
    // them. The two are numbered in the same order they are first requested, so the
    // id a call is written with is the index the function ends up at.
    const std::size_t id = function_count + instantiations_.size();
    instantiation_ids_.emplace(std::move(key), instantiations_.size());
    instantiations_.push_back(Instantiation {id, name, type_args, false});
    return id;
  }

  void ParseContext::set_type_substitution(std::unordered_map<std::string, SemType> substitution) {
    type_substitution_ = std::move(substitution);
  }

  std::expected<SemType, ParseError> ParseContext::read_type(TokenCursor& cursor, std::string_view context) const {
    // While an instantiation's body is parsed, a type parameter names the deduced
    // concrete type; everywhere else the substitution is empty and this is the
    // ordinary type read.
    if (cursor.at(TokenType::Identifier)) {
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
