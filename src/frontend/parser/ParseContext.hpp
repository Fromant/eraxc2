#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
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
    Const,
    Mut,
    Move,
    Ref,
    Struct,
    Public,
    Private,
    Destructor,
    Namespace,
    If,
    Else,
    While,
    Do,
    For,
    Break,
    Continue,
    Return,
    Null,
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
    Extends,
  };

  Keyword keyword_of(std::string_view text) noexcept;
  std::string_view to_string(Keyword keyword) noexcept;
  /// The type a type keyword names; every other keyword is not a type.
  std::optional<SemType> type_of(Keyword keyword) noexcept;

  /// A capability: a named set of operations a type supports, the closed vocabulary
  /// a template's type parameters are bounded by (`docs/core/types.md#capabilities`).
  enum class Capability {
    Ordered,
    Negatable,
    Arithmetic,
    Clonable,
    Movable
  };

  std::string_view to_string(Capability capability) noexcept;
  /// The capability a bound names, or nothing when the name is not one.
  std::optional<Capability> capability_of(std::string_view text) noexcept;
  /// Whether a type structurally satisfies a capability — no declaration, just the
  /// operator rules already in place.
  bool satisfies(SemType type, Capability capability) noexcept;

  /// One type parameter of a template: its name and the capabilities it promises.
  struct TypeParam {
    std::string name;
    std::vector<Capability> bounds;
  };

  /// A signature type: a concrete leaf, a type parameter, or a pointer whose
  /// pointee is another signature type. Deduction and substitution share this shape.
  struct TemplateType {
    SemType type = SemType::Void;
    int param = -1;
    std::shared_ptr<const TemplateType> pointee = {};
    bool reference = false;
    bool mutable_reference = false;
    RecordInfo* record_template = nullptr;
    std::vector<TemplateType> arguments = {};

    SemType resolve(const std::vector<SemType>& args = {}) const {
      if (pointee) {
        return reference ? SemType::reference(pointee->resolve(args), mutable_reference) :
                           SemType::pointer(pointee->resolve(args));
      }
      return param >= 0 ? args[static_cast<std::size_t>(param)] : type;
    }

    bool contains(std::size_t index) const noexcept {
      if (pointee) {
        return pointee->contains(index);
      }
      for (const auto& argument : arguments) {
        if (argument.contains(index)) {
          return true;
        }
      }
      return param == static_cast<int>(index);
    }
  };

  /// A `fn name<T …>(…)` declaration (`docs/core/statements.md#templates`). Its body
  /// is not parsed where it is written: `body_index` is the `{` that opens it, and
  /// the body is parsed once per deduced instantiation.
  struct TemplateInfo {
    std::string name;
    /// The namespace path the template was declared in (`{}` for the root), so its
    /// body is re-entered in the same scope when an instantiation is parsed.
    std::vector<std::string> ns_path;
    SourceLocation location;
    std::vector<TypeParam> type_params;
    std::vector<std::string> param_names;
    std::vector<TemplateType> params;
    TemplateType return_type;
    std::size_t body_index = 0;
    bool invalidates_references = false;
  };

  /// One template instantiation a call requested, keyed by qualified name and
  /// deduced type arguments. `id` is the function's index in the program (source
  /// functions come first, instantiations after); `generated` says whether its body
  /// has been parsed. `tmpl` is the template it came from, so the body is parsed in
  /// the template's own namespace.
  struct Instantiation {
    std::size_t id = 0;
    const TemplateInfo* tmpl = nullptr;
    std::vector<SemType> type_args;
    bool generated = false;
  };

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
      return residual_ ? *residual_ : (*tokens_)[index_];
    }
    const Token& peek(std::size_t ahead = 1) const noexcept;
    /// Consumes the current token and returns it, never stepping past
    /// `EndOfFile`.
    const Token& take() noexcept;

    std::size_t index() const noexcept {
      return index_;
    }
    /// Moves the cursor to \p index: a template's body is parsed by seeking to the
    /// `{` that opens it.
    void seek(std::size_t index) noexcept {
      index_ = index;
      residual_.reset();
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
    bool take_type_close();

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
    bool moves_from_indirect_storage() const noexcept;
    std::size_t past_operand(std::size_t index) const noexcept;

    /// The index just past the `( … )` group that starts at \p index.
    std::size_t past_group(std::size_t index) const noexcept;

    SourceLocation location() const noexcept {
      return current().location;
    }

private:
    const std::vector<Token>* tokens_;
    std::size_t index_ = 0;
    std::optional<Token> residual_;
    Token last_split_;
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
    bool reassignable = true;
    bool mutable_object = false;
  };

  struct FunctionInfo {
    std::vector<SemType> params;
    SemType return_type = SemType::Void;
    std::size_t id = 0;
    bool invalidates_references = false;
  };

  struct FieldInfo {
    std::string name;
    SemType type = SemType::Void;
    bool is_public = false;
  };

  struct MethodInfo {
    bool receiver_result = false;
    FunctionInfo function;
    std::vector<std::string> param_names;
    std::size_t body_index = 0;
    bool factory = false;
    bool is_public = false;
    SourceLocation location;
    bool generated = false;
    bool invalidates_references = false;
  };

  struct RecordInfo {
    std::string name;
    std::vector<std::string> ns_path;
    SourceLocation location;
    std::vector<FieldInfo> fields;
    std::unordered_map<std::string, std::vector<MethodInfo>> methods;
    std::optional<MethodInfo> destructor;
    std::optional<std::size_t> drop_id;
    std::optional<std::size_t> clone_id;
    std::vector<TypeParam> type_params;
    std::size_t body_index = 0;
    std::vector<SemType> type_args;
    RecordInfo* template_origin = nullptr;
    bool auto_clone = true;
    bool complete = false;
    bool members_complete = false;
  };

  /// One local: the frame cell it lives in, whether it is definitely assigned on
  /// every path reaching the current point, and where it was declared.
  struct LocalInfo {
    middleend::Type type = middleend::Type::Void;
    SemType sem_type = SemType::Void;
    std::uint64_t vreg = 0;
    bool assigned = true;
    SourceLocation location;
    bool reassignable = true;
    bool mutable_object = false;
    bool moved = false;
    std::size_t flow_id = 0;
    std::vector<ReferenceOrigin> origins = {};
    std::optional<middleend::Operand> live_flag = std::nullopt;
    bool parameter = false;
  };

  /// A lexical scope. A `for` init opens one of its own, so the loop variable dies
  /// with the loop even though CFG blocks do not nest the same way.
  struct Scope {
    std::unordered_map<std::string, LocalInfo> locals;
    std::vector<std::string> declaration_order;
    /// The first frame cell this scope may hand out, handed back when it closes.
    std::uint64_t vreg_high_water = 0;
  };

  /// A namespace: a named scope for top-level declarations, nestable
  /// (`docs/core/language.md#namespaces`). Names are resolved inside-out; a
  /// namespace's own members shadow an enclosing namespace's. The root namespace
  /// has an empty `path` and holds the program's top-level names.
  struct NameSpace {
    std::vector<std::string> path;
    std::unordered_map<std::string, GlobalInfo> globals;
    std::unordered_map<std::string, std::vector<FunctionInfo>> functions;
    std::unordered_map<std::string, TemplateInfo> templates;
    std::unordered_map<std::string, std::unique_ptr<NameSpace>> namespaces;
  };

  /// Which of the visible locals are assigned on entry to a loop: after the loop a
  /// local is assigned only if it was assigned both before and at the end of the
  /// body. The entries point into scopes that outlive the loop.
  struct AssignedSnapshot {
    struct Entry {
      LocalInfo* local;
      bool assigned;
      bool moved;
      std::vector<ReferenceOrigin> origins;
      std::size_t generation = 0;
      std::size_t interior_generation = 0;
    };
    std::vector<Entry> flags;
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
    LocalInfo* local_at(std::uint64_t vreg) noexcept;
    FunctionInfo* function_by_id(std::size_t id) noexcept;
    LocalInfo* local_by_id(std::size_t id) noexcept;
    std::expected<Value, ParseError> local_value(const LocalInfo& local, SourceLocation location);
    std::expected<RecordInfo*, ParseError> declare_record(const std::string& qualified, SourceLocation location);
    RecordInfo* find_record(const std::string& name) const noexcept;
    std::vector<std::unique_ptr<RecordInfo>>& records() noexcept {
      return records_;
    }
    const std::vector<std::unique_ptr<RecordInfo>>& records() const noexcept {
      return records_;
    }
    RecordInfo* active_record = nullptr;
    bool in_factory = false;
    std::optional<std::size_t> conditional_receiver;
    std::function<std::expected<RecordInfo*, ParseError>(RecordInfo*, std::vector<SemType>, SourceLocation)>
        record_instantiator;
    std::function<std::expected<std::size_t, ParseError>(RecordInfo*, SourceLocation)> clone_instantiator;
    std::expected<std::size_t, ParseError> ensure_clone(RecordInfo* record, SourceLocation location);
    /// The global \p name, searching the current namespace outward to the root.
    const GlobalInfo* find_global(const std::string& name) const noexcept;
    /// The same global, writable: the declaration pass leaves the value unset and
    /// the item pass fills it in once the initializer has folded. A global is only
    /// ever declared in the current namespace.
    GlobalInfo& global_of(const std::string& name) noexcept;
    /// Every declaration of \p name — one entry, or several forming an overload
    /// set — or null when the name has none, searching the current namespace
    /// outward to the root.
    const std::vector<FunctionInfo>* find_functions(const std::string& name) const noexcept;
    /// The template \p name carries, or null when it carries none (a name carries
    /// at most one, `docs/core/statements.md#templates`), searching outward.
    const TemplateInfo* find_template(const std::string& name) const noexcept;
    /// The declaration of \p name whose parameters are exactly \p params: how a
    /// duplicate signature is detected and how the two passes agree on an id.
    const FunctionInfo* find_function(const std::string& name, const std::vector<SemType>& params) const noexcept;
    /// Whether the innermost lexical scope already has this name, which is what
    /// makes a second `let` of it an error rather than a shadow.
    bool declares(const std::string& name) const noexcept;

    /// The namespace lookups as the declaration pass needs them: the *current*
    /// namespace only, so a name in an enclosing namespace is not a clash.
    bool is_declared_in_current(const std::string& name) const noexcept;
    NameSpace* find_namespace_in_current(const std::string& name) noexcept;
    const NameSpace* find_namespace_in_current(const std::string& name) const noexcept;
    const std::vector<FunctionInfo>* find_functions_in_current(const std::string& name) const noexcept;
    const TemplateInfo* find_template_in_current(const std::string& name) const noexcept;
    const GlobalInfo* find_global_in_current(const std::string& name) const noexcept;
    const FunctionInfo* find_function_in_current(const std::string& name,
                                                 const std::vector<SemType>& params) const noexcept;

    /// The namespace-directed lookups a qualified name (`a.b.f`) uses: exactly the
    /// namespace named, never searching its enclosing namespaces.
    const GlobalInfo* find_global_in(const NameSpace& ns, const std::string& name) const noexcept;
    const std::vector<FunctionInfo>* find_functions_in(const NameSpace& ns, const std::string& name) const noexcept;
    const TemplateInfo* find_template_in(const NameSpace& ns, const std::string& name) const noexcept;
    const FunctionInfo* find_function_in(const NameSpace& ns, const std::string& name,
                                         const std::vector<SemType>& params) const noexcept;
    /// A nested namespace of \p ns, or null when it has no such member.
    const NameSpace* find_namespace(const NameSpace& ns, const std::string& name) const noexcept;
    /// The namespace \p name reaches, searching the current namespace outward.
    const NameSpace* find_namespace(const std::string& name) const noexcept;

    /// The innermost namespace of the declaration walk.
    NameSpace& current_namespace() noexcept {
      return *namespace_stack_.back();
    }
    const NameSpace& current_namespace() const noexcept {
      return *namespace_stack_.back();
    }
    void push_namespace(NameSpace& ns) noexcept {
      namespace_stack_.push_back(&ns);
    }
    void pop_namespace() noexcept {
      namespace_stack_.pop_back();
    }
    /// Resolves \p path (one identifier per segment) from the current namespace:
    /// the first segment searches outward, each later one is a direct member of the
    /// namespace before it. Null with a message when a segment names no namespace.
    std::expected<NameSpace*, ParseError> resolve_namespace(const std::vector<std::string>& path,
                                                            SourceLocation location) const;
    /// Sets the active namespace to \p path from the root (for re-parsing a
    /// template's body), returning the previous path so it can be restored.
    std::vector<std::string> set_namespace_path(const std::vector<std::string>& path);

    /// Adds a nested namespace to the current namespace, failing if the name is
    /// already taken there by a global, function, template, or namespace.
    std::expected<NameSpace*, ParseError> declare_namespace(const std::string& name, SourceLocation location);

    /// Adds a global, numbering it in declaration order.
    std::size_t declare_global(const std::string& name, GlobalInfo info);
    /// Adds a function, numbering it in declaration order.
    std::size_t declare_function(const std::string& name, FunctionInfo info);
    /// Records the one template \p name may carry in the current namespace.
    void declare_template(const std::string& name, TemplateInfo info);
    /// Declares a local in the innermost scope, failing if the name is taken.
    std::expected<void, ParseError> declare_local(const std::string& name, LocalInfo info);

    /// The id of the instantiation of \p tmpl for \p type_args, registering it if
    /// this is its first call. The id is the program function it will become: source
    /// functions keep the ids the declaration pass gave them, and instantiations are
    /// numbered after them in the order they are first requested.
    std::expected<std::size_t, ParseError> instantiate(const TemplateInfo* tmpl, const std::vector<SemType>& type_args,
                                                       SourceLocation location);
    /// Every instantiation requested so far; a body may be parsed later, in the
    /// order the vector lists.
    std::vector<Instantiation>& instantiations() noexcept {
      return instantiations_;
    }
    const std::vector<Instantiation>& instantiations() const noexcept {
      return instantiations_;
    }

    /// Sets the type-parameter substitution active while an instantiation's body is
    /// parsed: a `let` or a cast naming a type parameter reads the deduced type.
    void set_type_substitution(std::unordered_map<std::string, SemType> substitution);
    void clear_type_substitution() noexcept {
      type_substitution_.clear();
    }
    /// The type at the cursor, resolving a type-parameter name through the active
    /// substitution before falling back to a concrete type name.
    std::expected<SemType, ParseError> read_type(TokenCursor& cursor, std::string_view context = {}) const;
    std::expected<std::vector<SemType>, ParseError> read_type_arguments(TokenCursor& cursor) const;
    std::expected<SemType, ParseError> resolve_type(const TemplateType& type, const std::vector<SemType>& args,
                                                    SourceLocation location) const;
    const std::unordered_map<std::string, SemType>& type_substitution() const noexcept {
      return type_substitution_;
    }

    /// A chain of instantiations demanding new types forever is a compile-time
    /// error (`docs/core/statements.md#templates`); this bounds the set.
    static constexpr std::size_t max_instantiations = 1024;

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
    /// A for-step is parsed before its predecessors; the CFG check owns its reads.
    bool deferred_local_reads = false;

    /// Clears the per-function state so the next body starts from an empty
    /// frame: a fresh `CfgBuilder`, no scopes, no loops.
    void reset_frame(std::vector<middleend::Type> params);
    /// Source-level definite initialization, including back edges after moves.
    void read_local(const LocalInfo& local, SourceLocation location);
    void initialize_local(const LocalInfo& local);
    void move_local(LocalInfo& local);
    std::expected<void, ParseError> check_local_flow() const;
    std::expected<Value, ParseError> borrow_value(const Value& value, bool mutable_reference, SourceLocation location,
                                                  bool ephemeral = false);
    std::expected<Value, ParseError> copy_value(const Value& value, SourceLocation location, bool materialize = false);
    void drop_value(const Value& value);
    void invalidate_value(const Value& value);
    void invalidate_referent(const Value& reference);
    void invalidate_interior(const Value& value);
    std::vector<ReferenceOrigin> interior_origins(const Value& value);
    void track_temporary(Value& value);
    void transfer_value(const Value& value);
    void cleanup_temporaries();
    void emit_cleanup(std::size_t retained_scopes = 0);
    bool has_owned_values() const;
    std::expected<void, ParseError> validate_origins(const Value& value, SourceLocation location);
    std::expected<void, ParseError> check_return(const Value& value, SourceLocation location);
    std::expected<void, ParseError> bind_origins(LocalInfo& local, const Value& value, SourceLocation location);
    std::expected<void, ParseError> store_origins(const Value& place, const Value& value, SourceLocation location);
    std::expected<void, ParseError> propagate_call_origins(const std::vector<Value>& arguments,
                                                           SourceLocation location);

private:
    /// The root namespace holds the program's top-level names; `namespace_stack_`
    /// walks from it into the declaration currently being parsed.
    NameSpace root_namespace_;
    std::vector<NameSpace*> namespace_stack_ {&root_namespace_};
    std::vector<Instantiation> instantiations_;
    /// The instantiation key (qualified name plus its type arguments) to its index
    /// in `instantiations_`, so a second call with the same types reuses the first.
    std::unordered_map<std::string, std::size_t> instantiation_ids_;
    /// The active type-parameter substitution, empty outside an instantiation body.
    std::unordered_map<std::string, SemType> type_substitution_;
    std::vector<Scope> scopes_;
    std::vector<LoopTargets> loops_;
    struct LocalEvent {
      std::size_t block;
      std::size_t local;
      enum class Kind {
        Read,
        Initialize,
        Move
      } kind;
      SourceLocation location;
    };
    std::vector<LocalEvent> local_events_;
    std::vector<std::string> local_names_;
    struct LoanInfo {
      std::size_t root;
      bool interior;
      bool external;
    };
    struct LoanEvent {
      enum class Kind {
        Borrow,
        Read,
        Invalidate,
        InvalidateInterior
      } kind;
      std::size_t block;
      std::size_t subject;
      SourceLocation location;
      bool external = false;
    };
    std::vector<LoanInfo> loans_;
    std::vector<LoanEvent> loan_events_;
    ReferenceOrigin make_origin(std::size_t root, bool external, bool interior);
    void record_invalidation(std::size_t root, bool interior_only, bool external = false);
    std::vector<std::unique_ptr<RecordInfo>> records_;
    std::unordered_map<std::string, RecordInfo*> record_names_;
    std::vector<Value> temporaries_;
    std::vector<std::size_t> origin_generations_;
    std::vector<std::size_t> interior_generations_;
    std::vector<bool> origin_alive_;
    std::optional<ParseError> cleanup_error_;
  };

}  // namespace erax::frontend
