#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "middleend/jir/Jir.hpp"

/// Helpers the parser test files share: a test should read like a program, and
/// a program is either accepted whole or rejected with one message.
namespace erax::test {

using erax::frontend::ProgramParser;
using erax::frontend::Token;
using erax::frontend::Tokenizer;
using erax::middleend::BasicBlock;
using erax::middleend::Cfg;
using erax::middleend::Command;
using erax::middleend::Op;
using erax::middleend::Operand;
using erax::middleend::OperandKind;
using erax::middleend::Program;
using erax::middleend::Type;

/// Parses a whole program the way the driver does, so a test reads like a file.
inline Program parse_ok(std::string_view source) {
    auto tokens = Tokenizer{}.tokenize(source, "test.erax");
    REQUIRE(tokens.has_value());
    auto program = ProgramParser{*tokens}.parse();
    REQUIRE(program.has_value());
    return *program;
}

/// The message of the first error, which is all a program ever reports: the parse
/// is all-or-nothing, so there is no second one to look at.
inline std::string parse_error(std::string_view source) {
    auto tokens = Tokenizer{}.tokenize(source, "test.erax");
    REQUIRE(tokens.has_value());
    auto program = ProgramParser{*tokens}.parse();
    REQUIRE_FALSE(program.has_value());
    return program.error().message;
}

/// The location of the first error, for a rule about *where* something is wrong.
inline std::string parse_error_location(std::string_view source) {
    auto tokens = Tokenizer{}.tokenize(source, "test.erax");
    REQUIRE(tokens.has_value());
    auto program = ProgramParser{*tokens}.parse();
    REQUIRE_FALSE(program.has_value());
    return program.error().location.to_string();
}

/// The first message from either stage, for a source a lexer rule already rejects: a
/// lexical rule is a parser rule that runs earlier, so both are "the first error".
inline std::string any_error(std::string_view source) {
    auto tokens = Tokenizer{}.tokenize(source, "test.erax");
    if (!tokens) return tokens.error().message;
    auto program = ProgramParser{*tokens}.parse();
    REQUIRE_FALSE(program.has_value());
    return program.error().message;
}

/// The location of the first error from either stage.
inline std::string any_error_location(std::string_view source) {
    auto tokens = Tokenizer{}.tokenize(source, "test.erax");
    if (!tokens) return tokens.error().location.to_string();
    auto program = ProgramParser{*tokens}.parse();
    REQUIRE_FALSE(program.has_value());
    return program.error().location.to_string();
}

/// The commands of a function's CFG, flattened in block order: a test that asks
/// "what does this lower to" only cares about the sequence.
inline const Cfg& body_of(const Program& program, std::size_t id) {
    return program.functions[id].cfg;
}

inline std::vector<Command> commands_of(const Program& program, std::size_t id) {
    std::vector<Command> commands;
    for (const BasicBlock& block : program.functions[id].cfg.blocks) {
        for (const Command& command : block.commands) commands.push_back(command);
    }
    return commands;
}

inline std::vector<Op> ops_of(const Program& program, std::size_t id) {
    std::vector<Op> ops;
    for (const Command& command : commands_of(program, id)) ops.push_back(command.op);
    return ops;
}

/// How many times a function emits `op`.
inline std::size_t count_of(const std::vector<Op>& ops, Op op) {
    return static_cast<std::size_t>(std::count(ops.begin(), ops.end(), op));
}

inline bool has_op(const std::vector<Op>& ops, Op op) {
    return std::find(ops.begin(), ops.end(), op) != ops.end();
}

/// Every shipped block is either closed by a `Ret` or has an edge, so no code can
/// fall off the end of the frame.
inline bool every_block_ends(const Cfg& cfg) {
    for (const BasicBlock& block : cfg.blocks) {
        if (block.edge_count() > 0) continue;
        if (block.commands.empty() || block.commands.back().op != Op::Ret) return false;
    }
    return true;
}

inline bool is_ret(const Command& command) { return command.op == Op::Ret; }

/// The index of the first block that emits `op`, or `size()` if none does.
inline std::size_t first_block_with(const Program& program, std::size_t id, Op op) {
    const Cfg& cfg = program.functions[id].cfg;
    for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
        for (const Command& command : cfg.blocks[index].commands) {
            if (command.op == op) return index;
        }
    }
    return cfg.blocks.size();
}

}  // namespace erax::test
