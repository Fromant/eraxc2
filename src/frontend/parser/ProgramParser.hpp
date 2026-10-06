#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "ExpressionParser.hpp"
#include "ParseContext.hpp"
#include "SemType.hpp"
#include "Value.hpp"
#include "middleend/jir/Jir.hpp"

namespace erax::frontend {

    /// Lowers a whole `.erax` file to a `Program`: the globals, the functions, and
    /// the CFG of every body. Like the tokenizer it is all-or-nothing, and like the
    /// tokenizer it builds no tree — a source file becomes JIR and nothing else.
    ///
    /// The parse is three passes over the same token stream:
    ///  1. every `Operator` token is checked against the whitelist, so an unknown
    ///     operator is reported where it is written instead of as a missing token;
    ///  2. every item's name, type, and parameters are read, which gives every name
    ///     its id and every call its callee before any body is parsed — so
    ///     declaration order is free for functions, and a function may read a global
    ///     declared below it;
    ///  3. the items themselves, in order: globals fold their initializers to
    ///     constants, functions build their CFG block by block.
    ///
    /// Statements are the only place CFG edges are written. Every expression fits in
    /// one block, because `&&`, `||` and `^^` are eager.
    class ProgramParser {
    public:
        explicit ProgramParser(const std::vector<Token>& tokens);

        using result = std::expected<middleend::Program, ParseError>;
        using step = std::expected<void, ParseError>;

        result parse();

    private:
        /// A function's name, its parameters, and its return type — everything a
        /// call needs before the function itself is parsed.
        struct Signature {
            std::string name;
            SourceLocation location;
            std::vector<std::string> param_names;
            std::vector<SemType> params;
            SemType return_type = SemType::Void;
        };

        /// Whether control can reach the end of a statement or a block. A block's end
        /// is reachable only if it is reachable after every one of its statements,
        /// which is what makes `return` close off the rest of a body without making
        /// the code after it invalid.
        struct Flow {
            bool falls_through = true;
        };

        /// A name that can be assigned: a local's frame cell or a global's.
        struct LValue {
            LocalInfo* local = nullptr;
            const GlobalInfo* global = nullptr;
            SemType type = SemType::Void;
            middleend::Operand target;

            bool is_global() const noexcept {
                return global != nullptr;
            }
            void mark_assigned() const noexcept {
                if (local != nullptr)
                    local->assigned = true;
            }
        };

        step operator_whitelist();
        step declarations();
        step declare_global(TokenCursor& cursor);
        step declare_function(TokenCursor& cursor);
        step global();
        step function();

        static std::expected<Signature, ParseError> signature(TokenCursor& cursor);
        /// Skips a `{ … }` body, brace for brace.
        static step skip_body(TokenCursor& cursor);

        step let();
        step assign(const LValue& target);
        std::expected<LValue, ParseError> lvalue(const std::string& name, SourceLocation location);
        /// `target = value`, the one place a cell or a global is written directly.
        void write(const LValue& target, const Value& value);

        std::expected<Flow, ParseError> block();
        std::expected<Flow, ParseError> statement();
        /// A `for`'s init: a declaration or an assignment, and nothing else.
        std::expected<Flow, ParseError> for_init();
        /// `lvalue <assign-op> value`, without the `;` that ends it as a statement.
        step assignment();
        std::expected<Flow, ParseError> if_statement();
        std::expected<Flow, ParseError> while_statement();
        std::expected<Flow, ParseError> do_statement();
        std::expected<Flow, ParseError> for_statement();
        std::expected<Flow, ParseError> return_statement();
        std::expected<Flow, ParseError> jump(Keyword keyword);
        /// A `condition`: an expression that must be `bool`.
        std::expected<Value, ParseError> condition();
        /// Edges to \p taken when a `bool` test holds and to \p other when it does
        /// not; a test that folds picks its edge instead of branching.
        void branch_on(const Value& test, std::size_t taken, std::size_t other);

        ParseContext context_;
        ExpressionParser expressions_;
        middleend::Program program_;
        std::size_t entrypoint_ = 0;
        bool has_entrypoint_ = false;
        /// The function being parsed, for diagnostics.
        std::string current_function_;
        SemType current_return_type_ = SemType::Void;
    };

}  // namespace erax::frontend
