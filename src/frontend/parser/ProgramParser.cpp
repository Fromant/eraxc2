#include "ProgramParser.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "middleend/jir/Builder.hpp"
#include "middleend/jir/Jir.hpp"
#include "primitives/Primitives.hpp"

namespace erax::frontend {

  using middleend::CfgBuilder;
  using middleend::Condition;
  using middleend::Global;
  using middleend::Op;
  using middleend::Operand;
  using middleend::Program;
  using middleend::Type;

  namespace {

    /// The legal operator texts, from `docs/core/language.md#operator-set`. A token
    /// whose text is not one of them is a compile-time error, which is why this is a
    /// pass of its own: `a+++++b` is one token, and reporting it where it is written
    /// beats reading it as a missing `+`.
    constexpr std::string_view operators[] = {
        "+",  "-",  "*",  "/", "%",  "<<", ">>", "<",  "<=", ">",  ">=", "==", "!=",  "&",   "|", "^",
        "||", "^^", "&&", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "!", "~",
    };

    /// The compound assignments and the op each one means.
    struct CompoundAssign {
      std::string_view text;
      Op op;
    };

    constexpr CompoundAssign compound_assigns[] = {
        {"+=", Op::Add}, {"-=", Op::Sub}, {"*=", Op::Mul}, {"/=", Op::Sdiv}, {"%=", Op::Smod},
        {"&=", Op::And}, {"|=", Op::Or},  {"^=", Op::Xor}, {"<<=", Op::Shl}, {">>=", Op::Sar},
    };

    bool at_assignment_operator(const TokenCursor& cursor) {
      if (cursor.at_op("=")) {
        return true;
      }
      return std::ranges::any_of(compound_assigns, [&](const CompoundAssign& op) {
        return cursor.at_op(op.text);
      });
    }

    bool rhs_may_call(const TokenCursor& cursor) {
      const auto& tokens = cursor.tokens();
      int parentheses = 0;
      for (std::size_t i = cursor.index(); i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::Semicolon || tokens[i].type == TokenType::EndOfFile) {
          break;
        }
        if (tokens[i].type == TokenType::RParen) {
          if (!parentheses) {
            break;
          }
          --parentheses;
        } else if (tokens[i].type == TokenType::LParen) {
          if (i && tokens[i - 1].type == TokenType::Identifier) {
            return true;
          }
          ++parentheses;
        }
      }
      return false;
    }

    std::string quoted(std::string_view text) {
      return "'" + std::string {text} + "'";
    }

    /// A type's name, for a message.
    std::string type_name(SemType type) {
      return std::string {to_string(type)};
    }

    /// A dotted name from its segments (`a`, `b`, `c` → `a.b.c`).
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

    /// A name as a message spells it: the namespace path it lives in, dotted, then
    /// the name itself (`std.print`).
    std::string qualified_name(const NameSpace& ns, const std::string& name) {
      std::vector<std::string> parts = ns.path;
      parts.push_back(name);
      return qualified_path(parts);
    }

    /// Whether the token after the name at the cursor is an assignment operator:
    /// what tells `name = …` from a statement that merely starts with a name. A
    /// qualified target (`a.b.g = …`) is stepped over first, segment by segment.
    bool at_assignment(const TokenCursor& cursor) noexcept {
      const std::vector<Token>& tokens = cursor.tokens();
      std::size_t index = cursor.index() + 1;
      while (index + 1 < tokens.size() && tokens[index].type == TokenType::Dot &&
             tokens[index + 1].type == TokenType::Identifier) {
        index += 2;
      }
      if (index >= tokens.size() || tokens[index].type != TokenType::Operator) {
        return false;
      }
      const std::string& text = tokens[index].text;
      if (text == "=") {
        return true;
      }
      for (const CompoundAssign& compound : compound_assigns) {
        if (text == compound.text) {
          return true;
        }
      }
      return false;
    }

    /// Whether a `*` at the cursor begins a store through a pointer: it is, when
    /// the operand it dereferences is followed by an assignment operator. The
    /// operand is stepped over with `past_operand`, so `*p = …` and `**pp += …`
    /// are both recognized and a bare `*p;` load is not.
    bool at_deref_assignment(const TokenCursor& cursor) noexcept {
      const std::vector<Token>& tokens = cursor.tokens();
      std::size_t index = cursor.index();
      if (index >= tokens.size() || tokens[index].type != TokenType::Operator || tokens[index].text != "*") {
        return false;
      }
      index = cursor.past_operand(index + 1);
      if (index >= tokens.size() || tokens[index].type != TokenType::Operator) {
        return false;
      }
      const std::string& text = tokens[index].text;
      if (text == "=") {
        return true;
      }
      for (const CompoundAssign& compound : compound_assigns) {
        if (text == compound.text) {
          return true;
        }
      }
      return false;
    }

    /// A type in a function signature: a type keyword, a type parameter of the
    /// enclosing template, or a note that `string` has no type yet.
    std::expected<TemplateType, ParseError> expect_signature_type(TokenCursor& cursor,
                                                                  const std::vector<TypeParam>& type_params,
                                                                  ParseContext& context) {
      const auto location = cursor.location();
      if (!cursor.at(TokenType::Identifier)) {
        return std::unexpected(ParseError {"expected a type", location});
      }
      const auto text = cursor.current().text;
      TemplateType pattern;
      if (text == "ptr" || text == "ref") {
        cursor.take();
        pattern.reference = text == "ref";
        pattern.mutable_reference = pattern.reference && cursor.at_keyword(Keyword::Mut);
        if (pattern.mutable_reference) {
          cursor.take();
        }
        auto inner = expect_signature_type(cursor, type_params, context);
        if (!inner) {
          return inner;
        }
        pattern.pointee = std::make_shared<const TemplateType>(*inner);
      } else {
        for (std::size_t i = 0; i < type_params.size(); ++i) {
          if (type_params[i].name == text) {
            cursor.take();
            return TemplateType {SemType::Void, static_cast<int>(i)};
          }
        }
        auto scan = cursor;
        std::string name = scan.take().text;
        while (scan.match(TokenType::Dot)) {
          auto part = expect_identifier(scan);
          if (!part) {
            return std::unexpected(part.error());
          }
          name += "." + *part;
        }
        auto* record = context.find_record(name);
        if (record && !record->type_params.empty() && scan.match_op("<")) {
          pattern.record_template = record;
          do {
            auto argument = expect_signature_type(scan, type_params, context);
            if (!argument) {
              return argument;
            }
            pattern.arguments.push_back(*argument);
          } while (scan.match(TokenType::Comma));
          if (!scan.take_type_close()) {
            return std::unexpected(ParseError {"expected '>'", scan.location()});
          }
          cursor = scan;
        } else {
          auto type = context.read_type(cursor);
          if (!type) {
            return std::unexpected(type.error());
          }
          return TemplateType {*type, -1};
        }
      }
      bool dependent = false;
      for (std::size_t i = 0; i < type_params.size(); ++i) {
        dependent |= pattern.contains(i);
      }
      if (!dependent) {
        auto type = context.resolve_type(pattern, {}, location);
        if (!type) {
          return std::unexpected(type.error());
        }
        return TemplateType {*type, -1};
      }
      return pattern;
    }

  }  // namespace

  ProgramParser::ProgramParser(const std::vector<Token>& tokens) : context_(tokens), expressions_(context_) {
    context_.record_instantiator = [this](RecordInfo* record, std::vector<SemType> args, SourceLocation location) {
      return instantiate_record(record, std::move(args), location);
    };
    context_.clone_instantiator = [this](RecordInfo* record, SourceLocation) -> std::expected<std::size_t, ParseError> {
      record->clone_id = context_.function_count++;
      if (auto found = record->methods.find("clone"); found != record->methods.end() && !found->second.empty()) {
        found->second.front().function.id = *record->clone_id;
      }
      return *record->clone_id;
    };
  }

  ProgramParser::result ProgramParser::parse() {
    if (auto checked = operator_whitelist(); !checked) {
      return std::unexpected(checked.error());
    }
    TokenCursor discovery(context_.cursor.tokens());
    if (auto discovered = discover_records(discovery, {}); !discovered) {
      return std::unexpected(discovered.error());
    }
    if (auto declared = declarations(); !declared) {
      return std::unexpected(declared.error());
    }

    declarations_complete_ = true;
    for (std::size_t i = 0; i < context_.records().size(); ++i) {
      auto* record = context_.records()[i].get();
      if (record->template_origin && !record->members_complete) {
        auto completed = instantiate_record(record->template_origin, record->type_args, record->location);
        if (!completed) {
          return std::unexpected(completed.error());
        }
      }
    }
    if (auto validated = validate_record_ownership(); !validated) {
      return std::unexpected(validated.error());
    }
    infer_function_effects();
    program_.globals.resize(context_.global_count);
    if (auto parsed = items(/*top=*/true); !parsed) {
      return std::unexpected(parsed.error());
    }

    // A call above may have deduced a template's type arguments; each such
    // instantiation is a function of its own, parsed now, one per entry — but a
    // body can request further instantiations, hence the check on the running size
    // rather than a snapshot.
    std::size_t next_instantiation = 0;
    while (true) {
      if (auto lowered = lower_record_methods(); !lowered) {
        return std::unexpected(lowered.error());
      }
      if (next_instantiation == context_.instantiations().size()) {
        break;
      }
      const Instantiation inst = context_.instantiations()[next_instantiation++];
      if (inst.generated) {
        continue;
      }
      if (auto parsed = instantiate_function(inst); !parsed) {
        return std::unexpected(parsed.error());
      }
      context_.instantiations()[next_instantiation - 1].generated = true;
    }

    if (!has_entrypoint_) {
      return std::unexpected(ParseError {"the program must declare 'fn main(): i32'", context_.cursor.location()});
    }
    generate_entrypoint_cleanup();
    program_.entrypoint = entrypoint_;
    return std::move(program_);
  }

  ProgramParser::step ProgramParser::items(bool top) {
    TokenCursor& cursor = context_.cursor;
    while (!cursor.at_end()) {
      if (!top && cursor.at(TokenType::RBrace)) {
        return {};
      }
      step item;
      if (cursor.current().text == "struct") {
        cursor.take();
        while (!cursor.at(TokenType::LBrace) && !cursor.at_end()) {
          cursor.take();
        }
        item = skip_body(cursor);
      } else if (cursor.at_keyword(Keyword::Fn)) {
        item = function();
      } else if (cursor.at_keyword(Keyword::Let) || cursor.at_keyword(Keyword::Const)) {
        item = global();
      } else if (cursor.at_keyword(Keyword::Namespace)) {
        item = namespace_body();
      } else {
        return std::unexpected(
            ParseError {"expected a global, a function, or a namespace declaration", cursor.location()});
      }
      if (!item) {
        return std::unexpected(item.error());
      }
    }
    if (!top) {
      return std::unexpected(ParseError {"unterminated namespace body", cursor.location()});
    }
    return {};
  }

  ProgramParser::step ProgramParser::namespace_body() {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation start = cursor.location();
    cursor.take();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    auto open = expect(cursor, TokenType::LBrace);
    if (!open) {
      return std::unexpected(open.error());
    }
    // The pre-pass already created the namespace; this pass only enters it so its
    // items resolve against the right symbol tables.
    NameSpace* child = context_.find_namespace_in_current(*name);
    if (child == nullptr) {
      return std::unexpected(ParseError {"internal error: no namespace '" + *name + "' to enter", start});
    }
    context_.push_namespace(*child);
    if (auto body = items(/*top=*/false); !body) {
      context_.pop_namespace();
      return std::unexpected(body.error());
    }
    context_.pop_namespace();
    if (auto close = expect(cursor, TokenType::RBrace); !close) {
      return std::unexpected(close.error());
    }
    return {};
  }

  ProgramParser::step ProgramParser::operator_whitelist() {
    const auto& tokens = context_.cursor.tokens();
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      const auto& token = tokens[index];
      if (token.type != TokenType::Operator ||
          (token.text == "?" && index >= 2 && tokens[index - 1].text == "mut" && tokens[index - 2].text == "ref")) {
        continue;
      }
      if (token.text.size() > 2 && token.text.find_first_not_of('>') == std::string::npos) {
        continue;  // Nested type closers are split only in type positions.
      }
      bool known = false;
      for (const std::string_view text : operators) {
        if (token.text == text) {
          known = true;
        }
      }
      if (!known) {
        return std::unexpected(ParseError {"unknown operator " + quoted(token.text), token.location});
      }
    }
    return {};
  }

  std::expected<ProgramParser::Signature, ParseError> ProgramParser::signature(TokenCursor& cursor,
                                                                               bool allow_receiver_result) {
    Signature signature;
    const SourceLocation start = cursor.location();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    signature.name = *name;
    signature.location = start;

    // `<T, U extends Ordered & Negatable>` makes the declaration a template; the
    // names are bound here and may be used as types in the parameter list below.
    if (cursor.at_op("<")) {
      cursor.take();
      while (true) {
        auto param = expect_identifier(cursor);
        if (!param) {
          return std::unexpected(param.error());
        }
        for (const TypeParam& existing : signature.type_params) {
          if (existing.name == *param) {
            return std::unexpected(ParseError {"type parameter '" + *param + "' is declared twice", cursor.location()});
          }
        }
        TypeParam type_param;
        type_param.name = *param;
        if (cursor.at_keyword(Keyword::Extends)) {
          cursor.take();
          while (true) {
            if (!cursor.at(TokenType::Identifier)) {
              return std::unexpected(ParseError {"expected a capability after 'extends'", cursor.location()});
            }
            const std::string capability = cursor.current().text;
            const std::optional<Capability> known = capability_of(capability);
            if (!known) {
              return std::unexpected(ParseError {"'" + capability + "' is not a capability", cursor.location()});
            }
            if (std::ranges::find(type_param.bounds, *known) != type_param.bounds.end()) {
              return std::unexpected(ParseError {"capability '" + capability + "' is listed twice", cursor.location()});
            }
            type_param.bounds.push_back(*known);
            cursor.take();
            if (!cursor.match_op("&")) {
              break;
            }
          }
        }
        signature.type_params.push_back(std::move(type_param));
        if (!cursor.match(TokenType::Comma)) {
          break;
        }
      }
      if (auto close = expect_op(cursor, ">"); !close) {
        return std::unexpected(close.error());
      }
    }

    if (auto open = expect(cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    if (!cursor.at(TokenType::RParen)) {
      while (true) {
        auto param = expect_identifier(cursor);
        if (!param) {
          return std::unexpected(param.error());
        }
        if (auto colon = expect(cursor, TokenType::Colon); !colon) {
          return std::unexpected(colon.error());
        }
        auto type = expect_signature_type(cursor, signature.type_params, context_);
        if (!type) {
          return std::unexpected(type.error());
        }
        signature.param_names.push_back(*param);
        signature.params.push_back(*type);
        if (!cursor.match(TokenType::Comma)) {
          break;
        }
      }
    }
    if (auto close = expect(cursor, TokenType::RParen); !close) {
      return std::unexpected(close.error());
    }
    if (cursor.match(TokenType::Colon)) {
      if (allow_receiver_result && cursor.at_keyword(Keyword::Ref) && cursor.peek().text == "mut" &&
          cursor.peek(2).text == "?") {
        cursor.take();
        cursor.take();
        cursor.take();
        auto pointee = expect_signature_type(cursor, signature.type_params, context_);
        if (!pointee) {
          return std::unexpected(pointee.error());
        }
        signature.receiver_result = true;
        signature.return_type.reference = true;
        signature.return_type.pointee = std::make_shared<const TemplateType>(*pointee);
        return signature;
      }
      auto type = expect_signature_type(cursor, signature.type_params, context_);
      if (!type) {
        return std::unexpected(type.error());
      }
      signature.return_type = *type;
    }
    return signature;
  }

  ProgramParser::step ProgramParser::declarations() {
    TokenCursor cursor(context_.cursor.tokens());
    return declarations_in(cursor, /*top=*/true);
  }

  ProgramParser::step ProgramParser::declarations_in(TokenCursor& cursor, bool top) {
    while (!cursor.at_end()) {
      if (!top && cursor.at(TokenType::RBrace)) {
        return {};
      }
      if (cursor.current().text == "struct") {
        if (auto declared = declare_record(cursor); !declared) {
          return std::unexpected(declared.error());
        }
      } else if (cursor.at_keyword(Keyword::Let) || cursor.at_keyword(Keyword::Const)) {
        if (auto declared = declare_global(cursor); !declared) {
          return std::unexpected(declared.error());
        }
      } else if (cursor.at_keyword(Keyword::Fn)) {
        if (auto declared = declare_function(cursor); !declared) {
          return std::unexpected(declared.error());
        }
      } else if (cursor.at_keyword(Keyword::Namespace)) {
        if (auto declared = declare_namespace(cursor); !declared) {
          return std::unexpected(declared.error());
        }
      } else {
        // Not an item. The main pass is what reports anything wrong with it;
        // this pass only needs to step over it.
        cursor.take();
      }
    }
    if (!top) {
      return std::unexpected(ParseError {"unterminated namespace body", cursor.location()});
    }
    return {};
  }

  ProgramParser::step ProgramParser::declare_namespace(TokenCursor& cursor) {
    const SourceLocation start = cursor.location();
    cursor.take();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    if (auto open = expect(cursor, TokenType::LBrace); !open) {
      return std::unexpected(open.error());
    }
    auto child = context_.declare_namespace(*name, start);
    if (!child) {
      return std::unexpected(child.error());
    }
    context_.push_namespace(**child);
    if (auto body = declarations_in(cursor, /*top=*/false); !body) {
      context_.pop_namespace();
      return std::unexpected(body.error());
    }
    context_.pop_namespace();
    if (auto close = expect(cursor, TokenType::RBrace); !close) {
      return std::unexpected(close.error());
    }
    return {};
  }

  ProgramParser::step ProgramParser::declare_global(TokenCursor& cursor) {
    const bool reassignable = cursor.take().text == "let";
    const bool mutable_object = cursor.at_keyword(Keyword::Mut);
    if (mutable_object) {
      cursor.take();
    }
    const SourceLocation start = cursor.location();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    if (auto colon = expect(cursor, TokenType::Colon); !colon) {
      return std::unexpected(colon.error());
    }
    auto type = context_.read_type(cursor);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (type_has_references(*type)) {
      return std::unexpected(ParseError {"global variables cannot contain safe references", cursor.location()});
    }
    if (auto equals = expect_op(cursor, "="); !equals) {
      return std::unexpected(equals.error());
    }
    if (context_.is_declared_in_current(*name)) {
      return std::unexpected(ParseError {"'" + *name + "' is already declared", start});
    }
    // The id is assigned here, before the value: a function declared above a
    // global may still read it, since globals are visible everywhere.
    context_.declare_global(*name, GlobalInfo {lower(*type), *type, 0, std::nullopt, reassignable, mutable_object});
    // The initializer itself is left to the main pass, which folds it; it is a
    // constant expression, so it holds no `;` or `}` of its own.
    cursor.skip_to_sync();
    return {};
  }

  ProgramParser::step ProgramParser::declare_function(TokenCursor& cursor) {
    cursor.take();
    auto parsed = signature(cursor);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    const Signature found = *parsed;

    // A name belongs to one kind of thing in a namespace: a function may not share
    // it with a global or a namespace. It may share it with a template, though —
    // the concrete declaration is preferred over the template at a call.
    if (context_.find_global_in_current(found.name) != nullptr ||
        context_.find_namespace_in_current(found.name) != nullptr ||
        context_.find_record(qualified_name(context_.current_namespace(), found.name)) != nullptr) {
      return std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location});
    }

    if (!found.type_params.empty()) {
      // A template is not an overload; it is recorded beside the concrete
      // declarations and instantiated per call.
      if (found.name == "main") {
        return std::unexpected(ParseError {"the entry point must be 'fn main(): i32'", found.location});
      }
      if (context_.find_template_in_current(found.name) != nullptr) {
        return std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location});
      }
      // Deduction works from the parameter types, so a type parameter that never
      // appears in one has nothing to bind it (`docs/core/statements.md#templates`).
      for (std::size_t index = 0; index < found.type_params.size(); ++index) {
        bool used = false;
        for (const TemplateType& param : found.params) {
          if (param.contains(index)) {
            used = true;
          }
        }
        if (!used) {
          return std::unexpected(ParseError {
              "type parameter '" + found.type_params[index].name + "' of '" + found.name + "' is not in the parameters",
              found.location});
        }
      }
      TemplateInfo info;
      info.name = found.name;
      info.location = found.location;
      info.type_params = found.type_params;
      info.param_names = found.param_names;
      info.params = found.params;
      info.return_type = found.return_type;
      // The cursor sits on the `{` that opens the body; the body is parsed once per
      // instantiation, so only its position is kept.
      info.body_index = cursor.index();
      context_.declare_template(found.name, std::move(info));
      EffectSource source;
      source.tmpl = &context_.current_namespace().templates.at(found.name);
      source.ns_path = context_.current_namespace().path;
      source.names = found.param_names;
      source.body = cursor.index();
      for (const auto& param : found.params) {
        source.generic_mutable |= param.pointee && param.reference && param.mutable_reference;
        source.generic_mutable |= param.type.is_reference() && param.type.mutable_reference;
        source.params.push_back(param.type);
      }
      effect_sources_.push_back(std::move(source));
      if (auto body = skip_body(cursor); !body) {
        return std::unexpected(body.error());
      }
      return {};
    }

    std::vector<SemType> params;
    params.reserve(found.params.size());
    for (const TemplateType& param : found.params) {
      params.push_back(param.resolve());
    }

    // A new parameter list under the same name joins the overload set; the same
    // parameters twice are the same declaration written twice.
    if (context_.find_function_in_current(found.name, params) != nullptr) {
      return std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location});
    }
    if (found.name == "main") {
      if (!context_.current_namespace().path.empty() || !params.empty() || found.return_type.type != SemType::I32) {
        return std::unexpected(ParseError {"the entry point must be 'fn main(): i32'", found.location});
      }
      has_entrypoint_ = true;
    }
    const auto id = context_.declare_function(found.name, FunctionInfo {params, found.return_type.type, 0});
    effect_sources_.push_back(
        {id, nullptr, context_.current_namespace().path, found.param_names, params, cursor.index()});
    if (auto body = skip_body(cursor); !body) {
      return std::unexpected(body.error());
    }
    return {};
  }

  ProgramParser::step ProgramParser::skip_body(TokenCursor& cursor) {
    const SourceLocation start = cursor.location();
    if (auto open = expect(cursor, TokenType::LBrace); !open) {
      return std::unexpected(open.error());
    }
    // The opening brace is already consumed, so the body is one level deep.
    int depth = 1;
    while (!cursor.at_end()) {
      if (cursor.at(TokenType::LBrace)) {
        ++depth;
      } else if (cursor.at(TokenType::RBrace)) {
        --depth;
        if (depth == 0) {
          cursor.take();
          return {};
        }
      }
      cursor.take();
    }
    return std::unexpected(ParseError {"unterminated function body", start});
  }

  ProgramParser::step ProgramParser::global() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    const bool mutable_object = cursor.at_keyword(Keyword::Mut);
    if (mutable_object) {
      cursor.take();
    }
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    if (auto colon = expect(cursor, TokenType::Colon); !colon) {
      return std::unexpected(colon.error());
    }
    auto type = context_.read_type(cursor);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (type_has_references(*type)) {
      return std::unexpected(ParseError {"global variables cannot contain safe references", cursor.location()});
    }
    if (auto equals = expect_op(cursor, "="); !equals) {
      return std::unexpected(equals.error());
    }
    const std::size_t id = context_.find_global_in_current(*name)->id;

    if (type->is_record()) {
      context_.reset_frame({});
      context_.push_scope();
      context_.active_record = nullptr;
      context_.in_factory = false;
      auto value = expressions_.expression(*type);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (value->type != *type) {
        return std::unexpected(ParseError {"global initializer has the wrong type", cursor.location()});
      }
      auto copied = context_.copy_value(*value, cursor.location());
      if (!copied) {
        return std::unexpected(copied.error());
      }
      if (auto checked = context_.check_return(*copied, cursor.location()); !checked) {
        return checked;
      }
      if (auto semicolon = expect_semicolon(cursor); !semicolon) {
        return std::unexpected(semicolon.error());
      }
      context_.builder.emit(Op::Mov, {Operand::global(lower(*type), id), copied->as_operand()});
      context_.transfer_value(*copied);
      context_.emit_cleanup(0);
      context_.builder.emit(Op::Ret, {});
      context_.pop_scope();
      if (auto checked = context_.check_local_flow(); !checked) {
        return checked;
      }
      context_.builder.prune();
      program_.globals[id] = Global {lower(*type), context_.builder.take()};
      owned_globals_.emplace_back(id, type->record);
      return {};
    }
    // Scalar globals keep their constant-expression contract.
    auto value = expressions_.const_expression(*type);
    if (!value) {
      return std::unexpected(value.error());
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    context_.global_of(*name).value = value->constant;

    const Type lowered = lower(*type);
    program_.globals[id] = Global {lowered, middleend::Cfg {}};
    CfgBuilder init;
    init.emit(Op::Mov, {Operand::global(lowered, id), value->as_operand()});
    init.emit(Op::Ret, {});
    program_.globals[id].init = init.take();
    return {};
  }

  ProgramParser::step ProgramParser::function() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    auto parsed = signature(cursor);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    const Signature& found = *parsed;

    // A template declares no function of its own here: its body is parsed once per
    // deduced instantiation, when the item pass is done (`instantiate_function`).
    if (!found.type_params.empty()) {
      return skip_body(cursor);
    }

    std::vector<SemType> param_types;
    std::vector<Type> params;
    param_types.reserve(found.params.size());
    params.reserve(found.params.size());
    for (const TemplateType& param : found.params) {
      param_types.push_back(param.resolve());
      params.push_back(lower(param.resolve()));
    }
    const FunctionInfo* recorded = context_.find_function_in_current(found.name, param_types);

    current_function_ = qualified_name(context_.current_namespace(), found.name);
    current_return_type_ = found.return_type.type;
    context_.reset_frame(params);
    context_.push_scope();
    for (std::size_t index = 0; index < param_types.size(); ++index) {
      LocalInfo parameter {params[index], param_types[index], index, true, found.location};
      parameter.parameter = true;
      if (auto declared = context_.declare_local(found.param_names[index], parameter); !declared) {
        return std::unexpected(declared.error());
      }
    }

    auto body = block();
    if (!body) {
      return std::unexpected(body.error());
    }
    if (body->falls_through) {
      if (found.return_type.type != SemType::Void) {
        return std::unexpected(ParseError {
            "control reaches the end of '" + found.name + "', which returns " + type_name(found.return_type.type),
            found.location});
      }
      if (!context_.builder.terminated()) {
        context_.emit_cleanup(0);
        context_.builder.emit(Op::Ret, {});
      }
    }
    context_.pop_scope();
    // Whatever the body could not reach — the code after a `return`, the arm of an
    // `if` whose test folds the other way — is dropped here, so every block of the
    // CFG that ships is reachable.
    if (auto checked = context_.check_local_flow(); !checked) {
      return std::unexpected(checked.error());
    }
    context_.builder.prune();

    middleend::Function lowered;
    lowered.params = params;
    lowered.return_type = lower(found.return_type.type);
    lowered.frame_size = context_.builder.frame_size();
    lowered.cfg = context_.builder.take();
    if (recorded == nullptr) {
      return std::unexpected(ParseError {"internal error: missing function declaration", found.location});
    }
    const std::size_t id = recorded->id;
    if (program_.functions.size() <= id) {
      program_.functions.resize(id + 1);
    }
    program_.functions[id] = std::move(lowered);
    if (found.name == "main") {
      entrypoint_ = id;
    }
    return {};
  }

  ProgramParser::step ProgramParser::instantiate_function(const Instantiation& inst) {
    const TemplateInfo* tmpl = inst.tmpl;
    if (tmpl == nullptr) {
      return std::unexpected(ParseError {"internal error: no template to instantiate", SourceLocation {}});
    }

    // Substitute the deduced type arguments into the signature.
    std::unordered_map<std::string, SemType> substitution;
    std::vector<SemType> param_types;
    std::vector<Type> params;
    param_types.reserve(tmpl->params.size());
    params.reserve(tmpl->params.size());
    for (const TemplateType& param : tmpl->params) {
      auto resolved = context_.resolve_type(param, inst.type_args, tmpl->location);
      if (!resolved) {
        return std::unexpected(resolved.error());
      }
      param_types.push_back(*resolved);
      params.push_back(lower(param_types.back()));
    }
    auto resolved_return = context_.resolve_type(tmpl->return_type, inst.type_args, tmpl->location);
    if (!resolved_return) {
      return std::unexpected(resolved_return.error());
    }
    const SemType return_type = *resolved_return;
    for (std::size_t index = 0; index < tmpl->type_params.size(); ++index) {
      substitution.emplace(tmpl->type_params[index].name, inst.type_args[index]);
    }

    // Reposition the cursor at the template's `{` so its body parses as an ordinary
    // function body, with the type parameters resolved through the substitution, and
    // make the template's own namespace current so its body's names resolve there.
    const std::size_t saved = context_.cursor.index();
    const std::vector<std::string> saved_namespace = context_.set_namespace_path(tmpl->ns_path);
    context_.cursor.seek(tmpl->body_index);
    context_.set_type_substitution(std::move(substitution));
    const std::string qualified = qualified_name(context_.current_namespace(), tmpl->name);

    current_function_ = qualified;
    current_return_type_ = return_type;
    context_.reset_frame(params);
    context_.push_scope();
    for (std::size_t index = 0; index < tmpl->param_names.size(); ++index) {
      LocalInfo parameter {params[index], param_types[index], index, true, tmpl->location};
      parameter.parameter = true;
      if (auto declared = context_.declare_local(tmpl->param_names[index], parameter); !declared) {
        context_.clear_type_substitution();
        context_.cursor.seek(saved);
        context_.set_namespace_path(saved_namespace);
        return std::unexpected(declared.error());
      }
    }

    auto body = block();
    context_.clear_type_substitution();
    context_.cursor.seek(saved);
    context_.set_namespace_path(saved_namespace);
    if (!body) {
      return std::unexpected(body.error());
    }
    if (body->falls_through) {
      if (return_type != SemType::Void) {
        return std::unexpected(ParseError {
            "control reaches the end of '" + qualified + "', which returns " + type_name(return_type), tmpl->location});
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

    middleend::Function lowered;
    lowered.params = params;
    lowered.return_type = lower(return_type);
    lowered.frame_size = context_.builder.frame_size();
    lowered.cfg = context_.builder.take();
    if (program_.functions.size() <= inst.id) {
      program_.functions.resize(inst.id + 1);
    }
    program_.functions[inst.id] = std::move(lowered);
    return {};
  }

  ProgramParser::step ProgramParser::let() {
    TokenCursor& cursor = context_.cursor;
    const bool reassignable = cursor.take().text == "let";
    const bool mutable_object = cursor.at_keyword(Keyword::Mut);
    if (mutable_object) {
      cursor.take();
    }
    const SourceLocation start = cursor.location();
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    if (context_.declares(*name)) {
      return std::unexpected(ParseError {"'" + *name + "' is already declared in this scope", start});
    }
    std::optional<SemType> type;
    if (cursor.match(TokenType::Colon)) {
      auto parsed = context_.read_type(cursor);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      type = *parsed;
    }
    std::optional<Value> value;
    if (cursor.match_op("=")) {
      auto parsed = expressions_.expression(type);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      if (type && parsed->type != *type) {
        return std::unexpected(
            ParseError {"expected a value of type " + type_name(*type) + ", not " + type_name(parsed->type), start});
      }
      auto copied = context_.copy_value(*parsed, start);
      if (!copied) {
        return std::unexpected(copied.error());
      }
      value = *copied;
      if (!type) {
        type = parsed->type;
      }
    }
    if (!type) {
      return std::unexpected(ParseError {"expected ':' or '=' after the variable name", start});
    }
    if (*type == SemType::Void) {
      return std::unexpected(ParseError {"a variable cannot have type void", start});
    }
    if (!reassignable && !value) {
      return std::unexpected(ParseError {"a const declaration needs an initializer", start});
    }
    const Type lowered = lower(*type);
    const std::uint64_t cell = context_.builder.alloc(lowered);
    LocalInfo info {lowered, *type, cell, value.has_value(), start};
    info.reassignable = reassignable;
    info.mutable_object = mutable_object;
    if (auto declared = context_.declare_local(*name, info); !declared) {
      return std::unexpected(declared.error());
    }
    if (value) {
      if (auto bound = context_.bind_origins(*context_.find_local(*name), *value, start); !bound) {
        return bound;
      }
      context_.builder.emit(Op::Mov, {Operand::vreg(lowered, cell), value->as_operand()});
      context_.transfer_value(*value);
    }
    return {};
  }

  std::expected<ProgramParser::LValue, ParseError> ProgramParser::lvalue(const std::vector<std::string>& parts,
                                                                         SourceLocation location) {
    if (parts.size() == 1) {
      const std::string& name = parts.front();
      if (LocalInfo* local = context_.find_local(name)) {
        return LValue {local, nullptr, local->sem_type, Operand::vreg(local->type, local->vreg)};
      }
      if (const GlobalInfo* global = context_.find_global(name)) {
        return LValue {nullptr, global, global->sem_type, Operand::global(global->type, global->id)};
      }
      return std::unexpected(ParseError {"undeclared identifier '" + name + "'", location});
    }
    // Qualified: the last segment names the global, the rest its namespace path. A
    // local can never be reached this way.
    const std::vector<std::string> path(parts.begin(), parts.end() - 1);
    auto ns = context_.resolve_namespace(path, location);
    if (!ns) {
      return std::unexpected(ns.error());
    }
    const std::string& name = parts.back();
    if (const GlobalInfo* global = context_.find_global_in(**ns, name)) {
      return LValue {nullptr, global, global->sem_type, Operand::global(global->type, global->id)};
    }
    return std::unexpected(ParseError {"undeclared identifier '" + qualified_path(parts) + "'", location});
  }

  ProgramParser::step ProgramParser::write(const LValue& target, const Value& value) {
    if (target.place) {
      if (auto valid = context_.validate_origins(*target.place, context_.cursor.location()); !valid) {
        return valid;
      }
    }
    Value previous {target.type, target.target};
    if (target.place) {
      previous = *target.place;
    }
    if (target.local) {
      previous.local_id = target.local->flow_id;
      previous.live_flag = target.local->live_flag;
    }
    if (target.type.is_record()) {
      if (target.indirect) {
        context_.invalidate_interior(previous);
      } else {
        context_.invalidate_value(previous);
      }
      if (auto valid = context_.validate_origins(value, context_.cursor.location()); !valid) {
        return valid;
      }
      context_.drop_value(previous);
    }
    if (target.local) {
      if (auto bound = context_.bind_origins(*target.local, value, context_.cursor.location()); !bound) {
        return bound;
      }
    }
    if (target.indirect) {
      if (target.place) {
        if (auto stored = context_.store_origins(*target.place, value, context_.cursor.location()); !stored) {
          return stored;
        }
      }
      context_.builder.emit(Op::Store, {*target.indirect, value.as_operand()});
    } else {
      context_.builder.emit(Op::Mov, {target.target, value.as_operand()});
    }
    context_.transfer_value(value);
    target.mark_assigned();
    if (target.local) {
      context_.initialize_local(*target.local);
    }
    return {};
  }

  ProgramParser::step ProgramParser::assign(const LValue& target) {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();

    if (target.indirect && !target.mutable_object) {
      return std::unexpected(ParseError {"cannot modify an object without mut access", location});
    }
    if ((target.local && !target.local->reassignable) || (target.global && !target.global->reassignable)) {
      return std::unexpected(ParseError {"cannot assign to a const binding", location});
    }
    if (cursor.match_op("=")) {
      auto value = expressions_.expression(target.type);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (at_assignment_operator(cursor)) {
        return std::unexpected(ParseError {"assignment chains are not allowed", cursor.location()});
      }
      if (value->type != target.type) {
        return std::unexpected(
            ParseError {"cannot assign " + type_name(value->type) + " to " + type_name(target.type), location});
      }
      if (value->moved_from && *value->moved_from == target.target) {
        return std::unexpected(ParseError {"cannot move a value into itself", location});
      }
      auto copied = context_.copy_value(*value, location);
      if (!copied) {
        return std::unexpected(copied.error());
      }
      return write(target, *copied);
    }

    for (const CompoundAssign& compound : compound_assigns) {
      if (!cursor.match_op(compound.text)) {
        continue;
      }
      if (target.local && !target.local->assigned && !context_.deferred_local_reads) {
        return std::unexpected(ParseError {"compound assignment needs an initialized value", location});
      }
      if (!is_integer(target.type)) {
        return std::unexpected(
            ParseError {quoted(compound.text) + " is not defined for " + type_name(target.type), location});
      }
      if (target.local) {
        context_.read_local(*target.local, location);
      }
      auto left = target.target;
      if (!target.indirect && rhs_may_call(cursor)) {
        left = Operand::vreg(target.target.type, context_.builder.alloc(target.target.type));
        context_.builder.emit(Op::Mov, {left, target.target});
      }
      auto value = expressions_.expression(target.type);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (at_assignment_operator(cursor)) {
        return std::unexpected(ParseError {"assignment chains are not allowed", cursor.location()});
      }
      if (value->type != target.type) {
        return std::unexpected(
            ParseError {"cannot assign " + type_name(value->type) + " to " + type_name(target.type), location});
      }
      if (target.place) {
        if (auto valid = context_.validate_origins(*target.place, location); !valid) {
          return valid;
        }
      }
      // A global is not an operand a command can compute into, so the read-
      // modify-write goes through a frame cell.
      if (target.is_global() || target.indirect) {
        const Type type = target.target.type;
        const std::uint64_t cell = context_.builder.alloc(type);
        context_.builder.emit(compound.op, {Operand::vreg(type, cell), left, value->as_operand()});
        if (target.indirect) {
          context_.builder.emit(Op::Store, {*target.indirect, Operand::vreg(type, cell)});
        } else {
          context_.builder.emit(Op::Mov, {target.target, Operand::vreg(type, cell)});
        }
      } else {
        context_.builder.emit(compound.op, {target.target, left, value->as_operand()});
      }
      target.mark_assigned();
      if (target.local) {
        context_.initialize_local(*target.local);
      }
      return {};
    }
    return std::unexpected(ParseError {"expected an assignment operator", location});
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::block() {
    TokenCursor& cursor = context_.cursor;
    if (auto open = expect(cursor, TokenType::LBrace); !open) {
      return std::unexpected(open.error());
    }
    context_.push_scope();
    Flow flow;
    while (!cursor.at(TokenType::RBrace)) {
      if (cursor.at_end()) {
        context_.pop_scope();
        return std::unexpected(ParseError {"expected '}'", cursor.location()});
      }
      auto parsed = statement();
      if (!parsed) {
        context_.pop_scope();
        return std::unexpected(parsed.error());
      }
      flow.falls_through = flow.falls_through && parsed->falls_through;
    }
    cursor.take();
    context_.emit_cleanup(context_.scope_depth() - 1);
    context_.pop_scope();
    return flow;
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::statement() {
    TokenCursor& cursor = context_.cursor;
    const Keyword keyword = cursor.at(TokenType::Identifier) ? keyword_of(cursor.current().text) : Keyword::None;

    // A block is a statement, so a nested one opens a scope of its own.
    if (cursor.at(TokenType::LBrace)) {
      return block();
    }

    switch (keyword) {
      case Keyword::Const:
      case Keyword::Let: {
        if (auto declared = let(); !declared) {
          return std::unexpected(declared.error());
        }
        if (auto semicolon = expect_semicolon(cursor); !semicolon) {
          return std::unexpected(semicolon.error());
        }
        context_.cleanup_temporaries();
        return Flow {};
      }
      case Keyword::If: return if_statement();
      case Keyword::While: return while_statement();
      case Keyword::Do: return do_statement();
      case Keyword::For: return for_statement();
      case Keyword::Return: return return_statement();
      case Keyword::Break:
      case Keyword::Continue: return jump(keyword);
      default: break;
    }

    // `name <assign-op>` is an assignment; anything else that starts with a name is
    // an expression.
    if (keyword == Keyword::None && cursor.at(TokenType::Identifier) && at_assignment(cursor)) {
      if (auto written = assignment(); !written) {
        return std::unexpected(written.error());
      }
      if (auto semicolon = expect_semicolon(cursor); !semicolon) {
        return std::unexpected(semicolon.error());
      }
      context_.cleanup_temporaries();
      return Flow {};
    }

    // `*pointer <assign-op> value` stores through a pointer. A bare `*p;` — a load
    // whose value is discarded — is not this, and falls through to the expression
    // statement below.
    if (cursor.at_op("*") && at_deref_assignment(cursor)) {
      if (auto written = deref_assignment(); !written) {
        return std::unexpected(written.error());
      }
      if (auto semicolon = expect_semicolon(cursor); !semicolon) {
        return std::unexpected(semicolon.error());
      }
      context_.cleanup_temporaries();
      return Flow {};
    }

    auto value = expressions_.expression(std::nullopt);
    if (!value) {
      return std::unexpected(value.error());
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    context_.cleanup_temporaries();
    // A statement that ends its block — an expression calling a terminating
    // primitive like `exit` — stops flow here, exactly like `return`
    // (`docs/primitive.md#flow`).
    if (context_.builder.terminated()) {
      return Flow {false};
    }
    return Flow {};
  }

  ProgramParser::step ProgramParser::assignment() {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();
    const auto saved_cursor = cursor;
    std::vector<std::string> parts;
    auto name = expect_identifier(cursor);
    if (!name) {
      return std::unexpected(name.error());
    }
    parts.push_back(*name);
    while (cursor.at(TokenType::Dot)) {
      cursor.take();
      auto segment = expect_identifier(cursor);
      if (!segment) {
        return std::unexpected(segment.error());
      }
      parts.push_back(*segment);
    }
    std::expected<LValue, ParseError> target;
    if (parts.size() > 1 && context_.find_local(parts.front())) {
      cursor = saved_cursor;
      auto place = expressions_.expression(std::nullopt);
      if (!place) {
        return std::unexpected(place.error());
      }
      if (!place->place_address) {
        return std::unexpected(ParseError {"assignment needs a storage place", location});
      }
      LValue member {nullptr, nullptr, place->type, place->as_operand()};
      member.indirect = place->place_address;
      member.mutable_object = place->object_mutable;
      member.place = *place;
      target = member;
    } else {
      target = lvalue(parts, location);
    }
    if (!target) {
      return std::unexpected(target.error());
    }
    if (auto written = assign(*target); !written) {
      return std::unexpected(written.error());
    }
    return {};
  }

  ProgramParser::step ProgramParser::deref_assignment() {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();
    cursor.take();  // the `*` that makes this a store rather than a load
    auto pointer = expressions_.expression(std::nullopt);
    if (!pointer) {
      return std::unexpected(pointer.error());
    }
    if (pointer->type.is_reference() && !pointer->type.mutable_reference) {
      return std::unexpected(ParseError {"cannot write through a read-only reference", location});
    }
    if (!pointer->type.is_pointer() && !pointer->type.is_reference()) {
      return std::unexpected(ParseError {
          "cannot assign through a value of type " + type_name(pointer->type) + ", which is not a pointer", location});
    }
    const SemType pointee = *pointer->type.pointee;
    const Type lowered = lower(pointee);

    const auto capture_pointer = [&] {
      const auto stable = Operand::vreg(lower(pointer->type), context_.builder.alloc(lower(pointer->type)));
      context_.builder.emit(Op::Mov, {stable, pointer->as_operand()});
      pointer->operand = stable;
      pointer->constant.reset();
    };
    if (cursor.match_op("=")) {
      if (rhs_may_call(cursor)) {
        capture_pointer();
      }
      auto value = expressions_.expression(pointee);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (at_assignment_operator(cursor)) {
        return std::unexpected(ParseError {"assignment chains are not allowed", cursor.location()});
      }
      if (value->type != pointee) {
        return std::unexpected(ParseError {
            "cannot assign " + type_name(value->type) + " through a pointer to " + type_name(pointee), location});
      }
      if (auto valid = context_.validate_origins(*pointer, location); !valid) {
        return valid;
      }
      auto copied = context_.copy_value(*value, location);
      if (!copied) {
        return std::unexpected(copied.error());
      }
      if (pointer->type.is_reference()) {
        if (auto stored = context_.store_origins(*pointer, *copied, location); !stored) {
          return stored;
        }
      }
      if (pointer->type.is_reference() && pointee.is_record()) {
        const auto old = Operand::vreg(lowered, context_.builder.alloc(lowered));
        context_.builder.emit(Op::Load, {old, pointer->as_operand()});
        context_.drop_value(Value {pointee, old});
        context_.invalidate_referent(*pointer);
        if (auto valid = context_.validate_origins(*copied, location); !valid) {
          return valid;
        }
      }
      context_.builder.emit(Op::Store, {pointer->as_operand(), copied->as_operand()});
      context_.transfer_value(*copied);
      return {};
    }

    // A compound store reads, modifies, and writes back through the same pointer,
    // which needs a cell to hold the value between the two memory operations.
    for (const CompoundAssign& compound : compound_assigns) {
      if (!cursor.match_op(compound.text)) {
        continue;
      }
      if (!is_integer(pointee)) {
        return std::unexpected(
            ParseError {quoted(compound.text) + " is not defined for " + type_name(pointee), location});
      }
      std::optional<std::uint64_t> captured;
      if (rhs_may_call(cursor)) {
        capture_pointer();
        captured = context_.builder.alloc(lowered);
        context_.builder.emit(Op::Load, {Operand::vreg(lowered, *captured), pointer->as_operand()});
      }
      auto value = expressions_.expression(pointee);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (at_assignment_operator(cursor)) {
        return std::unexpected(ParseError {"assignment chains are not allowed", cursor.location()});
      }
      if (value->type != pointee) {
        return std::unexpected(ParseError {
            "cannot assign " + type_name(value->type) + " through a pointer to " + type_name(pointee), location});
      }
      if (auto valid = context_.validate_origins(*pointer, location); !valid) {
        return valid;
      }
      const std::uint64_t cell = captured ? *captured : context_.builder.alloc(lowered);
      if (!captured) {
        context_.builder.emit(Op::Load, {Operand::vreg(lowered, cell), pointer->as_operand()});
      }
      context_.builder.emit(compound.op,
                            {Operand::vreg(lowered, cell), Operand::vreg(lowered, cell), value->as_operand()});
      context_.builder.emit(Op::Store, {pointer->as_operand(), Operand::vreg(lowered, cell)});
      return {};
    }
    return std::unexpected(ParseError {"expected an assignment operator", location});
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::for_init() {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();
    if (cursor.at_keyword(Keyword::Let) || cursor.at_keyword(Keyword::Const)) {
      if (auto declared = let(); !declared) {
        return std::unexpected(declared.error());
      }
      context_.cleanup_temporaries();
      return Flow {};
    }
    if (cursor.at(TokenType::Identifier) && at_assignment(cursor)) {
      if (auto written = assignment(); !written) {
        return std::unexpected(written.error());
      }
      context_.cleanup_temporaries();
      return Flow {};
    }
    if (cursor.at_op("*") && at_deref_assignment(cursor)) {
      if (auto written = deref_assignment(); !written) {
        return std::unexpected(written.error());
      }
      context_.cleanup_temporaries();
      return Flow {};
    }
    return std::unexpected(ParseError {"expected a declaration or an assignment in the 'for' init", location});
  }

  std::expected<Value, ParseError> ProgramParser::condition() {
    const SourceLocation location = context_.cursor.location();
    auto value = expressions_.expression(SemType::Bool);
    if (!value) {
      return std::unexpected(value.error());
    }
    if (value->type != SemType::Bool) {
      return std::unexpected(ParseError {"condition must be 'bool', not " + type_name(value->type), location});
    }
    context_.cleanup_temporaries();
    return value;
  }

  void ProgramParser::branch_on(const Value& test, std::size_t taken, std::size_t other) {
    // A test that folds is not a branch at all: the CFG goes straight where the
    // value says, and the arm that was not taken becomes unreachable code that
    // `prune` drops.
    if (test.constant) {
      context_.builder.link_next(test.constant->signed_value() != 0 ? taken : other);
      return;
    }
    context_.builder.link_branch(taken, Condition {Op::CmpNe, test.as_operand(), Operand::instant(Type::I8, 0)}, other);
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::if_statement() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    if (auto open = expect(cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    auto test = condition();
    if (!test) {
      return std::unexpected(test.error());
    }
    // Read before the `)` is consumed, so the scan starts at the block.
    const bool has_else = cursor.else_follows();
    if (auto close = expect(cursor, TokenType::RParen); !close) {
      return std::unexpected(close.error());
    }

    const bool known = test->constant.has_value();
    const bool always = known && test->constant->signed_value() != 0;

    // A name is definitely assigned after the statement only if the paths into the
    // join all agree that it is, so each arm is read off separately.
    const AssignedSnapshot before = context_.assigned_snapshot();
    const std::size_t then_block = context_.builder.new_block();
    const std::size_t join_block = context_.builder.new_block();
    // With no `else` the arm that was not taken is the join itself.
    const std::size_t else_block = has_else ? context_.builder.new_block() : join_block;
    branch_on(*test, then_block, else_block);

    // Each arm is linked to the join as soon as it is parsed, while it is still
    // the block the builder is on.
    context_.builder.set_current(then_block);
    auto then_flow = block();
    if (!then_flow) {
      return std::unexpected(then_flow.error());
    }
    if (!context_.builder.terminated()) {
      context_.builder.link_next(join_block);
    }
    const AssignedSnapshot then_assigned = context_.assigned_snapshot();

    // The other arm is parsed either way: a test that folds to `true` makes the
    // `else` unreachable, not unchecked.
    std::optional<Flow> else_flow;
    AssignedSnapshot else_assigned = before;
    if (has_else) {
      context_.set_assigned(before);
      cursor.take();
      context_.builder.set_current(else_block);
      if (cursor.at_keyword(Keyword::If)) {
        auto nested = if_statement();
        if (!nested) {
          return std::unexpected(nested.error());
        }
        else_flow = *nested;
      } else {
        auto parsed = block();
        if (!parsed) {
          return std::unexpected(parsed.error());
        }
        else_flow = *parsed;
      }
      if (!context_.builder.terminated()) {
        context_.builder.link_next(join_block);
      }
      else_assigned = context_.assigned_snapshot();
    }
    context_.builder.set_current(join_block);

    const bool else_falls = !else_flow || else_flow->falls_through;
    if (known) {
      context_.set_assigned(always ? then_assigned : else_assigned);
    } else if (!then_flow->falls_through) {
      context_.set_assigned(else_assigned);
    } else if (!else_falls) {
      context_.set_assigned(then_assigned);
    } else {
      context_.set_assigned(then_assigned);
      context_.merge_assigned(else_assigned);
    }

    // With no `else`, the arm that was not taken always falls through.
    if (known) {
      return Flow {always ? then_flow->falls_through : else_falls};
    }
    return Flow {then_flow->falls_through || else_falls};
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::while_statement() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    if (auto open = expect(cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    // The test gets a block of its own, which is where a `continue` goes, so that
    // re-entering the loop evaluates the test again and nothing else: the block the
    // loop was written in runs once.
    const std::size_t cond_block = context_.builder.new_block();
    if (!context_.builder.terminated()) {
      context_.builder.link_next(cond_block);
    }
    context_.builder.set_current(cond_block);
    auto test = condition();
    if (!test) {
      return std::unexpected(test.error());
    }
    if (auto close = expect(cursor, TokenType::RParen); !close) {
      return std::unexpected(close.error());
    }

    const bool known = test->constant.has_value();
    const bool always = known && test->constant->signed_value() != 0;
    const std::size_t body_block = context_.builder.new_block();
    const std::size_t exit_block = context_.builder.new_block();
    branch_on(*test, body_block, exit_block);

    context_.builder.set_current(body_block);
    // A `while` may run zero times, so the state after it is the state before it:
    // a `break` can only add what the body assigned, never take any of it away.
    const AssignedSnapshot before = context_.assigned_snapshot();
    context_.push_loop(LoopTargets {body_block, cond_block, exit_block});
    auto body = block();
    if (!body) {
      return std::unexpected(body.error());
    }
    const bool has_break = context_.loop().has_break;
    context_.pop_loop();
    if (known && !always) {
      context_.set_assigned(before);
    } else {
      context_.merge_assigned(before);
    }
    if (!context_.builder.terminated()) {
      context_.builder.link_next(cond_block);
    }
    context_.builder.set_current(exit_block);

    // Without a `break` the loop only ends when the test is false, so only a test
    // that folds to `true` can keep the code after it from running.
    if (known && always) {
      return Flow {has_break};
    }
    return Flow {true};
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::do_statement() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    const std::size_t body_block = context_.builder.new_block();
    context_.builder.link_next(body_block);
    const std::size_t cond_block = context_.builder.new_block();
    const std::size_t exit_block = context_.builder.new_block();

    context_.builder.set_current(body_block);
    context_.push_loop(LoopTargets {body_block, cond_block, exit_block});
    auto body = block();
    if (!body) {
      return std::unexpected(body.error());
    }
    const bool has_break = context_.loop().has_break;
    const std::vector<AssignedSnapshot> breaks = context_.loop().breaks;
    const bool body_terminated = context_.builder.terminated();
    context_.pop_loop();
    // A `do` body always runs once, so what it assigned stays assigned; only a
    // `break` that skipped the write leaves a path where it is not.
    const AssignedSnapshot after_body = context_.assigned_snapshot();
    context_.set_assigned(after_body);
    for (const AssignedSnapshot& at_break : breaks) {
      context_.merge_assigned(at_break);
    }
    if (!body_terminated) {
      context_.builder.link_next(cond_block);
    }

    context_.builder.set_current(cond_block);
    if (auto while_keyword = expect_keyword(cursor, Keyword::While); !while_keyword) {
      return std::unexpected(while_keyword.error());
    }
    if (auto open = expect(cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    auto test = condition();
    if (!test) {
      return std::unexpected(test.error());
    }
    if (auto close = expect(cursor, TokenType::RParen); !close) {
      return std::unexpected(close.error());
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    const bool known = test->constant.has_value();
    const bool always = known && test->constant->signed_value() != 0;
    branch_on(*test, body_block, exit_block);
    context_.builder.set_current(exit_block);

    // The body always runs once, so the loop only never ends if the test is a
    // folded `true` and nothing breaks out of it.
    if (known && always) {
      return Flow {has_break};
    }
    return Flow {true};
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::for_statement() {
    TokenCursor& cursor = context_.cursor;
    cursor.take();
    if (auto open = expect(cursor, TokenType::LParen); !open) {
      return std::unexpected(open.error());
    }
    // The init opens a scope of its own, so the loop variable dies with the loop.
    // The state the loop starts from is the one the init leaves: the loop may run
    // zero times, so the body and the step assign nothing that survives it.
    context_.push_scope();
    if (!cursor.at(TokenType::Semicolon)) {
      if (auto parsed = for_init(); !parsed) {
        context_.pop_scope();
        return std::unexpected(parsed.error());
      }
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      context_.pop_scope();
      return std::unexpected(semicolon.error());
    }
    const AssignedSnapshot after_init = context_.assigned_snapshot();

    const std::size_t cond_block = context_.builder.new_block();
    context_.builder.link_next(cond_block);
    context_.builder.set_current(cond_block);

    std::optional<Value> test;
    if (!cursor.at(TokenType::Semicolon)) {
      auto parsed = condition();
      if (!parsed) {
        context_.pop_scope();
        return std::unexpected(parsed.error());
      }
      test = *parsed;
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      context_.pop_scope();
      return std::unexpected(semicolon.error());
    }

    const std::size_t step_block = context_.builder.new_block();
    const std::size_t body_block = context_.builder.new_block();
    const std::size_t exit_block = context_.builder.new_block();
    const bool known = test.has_value() && test->constant.has_value();
    if (!test) {
      context_.builder.link_next(body_block);
    } else {
      branch_on(*test, body_block, exit_block);
    }

    // The step is written before the body, but it runs after it, so it is parsed
    // into the block that ends every iteration — the one the body links to, and
    // the one a `continue` jumps to. It is an assignment, never a bare
    // expression, so the loop variable can move without another name.
    const AssignedSnapshot before_step = context_.assigned_snapshot();
    context_.builder.set_current(step_block);
    if (!cursor.at(TokenType::RParen)) {
      context_.deferred_local_reads = true;
      step written = cursor.at_op("*") ? deref_assignment() : assignment();
      context_.deferred_local_reads = false;
      if (written) {
        context_.cleanup_temporaries();
      }
      if (!written) {
        context_.pop_scope();
        return std::unexpected(written.error());
      }
    }
    if (auto close = expect(cursor, TokenType::RParen); !close) {
      context_.pop_scope();
      return std::unexpected(close.error());
    }

    context_.set_assigned(before_step);
    context_.builder.set_current(body_block);
    context_.push_loop(LoopTargets {body_block, step_block, exit_block});
    auto body = block();
    if (!body) {
      return std::unexpected(body.error());
    }
    const bool has_break = context_.loop().has_break;
    const bool body_terminated = context_.builder.terminated();
    context_.pop_loop();
    if (!body_terminated) {
      context_.builder.link_next(step_block);
    }

    // Whatever the step wrote is what the next test sees.
    context_.builder.set_current(step_block);
    if (!context_.builder.terminated()) {
      context_.builder.link_next(cond_block);
    }
    context_.builder.set_current(exit_block);
    if (known && test->constant->signed_value() == 0) {
      context_.set_assigned(after_init);
    } else {
      context_.merge_assigned(after_init);
    }
    context_.emit_cleanup(context_.scope_depth() - 1);
    context_.pop_scope();

    // A test that is not there, or that does not fold, may or may not fail, so the
    // code after the loop runs; a folded `true` runs forever without a `break`,
    // and a folded `false` never runs the body at all.
    if (known) {
      return Flow {test->constant->signed_value() != 0 ? has_break : true};
    }
    return Flow {test.has_value() ? true : has_break};
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::return_statement() {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();
    cursor.take();

    if (current_return_type_ == SemType::Void) {
      if (!cursor.at(TokenType::Semicolon)) {
        return std::unexpected(
            ParseError {"'" + current_function_ + "' returns nothing, so it cannot return a value", location});
      }
      if (auto semicolon = expect_semicolon(cursor); !semicolon) {
        return std::unexpected(semicolon.error());
      }
      if (!context_.builder.terminated()) {
        context_.emit_cleanup(0);
        context_.builder.emit(Op::Ret, {});
      }
      return Flow {false};
    }

    if (cursor.at(TokenType::Semicolon)) {
      return std::unexpected(
          ParseError {"expected a value of type " + type_name(current_return_type_) + " to return", location});
    }
    auto value = expressions_.expression(current_return_type_);
    if (!value) {
      return std::unexpected(value.error());
    }
    if (context_.conditional_receiver && !value->conditional_borrow) {
      return std::unexpected(ParseError {"ref mut? result requires a conditional borrow of this", location});
    }
    if (value->type != current_return_type_) {
      return std::unexpected(ParseError {
          "expected a value of type " + type_name(current_return_type_) + ", not " + type_name(value->type), location});
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    if (!context_.builder.terminated()) {
      auto copied = context_.copy_value(*value, location);
      if (!copied) {
        return std::unexpected(copied.error());
      }
      if (auto checked = context_.check_return(*copied, location); !checked) {
        return std::unexpected(checked.error());
      }
      auto stable = copied->as_operand();
      if (context_.has_owned_values()) {
        stable = Operand::vreg(lower(copied->type), context_.builder.alloc(lower(copied->type)));
        context_.builder.emit(Op::Mov, {stable, copied->as_operand()});
      }
      context_.transfer_value(*copied);
      context_.emit_cleanup(0);
      context_.builder.emit(Op::Ret, {stable});
    }
    return Flow {false};
  }

  std::expected<ProgramParser::Flow, ParseError> ProgramParser::jump(Keyword keyword) {
    TokenCursor& cursor = context_.cursor;
    const SourceLocation location = cursor.location();
    cursor.take();
    if (!context_.inside_loop()) {
      return std::unexpected(ParseError {quoted(to_string(keyword)) + " is only allowed in a loop", location});
    }
    if (auto semicolon = expect_semicolon(cursor); !semicolon) {
      return std::unexpected(semicolon.error());
    }
    const std::size_t target = keyword == Keyword::Break ? context_.loop().break_block : context_.loop().continue_block;
    if (!context_.builder.terminated()) {
      context_.emit_cleanup(context_.loop().scope_depth);
      context_.builder.link_next(target);
    }
    if (keyword == Keyword::Break) {
      context_.loop().has_break = true;
      // What a `break` leaves behind is one of the paths out of the loop, so it
      // has to be part of what the statement after the loop may rely on.
      context_.loop().breaks.push_back(context_.assigned_snapshot_in(context_.loop().scope_depth));
    }
    return Flow {false};
  }

}  // namespace erax::frontend
