#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "SemType.hpp"
#include "Value.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "middleend/jir/Builder.hpp"
#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

  /// The reserved words of `docs/core/language.md#reserved-words`. They lex as
  /// plain identifiers and are classified here, in the parser.
  enum class Keyword {
    None,
    Fn,
    Let,
    If,
    Else,
    While,
    Do,
    For,
    Break,
    Continue,
    Return,
    True,
    False,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    Bool,
    Char,
    Primitive,
    As,
  };

  Keyword keyword_of(std::string_view text) noexcept;
  std::string_view to_string(Keyword keyword) noexcept;
  /// The type a type keyword names; every other keyword is not a type.
  std::optional<SemType> type_of(Keyword keyword) noexcept;

  /// Every error carries a message and a position, the shape `LexError` has, and
  /// the parser returns them the same all-or-nothing way the tokenizer does.
  struct ParseError {
    std::string message;
    SourceLocation location;
  };

  /// A cursor over the token stream. It never reads past the trailing
  /// `EndOfFile`, so every step makes progress even at the end of a file.
  class TokenCursor {
public:
    explicit TokenCursor(const std::vector<Token>& tokens) noexcept : tokens_(&tokens) {}

    const std::vector<Token>& tokens() const noexcept {
      return *tokens_;
    }

    const Token& current() const noexcept {
      return (*tokens_)[index_];
    }
    const Token& peek(std::size_t ahead = 1) const noexcept;
    /// Consumes the current token and returns it, never stepping past
    /// `EndOfFile`.
    const Token& take() noexcept;

    std::size_t index() const noexcept {
      return index_;
    }
    bool at_end() const noexcept {
      return current().type == TokenType::EndOfFile;
    }
    bool at(TokenType type) const noexcept {
      return current().type == type;
    }
    bool at_op(std::string_view text) const noexcept {
      return current().type == TokenType::Operator && current().text == text;
    }
    bool at_keyword(Keyword keyword) const noexcept {
      return current().type == TokenType::Identifier && keyword_of(current().text) == keyword;
    }

    /// Consumes the token if it is there; reports nothing if it is not.
    bool match(TokenType type);
    bool match_op(std::string_view text);

    /// Skips to just past the next `;` at this nesting level, or past the `}`
    /// that closes it, so a recovery scan lands on the next statement or item.
    void skip_to_sync();

    /// Whether the `else` keyword follows the `{ … }` group that starts at the
    /// next token. An `if` has to know before it branches which edge an untaken
    /// test falls through to — the other arm, or the join when there is none.
    bool else_follows() const;

    /// Whether one of `operators` follows the operand that starts at the cursor,
    /// counting a `( … )` group or a call's arguments as part of that operand. An
    /// expression level needs this to know a literal is the whole expression
    /// before it types it, which is the only way a literal can take the type of
    /// its position without typing the operands of an operator too.
    bool operator_follows(std::span<const std::string_view> operators) const;

    /// Whether the keyword follows the operand that starts at the cursor, over the
    /// same extent `operator_follows` steps. A cast's operand takes no type from
    /// its position — the target says everything — so the level has to know an
    /// `as` comes before it parses the operand (`docs/core/types.md#casts`).
    bool keyword_follows(Keyword keyword) const;

    /// The index just past the operand starting at \p index: prefix operators,
    /// then a `( … )` group, a name, or a call's arguments after that name.
    std::size_t past_operand(std::size_t index) const noexcept;

    /// The index just past the `( … )` group that starts at \p index.
    std::size_t past_group(std::size_t index) const noexcept;

    SourceLocation location() const noexcept {
      return current().location;
    }

private:
    const std::vector<Token>* tokens_;
    std::size_t index_ = 0;
  };

  // The parse steps, each of which either consumes what it expects and moves on or
  // reports what was there instead. They take the cursor rather than the context,
  // so the signature pre-pass can read the same steps over its own cursor.
  std::expected<std::string, ParseError> expect_identifier(TokenCursor& cursor);
  std::expected<const Token*, ParseError> expect(TokenCursor& cursor, TokenType type);
  std::expected<void, ParseError> expect_op(TokenCursor& cursor, std::string_view text);
  std::expected<void, ParseError> expect_keyword(TokenCursor& cursor, Keyword keyword);
  std::expected<void, ParseError> expect_semicolon(TokenCursor& cursor);
  /// The type at the cursor: a type keyword, or a note that `string` has no type
  /// yet. \p context names what wanted the type, for the error to read
  /// "expected a type after 'as'".
  std::expected<SemType, ParseError> expect_type(TokenCursor& cursor, std::string_view context = {});

  /// What a name refers to. Function ids come from the pre-pass, so a call to a
  /// function declared later in the file already has one.
  struct GlobalInfo {
    middleend::Type type = middleend::Type::Void;
    SemType sem_type = SemType::Void;
    std::size_t id = 0;
    /// The value its initializer folded to, which a later constant expression may
    /// read: an initializer may only name globals declared before it, so the
    /// value is always known by the time one can use it.
    std::optional<ConstInt> value;
  };

  struct FunctionInfo {
    std::vector<SemType> params;
    SemType return_type = SemType::Void;
    std::size_t id = 0;
  };

  /// One local: the frame cell it lives in, whether it is definitely assigned on
  /// every path reaching the current point, and where it was declared.
  struct LocalInfo {
    middleend::Type type = middleend::Type::Void;
    SemType sem_type = SemType::Void;
    std::uint64_t vreg = 0;
    bool assigned = true;
    SourceLocation location;
  };

  /// A lexical scope. A `for` init opens one of its own, so the loop variable dies
  /// with the loop even though CFG blocks do not nest the same way.
  struct Scope {
    std::unordered_map<std::string, LocalInfo> locals;
    /// The first frame cell this scope may hand out, handed back when it closes.
    std::uint64_t vreg_high_water = 0;
  };

  /// Which of the visible locals are assigned on entry to a loop: after the loop a
  /// local is assigned only if it was assigned both before and at the end of the
  /// body. The entries point into scopes that outlive the loop.
  struct AssignedSnapshot {
    std::vector<std::pair<LocalInfo*, bool>> flags;
  };

  /// Where `break` and `continue` go. All three blocks exist before the body is
  /// parsed, so neither needs a fixup; one that ends up with no edge to it is
  /// dropped by `prune`.
  struct LoopTargets {
    std::size_t body = 0;
    std::size_t continue_block = 0;
    std::size_t break_block = 0;
    /// How many scopes the loop was entered with, so a `break` snapshot names only
    /// the locals that outlive it.
    std::size_t scope_depth = 0;
    /// What was assigned at each `break` that leaves this loop, so the statement
    /// after it can ask what every path out of the loop assigned.
    std::vector<AssignedSnapshot> breaks;
    bool has_break = false;

    LoopTargets(std::size_t body_block, std::size_t continue_target, std::size_t break_target) noexcept :
        body(body_block), continue_block(continue_target), break_block(break_target) {}
  };

  /// Everything the expression parser and the statement parser share: the cursor,
  /// the symbol tables, and the CFG under construction. One context per file; the
  /// frame, the scopes, and the loop stack are reset per function body.
  class ParseContext {
public:
    explicit ParseContext(const std::vector<Token>& tokens) noexcept : cursor(tokens) {}

    const LocalInfo* find_local(const std::string& name) const noexcept;
    /// The same local, writable: an assignment marks it definitely assigned.
    LocalInfo* find_local(const std::string& name) noexcept;
    const GlobalInfo* find_global(const std::string& name) const noexcept;
    /// The same global, writable: the declaration pass leaves the value unset and
    /// the item pass fills it in once the initializer has folded.
    GlobalInfo& global_of(const std::string& name) noexcept;
    /// Every declaration of \p name — one entry, or several forming an overload
    /// set — or null when the name has none.
    const std::vector<FunctionInfo>* find_functions(const std::string& name) const noexcept;
    /// The declaration of \p name whose parameters are exactly \p params: how a
    /// duplicate signature is detected and how the two passes agree on an id.
    const FunctionInfo* find_function(const std::string& name, const std::vector<SemType>& params) const noexcept;
    /// Whether the innermost scope already has this name, which is what makes a
    /// second `let` of it an error rather than a shadow.
    bool declares(const std::string& name) const noexcept;

    /// Adds a global, numbering it in declaration order.
    std::size_t declare_global(const std::string& name, GlobalInfo info);
    /// Adds a function, numbering it in declaration order.
    std::size_t declare_function(const std::string& name, FunctionInfo info);
    /// Declares a local in the innermost scope, failing if the name is taken.
    std::expected<void, ParseError> declare_local(const std::string& name, LocalInfo info);

    void push_scope();
    void pop_scope();

    /// Which of the visible locals are assigned, to intersect with the state at
    /// the end of a loop body.
    AssignedSnapshot assigned_snapshot();
    /// The same, over the outermost \p scopes scopes only. A `break` takes one of
    /// these: a local declared inside the body dies with it, so keeping a pointer
    /// to its flag would dangle by the time the loop is done with the snapshot.
    AssignedSnapshot assigned_snapshot_in(std::size_t scopes);
    /// How deep the scope stack is, which is how many scopes outlive a loop.
    std::size_t scope_depth() const noexcept {
      return scopes_.size();
    }
    /// A local is assigned after a loop only if it was assigned before it and at
    /// the end of its body.
    void merge_assigned(const AssignedSnapshot& before);
    /// Sets every flag to what a snapshot recorded. What a `do` body leaves behind
    /// is its own state, not an intersection: the body always runs once, so a
    /// local the body assigns is assigned afterwards however the test goes.
    void set_assigned(const AssignedSnapshot& snapshot) noexcept;

    const std::unordered_map<std::string, GlobalInfo>& globals() const noexcept {
      return globals_;
    }
    const std::unordered_map<std::string, std::vector<FunctionInfo>>& functions() const noexcept {
      return functions_;
    }
    std::size_t function_count = 0;
    std::size_t global_count = 0;

    bool inside_loop() const noexcept {
      return !loops_.empty();
    }
    LoopTargets& loop() noexcept {
      return loops_.back();
    }
    /// A loop records the scope depth it was entered at, so the `break`s inside it
    /// can snapshot only the locals that outlive the loop.
    void push_loop(LoopTargets targets) {
      targets.scope_depth = scopes_.size();
      loops_.push_back(std::move(targets));
    }
    void pop_loop() {
      loops_.pop_back();
    }

    TokenCursor cursor;
    middleend::CfgBuilder builder;

    /// Clears the per-function state so the next body starts from an empty
    /// frame: a fresh `CfgBuilder`, no scopes, no loops.
    void reset_frame(std::vector<middleend::Type> params);

private:
    std::unordered_map<std::string, GlobalInfo> globals_;
    std::unordered_map<std::string, std::vector<FunctionInfo>> functions_;
    std::vector<Scope> scopes_;
    std::vector<LoopTargets> loops_;
  };

}  // namespace erax::frontend
