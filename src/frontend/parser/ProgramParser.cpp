#include "ProgramParser.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "middleend/jir/Builder.hpp"
#include "middleend/jir/Jir.hpp"

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

        std::string quoted(std::string_view text) {
            return "'" + std::string {text} + "'";
        }

        /// A type's name, for a message.
        std::string type_name(SemType type) {
            return std::string {to_string(type)};
        }

        /// Whether the token after the one at the cursor is an assignment operator: what
        /// tells `name = …` from a statement that merely starts with a name.
        bool at_assignment(const TokenCursor& cursor) noexcept {
            const Token& next = cursor.peek();
            if (next.type != TokenType::Operator)
                return false;
            if (next.text == "=")
                return true;
            for (const CompoundAssign& compound : compound_assigns) {
                if (next.text == compound.text)
                    return true;
            }
            return false;
        }

    }  // namespace

    ProgramParser::ProgramParser(const std::vector<Token>& tokens) : context_(tokens), expressions_(context_) {}

    ProgramParser::result ProgramParser::parse() {
        if (auto checked = operator_whitelist(); !checked) {
            return std::unexpected(checked.error());
        }
        if (auto declared = declarations(); !declared) {
            return std::unexpected(declared.error());
        }

        while (!context_.cursor.at_end()) {
            step item;
            if (context_.cursor.at_keyword(Keyword::Fn)) {
                item = function();
            } else if (context_.cursor.at_keyword(Keyword::Let)) {
                item = global();
            } else {
                return std::unexpected(
                    ParseError {"expected a global or a function declaration", context_.cursor.location()});
            }
            if (!item)
                return std::unexpected(item.error());
        }
        if (!has_entrypoint_) {
            return std::unexpected(
                ParseError {"the program must declare 'fn main(): i32'", context_.cursor.location()});
        }
        program_.entrypoint = entrypoint_;
        return std::move(program_);
    }

    ProgramParser::step ProgramParser::operator_whitelist() {
        for (const Token& token : context_.cursor.tokens()) {
            if (token.type != TokenType::Operator)
                continue;
            bool known = false;
            for (const std::string_view text : operators) {
                if (token.text == text)
                    known = true;
            }
            if (!known) {
                return std::unexpected(ParseError {"unknown operator " + quoted(token.text), token.location});
            }
        }
        return {};
    }

    std::expected<ProgramParser::Signature, ParseError> ProgramParser::signature(TokenCursor& cursor) {
        Signature signature;
        const SourceLocation start = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name)
            return std::unexpected(name.error());
        signature.name = *name;
        signature.location = start;

        if (auto open = expect(cursor, TokenType::LParen); !open) {
            return std::unexpected(open.error());
        }
        if (!cursor.at(TokenType::RParen)) {
            while (true) {
                auto param = expect_identifier(cursor);
                if (!param)
                    return std::unexpected(param.error());
                if (auto colon = expect(cursor, TokenType::Colon); !colon) {
                    return std::unexpected(colon.error());
                }
                auto type = expect_type(cursor);
                if (!type)
                    return std::unexpected(type.error());
                signature.param_names.push_back(*param);
                signature.params.push_back(*type);
                if (!cursor.match(TokenType::Comma))
                    break;
            }
        }
        if (auto close = expect(cursor, TokenType::RParen); !close) {
            return std::unexpected(close.error());
        }
        if (cursor.match(TokenType::Colon)) {
            auto type = expect_type(cursor);
            if (!type)
                return std::unexpected(type.error());
            signature.return_type = *type;
        }
        return signature;
    }

    ProgramParser::step ProgramParser::declarations() {
        TokenCursor cursor(context_.cursor.tokens());
        while (!cursor.at_end()) {
            if (cursor.at_keyword(Keyword::Let)) {
                if (auto declared = declare_global(cursor); !declared) {
                    return std::unexpected(declared.error());
                }
            } else if (cursor.at_keyword(Keyword::Fn)) {
                if (auto declared = declare_function(cursor); !declared) {
                    return std::unexpected(declared.error());
                }
            } else {
                // Not an item. The main pass is what reports anything wrong with it;
                // this pass only needs to step over it.
                cursor.take();
            }
        }
        return {};
    }

    ProgramParser::step ProgramParser::declare_global(TokenCursor& cursor) {
        cursor.take();
        const SourceLocation start = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name)
            return std::unexpected(name.error());
        if (auto colon = expect(cursor, TokenType::Colon); !colon) {
            return std::unexpected(colon.error());
        }
        auto type = expect_type(cursor);
        if (!type)
            return std::unexpected(type.error());
        if (auto equals = expect_op(cursor, "="); !equals) {
            return std::unexpected(equals.error());
        }
        if (context_.find_global(*name) || context_.find_function(*name)) {
            return std::unexpected(ParseError {"'" + *name + "' is already declared", start});
        }
        // The id is assigned here, before the value: a function declared above a
        // global may still read it, since globals are visible everywhere.
        context_.declare_global(*name, GlobalInfo {lower(*type), *type, 0, std::nullopt});
        // The initializer itself is left to the main pass, which folds it; it is a
        // constant expression, so it holds no `;` or `}` of its own.
        cursor.skip_to_sync();
        return {};
    }

    ProgramParser::step ProgramParser::declare_function(TokenCursor& cursor) {
        cursor.take();
        auto parsed = signature(cursor);
        if (!parsed)
            return std::unexpected(parsed.error());
        const Signature found = *parsed;

        if (context_.find_function(found.name) || context_.find_global(found.name)) {
            return std::unexpected(ParseError {"'" + found.name + "' is already declared", found.location});
        }
        if (found.name == "main") {
            if (!found.params.empty() || found.return_type != SemType::I32) {
                return std::unexpected(ParseError {"the entry point must be 'fn main(): i32'", found.location});
            }
            has_entrypoint_ = true;
        }
        context_.declare_function(found.name, FunctionInfo {found.params, found.return_type, 0});
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
        auto name = expect_identifier(cursor);
        if (!name)
            return std::unexpected(name.error());
        if (auto colon = expect(cursor, TokenType::Colon); !colon) {
            return std::unexpected(colon.error());
        }
        auto type = expect_type(cursor);
        if (!type)
            return std::unexpected(type.error());
        if (auto equals = expect_op(cursor, "="); !equals) {
            return std::unexpected(equals.error());
        }
        const std::size_t id = context_.find_global(*name)->id;

        // A global's initializer is a constant expression, so it folds here: the value
        // is both what the init CFG writes and what a later initializer reads.
        auto value = expressions_.const_expression(*type);
        if (!value)
            return std::unexpected(value.error());
        if (auto semicolon = expect_semicolon(cursor); !semicolon) {
            return std::unexpected(semicolon.error());
        }
        context_.global_of(*name).value = value->constant;

        const Type lowered = lower(*type);
        program_.globals.push_back(Global {lowered, middleend::Cfg {}});
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
        if (!parsed)
            return std::unexpected(parsed.error());
        const Signature& found = *parsed;
        const FunctionInfo* recorded = context_.find_function(found.name);

        std::vector<Type> params;
        params.reserve(found.params.size());
        for (const SemType param : found.params)
            params.push_back(lower(param));

        current_function_ = found.name;
        current_return_type_ = found.return_type;
        context_.reset_frame(params);
        context_.push_scope();
        for (std::size_t index = 0; index < found.params.size(); ++index) {
            if (auto declared =
                    context_.declare_local(found.param_names[index],
                                           LocalInfo {params[index], found.params[index], index, true, found.location});
                !declared) {
                return std::unexpected(declared.error());
            }
        }

        auto body = block();
        if (!body)
            return std::unexpected(body.error());
        if (body->falls_through) {
            if (found.return_type != SemType::Void) {
                return std::unexpected(ParseError {
                    "control reaches the end of '" + found.name + "', which returns " + type_name(found.return_type),
                    found.location});
            }
            if (!context_.builder.terminated())
                context_.builder.emit(Op::Ret, {});
        }
        context_.pop_scope();
        // Whatever the body could not reach — the code after a `return`, the arm of an
        // `if` whose test folds the other way — is dropped here, so every block of the
        // CFG that ships is reachable.
        context_.builder.prune();

        middleend::Function lowered;
        lowered.params = params;
        lowered.return_type = lower(found.return_type);
        lowered.frame_size = context_.builder.frame_size();
        lowered.cfg = context_.builder.take();
        const std::size_t id = program_.functions.size();
        program_.functions.push_back(std::move(lowered));
        if (found.name == "main")
            entrypoint_ = id;
        if (recorded != nullptr && recorded->id != id) {
            // The pre-pass and this pass walk the items in the same order, so the ids
            // they hand out have to agree.
            return std::unexpected(ParseError {"internal error: '" + found.name + "' changed id", found.location});
        }
        return {};
    }

    ProgramParser::step ProgramParser::let() {
        TokenCursor& cursor = context_.cursor;
        cursor.take();

        const SourceLocation start = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name)
            return std::unexpected(name.error());
        if (context_.declares(*name)) {
            return std::unexpected(ParseError {"'" + *name + "' is already declared in this scope", start});
        }
        if (auto colon = expect(cursor, TokenType::Colon); !colon) {
            return std::unexpected(colon.error());
        }
        auto type = expect_type(cursor);
        if (!type)
            return std::unexpected(type.error());

        // The name is declared after the initializer is read, so `let x: i32 = x;`
        // reads an outer `x` — or nothing, if there is none — instead of the local
        // being declared.
        std::optional<Value> value;
        if (cursor.match_op("=")) {
            auto parsed = expressions_.expression(*type);
            if (!parsed)
                return std::unexpected(parsed.error());
            if (parsed->type != *type) {
                return std::unexpected(ParseError {
                    "expected a value of type " + type_name(*type) + ", not " + type_name(parsed->type), start});
            }
            value = *parsed;
        }

        const Type lowered = lower(*type);
        const std::uint64_t cell = context_.builder.alloc(lowered);
        if (auto declared = context_.declare_local(*name, LocalInfo {lowered, *type, cell, value.has_value(), start});
            !declared) {
            return std::unexpected(declared.error());
        }
        // A `let` with no initializer defines nothing yet: the cell is only written by
        // an assignment, and a read before one is an error.
        if (value) {
            context_.builder.emit(Op::Mov, {Operand::vreg(lowered, cell), value->as_operand()});
        }
        return {};
    }

    std::expected<ProgramParser::LValue, ParseError> ProgramParser::lvalue(const std::string& name,
                                                                           SourceLocation location) {
        if (LocalInfo* local = context_.find_local(name)) {
            return LValue {local, nullptr, local->sem_type, Operand::vreg(local->type, local->vreg)};
        }
        if (const GlobalInfo* global = context_.find_global(name)) {
            return LValue {nullptr, global, global->sem_type, Operand::global(global->type, global->id)};
        }
        return std::unexpected(ParseError {"undeclared identifier '" + name + "'", location});
    }

    void ProgramParser::write(const LValue& target, const Value& value) {
        context_.builder.emit(Op::Mov, {target.target, value.as_operand()});
        target.mark_assigned();
    }

    ProgramParser::step ProgramParser::assign(const LValue& target) {
        TokenCursor& cursor = context_.cursor;
        const SourceLocation location = cursor.location();

        if (cursor.match_op("=")) {
            auto value = expressions_.expression(target.type);
            if (!value)
                return std::unexpected(value.error());
            if (value->type != target.type) {
                return std::unexpected(
                    ParseError {"cannot assign " + type_name(value->type) + " to " + type_name(target.type), location});
            }
            write(target, *value);
            return {};
        }

        for (const CompoundAssign& compound : compound_assigns) {
            if (!cursor.match_op(compound.text))
                continue;
            if (!is_integer(target.type)) {
                return std::unexpected(
                    ParseError {quoted(compound.text) + " is not defined for " + type_name(target.type), location});
            }
            auto value = expressions_.expression(target.type);
            if (!value)
                return std::unexpected(value.error());
            if (value->type != target.type) {
                return std::unexpected(
                    ParseError {"cannot assign " + type_name(value->type) + " to " + type_name(target.type), location});
            }
            // A global is not an operand a command can compute into, so the read-
            // modify-write goes through a frame cell.
            if (target.is_global()) {
                const Type type = target.target.type;
                const std::uint64_t cell = context_.builder.alloc(type);
                context_.builder.emit(compound.op, {Operand::vreg(type, cell), target.target, value->as_operand()});
                context_.builder.emit(Op::Mov, {target.target, Operand::vreg(type, cell)});
            } else {
                context_.builder.emit(compound.op, {target.target, target.target, value->as_operand()});
            }
            target.mark_assigned();
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
        context_.pop_scope();
        return flow;
    }

    std::expected<ProgramParser::Flow, ParseError> ProgramParser::statement() {
        TokenCursor& cursor = context_.cursor;
        const Keyword keyword = cursor.at(TokenType::Identifier) ? keyword_of(cursor.current().text) : Keyword::None;

        // A block is a statement, so a nested one opens a scope of its own.
        if (cursor.at(TokenType::LBrace))
            return block();

        switch (keyword) {
            case Keyword::Let: {
                if (auto declared = let(); !declared)
                    return std::unexpected(declared.error());
                if (auto semicolon = expect_semicolon(cursor); !semicolon) {
                    return std::unexpected(semicolon.error());
                }
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
            if (auto written = assignment(); !written)
                return std::unexpected(written.error());
            if (auto semicolon = expect_semicolon(cursor); !semicolon) {
                return std::unexpected(semicolon.error());
            }
            return Flow {};
        }

        auto value = expressions_.expression(std::nullopt);
        if (!value)
            return std::unexpected(value.error());
        if (auto semicolon = expect_semicolon(cursor); !semicolon) {
            return std::unexpected(semicolon.error());
        }
        return Flow {};
    }

    ProgramParser::step ProgramParser::assignment() {
        TokenCursor& cursor = context_.cursor;
        const SourceLocation location = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name)
            return std::unexpected(name.error());
        auto target = lvalue(*name, location);
        if (!target)
            return std::unexpected(target.error());
        if (auto written = assign(*target); !written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    std::expected<ProgramParser::Flow, ParseError> ProgramParser::for_init() {
        TokenCursor& cursor = context_.cursor;
        const SourceLocation location = cursor.location();
        if (cursor.at_keyword(Keyword::Let)) {
            if (auto declared = let(); !declared)
                return std::unexpected(declared.error());
            return Flow {};
        }
        if (cursor.at(TokenType::Identifier) && at_assignment(cursor)) {
            if (auto written = assignment(); !written)
                return std::unexpected(written.error());
            return Flow {};
        }
        return std::unexpected(ParseError {"expected a declaration or an assignment in the 'for' init", location});
    }

    std::expected<Value, ParseError> ProgramParser::condition() {
        const SourceLocation location = context_.cursor.location();
        auto value = expressions_.expression(SemType::Bool);
        if (!value)
            return std::unexpected(value.error());
        if (value->type != SemType::Bool) {
            return std::unexpected(ParseError {"condition must be 'bool', not " + type_name(value->type), location});
        }
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
        context_.builder.link_branch(taken, Condition {Op::CmpNe, test.as_operand(), Operand::instant(Type::I8, 0)},
                                     other);
    }

    std::expected<ProgramParser::Flow, ParseError> ProgramParser::if_statement() {
        TokenCursor& cursor = context_.cursor;
        cursor.take();
        if (auto open = expect(cursor, TokenType::LParen); !open)
            return std::unexpected(open.error());
        auto test = condition();
        if (!test)
            return std::unexpected(test.error());
        // Read before the `)` is consumed, so the scan starts at the block.
        const bool has_else = cursor.else_follows();
        if (auto close = expect(cursor, TokenType::RParen); !close)
            return std::unexpected(close.error());

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
        if (!then_flow)
            return std::unexpected(then_flow.error());
        if (!context_.builder.terminated())
            context_.builder.link_next(join_block);
        const AssignedSnapshot then_assigned = context_.assigned_snapshot();

        // The other arm is parsed either way: a test that folds to `true` makes the
        // `else` unreachable, not unchecked.
        std::optional<Flow> else_flow;
        AssignedSnapshot else_assigned = before;
        if (has_else) {
            cursor.take();
            context_.builder.set_current(else_block);
            if (cursor.at_keyword(Keyword::If)) {
                auto nested = if_statement();
                if (!nested)
                    return std::unexpected(nested.error());
                else_flow = *nested;
            } else {
                auto parsed = block();
                if (!parsed)
                    return std::unexpected(parsed.error());
                else_flow = *parsed;
            }
            if (!context_.builder.terminated())
                context_.builder.link_next(join_block);
            else_assigned = context_.assigned_snapshot();
        }
        context_.builder.set_current(join_block);

        // A folded test has one path, so the arm it takes is the state after the
        // statement; an unfolded one keeps a local assigned only if both arms did.
        if (known) {
            context_.set_assigned(always ? then_assigned : else_assigned);
        } else {
            context_.merge_assigned(then_assigned);
            context_.merge_assigned(else_assigned);
        }

        // With no `else`, the arm that was not taken always falls through.
        const bool else_falls = !else_flow || else_flow->falls_through;
        if (known)
            return Flow {always ? then_flow->falls_through : else_falls};
        return Flow {then_flow->falls_through || else_falls};
    }

    std::expected<ProgramParser::Flow, ParseError> ProgramParser::while_statement() {
        TokenCursor& cursor = context_.cursor;
        cursor.take();
        if (auto open = expect(cursor, TokenType::LParen); !open)
            return std::unexpected(open.error());
        // The test gets a block of its own, which is where a `continue` goes, so that
        // re-entering the loop evaluates the test again and nothing else: the block the
        // loop was written in runs once.
        const std::size_t cond_block = context_.builder.new_block();
        if (!context_.builder.terminated())
            context_.builder.link_next(cond_block);
        context_.builder.set_current(cond_block);
        auto test = condition();
        if (!test)
            return std::unexpected(test.error());
        if (auto close = expect(cursor, TokenType::RParen); !close)
            return std::unexpected(close.error());

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
        if (!body)
            return std::unexpected(body.error());
        const bool has_break = context_.loop().has_break;
        context_.pop_loop();
        context_.merge_assigned(before);
        if (!context_.builder.terminated())
            context_.builder.link_next(cond_block);
        context_.builder.set_current(exit_block);

        // Without a `break` the loop only ends when the test is false, so only a test
        // that folds to `true` can keep the code after it from running.
        if (known && always)
            return Flow {has_break};
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
        if (!body)
            return std::unexpected(body.error());
        const bool has_break = context_.loop().has_break;
        const std::vector<AssignedSnapshot> breaks = context_.loop().breaks;
        const bool body_terminated = context_.builder.terminated();
        context_.pop_loop();
        // A `do` body always runs once, so what it assigned stays assigned; only a
        // `break` that skipped the write leaves a path where it is not.
        const AssignedSnapshot after_body = context_.assigned_snapshot();
        context_.set_assigned(after_body);
        for (const AssignedSnapshot& at_break : breaks)
            context_.merge_assigned(at_break);
        if (!body_terminated)
            context_.builder.link_next(cond_block);

        context_.builder.set_current(cond_block);
        if (auto while_keyword = expect_keyword(cursor, Keyword::While); !while_keyword) {
            return std::unexpected(while_keyword.error());
        }
        if (auto open = expect(cursor, TokenType::LParen); !open)
            return std::unexpected(open.error());
        auto test = condition();
        if (!test)
            return std::unexpected(test.error());
        if (auto close = expect(cursor, TokenType::RParen); !close)
            return std::unexpected(close.error());
        if (auto semicolon = expect_semicolon(cursor); !semicolon) {
            return std::unexpected(semicolon.error());
        }
        const bool known = test->constant.has_value();
        const bool always = known && test->constant->signed_value() != 0;
        branch_on(*test, body_block, exit_block);
        context_.builder.set_current(exit_block);

        // The body always runs once, so the loop only never ends if the test is a
        // folded `true` and nothing breaks out of it.
        if (known && always)
            return Flow {has_break};
        return Flow {true};
    }

    std::expected<ProgramParser::Flow, ParseError> ProgramParser::for_statement() {
        TokenCursor& cursor = context_.cursor;
        cursor.take();
        if (auto open = expect(cursor, TokenType::LParen); !open)
            return std::unexpected(open.error());
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
        context_.builder.set_current(step_block);
        if (!cursor.at(TokenType::RParen)) {
            if (auto written = assignment(); !written) {
                context_.pop_scope();
                return std::unexpected(written.error());
            }
        }
        if (auto close = expect(cursor, TokenType::RParen); !close) {
            context_.pop_scope();
            return std::unexpected(close.error());
        }

        context_.builder.set_current(body_block);
        context_.push_loop(LoopTargets {body_block, step_block, exit_block});
        auto body = block();
        if (!body)
            return std::unexpected(body.error());
        const bool has_break = context_.loop().has_break;
        const bool body_terminated = context_.builder.terminated();
        context_.pop_loop();
        if (!body_terminated)
            context_.builder.link_next(step_block);

        // Whatever the step wrote is what the next test sees.
        context_.builder.set_current(step_block);
        if (!context_.builder.terminated())
            context_.builder.link_next(cond_block);
        context_.builder.set_current(exit_block);
        context_.merge_assigned(after_init);
        context_.pop_scope();

        // A test that is not there, or that does not fold, may or may not fail, so the
        // code after the loop runs; a folded `true` runs forever without a `break`,
        // and a folded `false` never runs the body at all.
        if (known)
            return Flow {test->constant->signed_value() != 0 ? has_break : true};
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
            if (!context_.builder.terminated())
                context_.builder.emit(Op::Ret, {});
            return Flow {false};
        }

        if (cursor.at(TokenType::Semicolon)) {
            return std::unexpected(
                ParseError {"expected a value of type " + type_name(current_return_type_) + " to return", location});
        }
        auto value = expressions_.expression(current_return_type_);
        if (!value)
            return std::unexpected(value.error());
        if (value->type != current_return_type_) {
            return std::unexpected(ParseError {
                "expected a value of type " + type_name(current_return_type_) + ", not " + type_name(value->type),
                location});
        }
        if (auto semicolon = expect_semicolon(cursor); !semicolon) {
            return std::unexpected(semicolon.error());
        }
        if (!context_.builder.terminated())
            context_.builder.emit(Op::Ret, {value->as_operand()});
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
        const std::size_t target =
            keyword == Keyword::Break ? context_.loop().break_block : context_.loop().continue_block;
        if (!context_.builder.terminated())
            context_.builder.link_next(target);
        if (keyword == Keyword::Break) {
            context_.loop().has_break = true;
            // What a `break` leaves behind is one of the paths out of the loop, so it
            // has to be part of what the statement after the loop may rely on.
            context_.loop().breaks.push_back(context_.assigned_snapshot_in(context_.loop().scope_depth));
        }
        return Flow {false};
    }

}  // namespace erax::frontend
