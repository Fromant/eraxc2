#include "Validator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "primitives/Primitives.hpp"

namespace erax::middleend {

  std::string location_of(const ValidationError& error) {
    switch (error.subject) {
      case Subject::Program: return "the program";
      case Subject::GlobalInit:
        return "global " + std::to_string(error.index) + "'s initializer, block " + std::to_string(error.block);
      case Subject::Function:
        return "function " + std::to_string(error.index) + ", block " + std::to_string(error.block);
    }
    return "the program";
  }

  std::string to_string(const ValidationError& error) {
    std::string text = location_of(error);
    if (error.command)
      text += ", command " + std::to_string(*error.command);
    return text + ": " + error.message;
  }

  std::expected<void, std::vector<ValidationError>> check(const Program& program) {
    std::vector<ValidationError> errors = validate(program);
    if (!errors.empty())
      return std::unexpected(std::move(errors));
    return {};
  }

  namespace {

    constexpr std::size_t no_edge = static_cast<std::size_t>(-1);

    /// Where a command is, so a helper can report it without being told again.
    struct At {
      std::size_t block;
      std::optional<std::size_t> command;
    };

    /// One CFG with everything the rules need about it. A function's body has a
    /// frame and a return type; a global's initializer has neither, so the rules
    /// that need those are functions only.
    struct Body {
      Subject subject = Subject::Program;
      std::size_t index = 0;
      const Cfg* cfg = nullptr;
      const std::vector<Type>* params = nullptr;
      Type return_type = Type::Void;
      std::size_t frame_size = 0;
    };

    std::string_view kind_text(OperandKind kind) {
      switch (kind) {
        case OperandKind::Instant: return "an instant";
        case OperandKind::VReg: return "a vreg";
        case OperandKind::Global: return "a global";
        case OperandKind::Fn: return "a function";
      }
      return "an operand";
    }

    std::string slot_text(std::size_t slot) {
      return "arg " + std::to_string(slot);
    }

    std::string arity_text(Op op) {
      const Arity spec = arity(op);
      if (spec.max == Arity::unbounded)
        return "at least " + std::to_string(spec.min) + " arguments";
      if (spec.min == spec.max)
        return std::to_string(spec.min) + " arguments";
      return std::to_string(spec.min) + " or " + std::to_string(spec.max) + " arguments";
    }

    /// The signedness an op's name commits its operands to, or nothing when the
    /// operation is defined either way. `JIR.md` says signedness "comes from the
    /// operand type", so an op that names a signedness has to agree with it.
    std::optional<bool> signedness(Op op) {
      switch (op) {
        case Op::Sdiv:
        case Op::Smod:
        case Op::Sar: return true;
        case Op::Udiv:
        case Op::Umod: return false;
        default: return std::nullopt;
      }
    }

    /// The one slot every op with a result writes; `Ret` only ever reads.
    bool is_write(Op op, std::size_t slot) {
      return has_result(op) && slot == 0;
    }

    /// A command that ends its block with no outgoing edge: `Ret`, or a
    /// terminating `PRIM` (`docs/primitive.md#flow`).
    bool closes_block(const Command& command) {
      return command.op == Op::Ret || primitives::terminates(command);
    }

    /// Every vreg the CFG names, in commands and in branch tests.
    std::uint64_t highest_vreg(const Cfg& cfg) {
      std::uint64_t highest = 0;
      auto consider = [&highest](const Operand& operand) {
        if (operand.kind == OperandKind::VReg && !operand.is_empty()) {
          highest = std::max(highest, operand.value);
        }
      };
      for (const BasicBlock& block : cfg.blocks) {
        for (const Command& command : block.commands) {
          for (const Operand& operand : command.args)
            consider(operand);
        }
        if (block.branch) {
          consider(block.branch->condition.a);
          consider(block.branch->condition.b);
        }
      }
      return highest;
    }

    /// How many cells the per-cell tables need. A function's frame is the bound; an
    /// initializer has none, so it takes whatever it uses.
    std::size_t cell_count(const Body& body) {
      return std::max<std::size_t>(body.frame_size, static_cast<std::size_t>(highest_vreg(*body.cfg)) + 1);
    }

    std::vector<bool> reachable_blocks(const Cfg& cfg) {
      std::vector<bool> reachable(cfg.blocks.size(), false);
      if (cfg.blocks.empty())
        return reachable;
      std::vector<std::size_t> stack {0};
      reachable[0] = true;
      while (!stack.empty()) {
        const BasicBlock& block = cfg.blocks[stack.back()];
        stack.pop_back();
        const std::size_t targets[2] = {block.next.value_or(no_edge), block.branch ? block.branch->target : no_edge};
        for (const std::size_t target : targets) {
          if (target == no_edge || target >= cfg.blocks.size() || reachable[target])
            continue;
          reachable[target] = true;
          stack.push_back(target);
        }
      }
      return reachable;
    }

    /// Blocks an edge arrives from. An edge naming a block that does not exist is
    /// already reported; it is left out here rather than indexing past the end.
    std::vector<std::vector<std::size_t>> predecessors_of(const Cfg& cfg) {
      std::vector<std::vector<std::size_t>> incoming(cfg.blocks.size());
      for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
        const BasicBlock& block = cfg.blocks[index];
        for (const std::size_t target : {block.next.value_or(no_edge), block.branch ? block.branch->target : no_edge}) {
          if (target == no_edge || target >= cfg.blocks.size())
            continue;
          incoming[target].push_back(index);
        }
      }
      return incoming;
    }

    /// The cells a block writes, which is what makes them defined on the way out.
    std::vector<bool> written_by(const BasicBlock& block, std::size_t cells) {
      std::vector<bool> written(cells, false);
      for (const Command& command : block.commands) {
        if (!has_result(command.op))
          continue;
        const Operand& result = command.args[0];
        if (result.kind == OperandKind::VReg && !result.is_empty() && result.value < cells) {
          written[result.value] = true;
        }
      }
      return written;
    }

    class Validator {
  public:
      explicit Validator(const Program& program) noexcept : program_(program) {}

      std::vector<ValidationError> run() {
        for (std::size_t index = 0; index < program_.globals.size(); ++index) {
          const Body body {Subject::GlobalInit, index, &program_.globals[index].init, nullptr, Type::Void, 0};
          check_cfg(body);
          check_commands(body);
          check_definitions(body);
        }
        for (std::size_t index = 0; index < program_.functions.size(); ++index) {
          const Function& function = program_.functions[index];
          const Body body {Subject::Function,  index, &function.cfg, &function.params, function.return_type,
                           function.frame_size};
          check_frame(body);
          check_cfg(body);
          check_commands(body);
          check_definitions(body);
        }
        check_entrypoint();
        return std::move(errors_);
      }

  private:
      const Program& program_;
      std::vector<ValidationError> errors_;

      void report(const Body& body, At at, std::string message) {
        errors_.push_back(ValidationError {body.subject, body.index, at.block, at.command, std::move(message)});
      }

      void report(Subject subject, std::size_t index, std::string message) {
        errors_.push_back(ValidationError {subject, index, 0, std::nullopt, std::move(message)});
      }

      // 3. Frames: the one rule every operand shares.
      void need_in_frame(const Body& body, At at, const Operand& operand, std::string_view what) {
        if (body.frame_size == 0)
          return;  // an initializer has no frame to be outside of
        if (operand.kind != OperandKind::VReg || operand.is_empty())
          return;
        if (operand.value >= body.frame_size) {
          report(body, at,
                 std::string {what} + " names $" + std::to_string(operand.value) + " in a frame of " +
                     std::to_string(body.frame_size) + " cells");
        }
      }

      // 4. Definitions: one vreg id means one type for the whole function.
      void need_one_type(const Body& body, At at, const Operand& operand, std::string_view what,
                         std::vector<std::optional<Type>>& types) {
        if (operand.value >= types.size())
          return;
        if (types[operand.value] && *types[operand.value] != operand.type) {
          report(body, at,
                 std::string {what} + " uses $" + std::to_string(operand.value) + " as " +
                     std::string {to_string(operand.type)} + ", but it is " +
                     std::string {to_string(*types[operand.value])});
          return;
        }
        types[operand.value] = operand.type;
      }

      /// An operand that holds a value: an instant, a cell, or a global. `Fn` names
      /// a function rather than holding one, and `EMPTY` names no cell at all.
      void need_value(const Body& body, At at, const Operand& operand, std::string_view role) {
        if (operand.is_empty()) {
          report(body, at, std::string {role} + " is the EMPTY sentinel, which holds no value");
        } else if (operand.kind == OperandKind::Fn) {
          report(body, at, std::string {role} + " names a function rather than holding a value");
        } else if (operand.type == Type::Void) {
          report(body, at, std::string {role} + " is void");
        }
      }

      void need_kind(const Body& body, At at, const Command& command, std::size_t slot, OperandKind kind,
                     std::string_view role) {
        const Operand& operand = command.args[slot];
        if (operand.kind != kind) {
          report(body, at,
                 std::string {role} + " must be " + std::string {kind_text(kind)} + ", but " + slot_text(slot) +
                     " is " + std::string {kind_text(operand.kind)});
        }
      }

      void need_type(const Body& body, At at, const Command& command, std::size_t slot, Type type) {
        const Operand& operand = command.args[slot];
        if (operand.type != type) {
          report(body, at,
                 std::string {to_string(command.op)} + " needs " + slot_text(slot) + " to be " +
                     std::string {to_string(type)} + ", but it is " + std::string {to_string(operand.type)});
        }
      }

      /// Two operands of one op are the same type or the program is rejected —
      /// `CAST` is the one op that never comes here, its two types being its
      /// whole point (`JIR.md#conversions`).
      void need_same_type(const Body& body, At at, const Command& command, std::size_t slot, std::size_t other) {
        const Operand& operand = command.args[slot];
        const Type expected = command.args[other].type;
        if (operand.type != expected) {
          report(body, at,
                 std::string {to_string(command.op)} + " needs one type throughout, but " + slot_text(slot) + " is " +
                     std::string {to_string(operand.type)} + " and " + slot_text(other) + " is " +
                     std::string {to_string(expected)});
        }
      }

      void need_signedness(const Body& body, At at, const Command& command) {
        const std::optional<bool> wanted = signedness(command.op);
        if (!wanted)
          return;
        for (const std::size_t slot : {std::size_t {1}, std::size_t {2}}) {
          if (is_signed(command.args[slot].type) == *wanted)
            continue;
          report(body, at,
                 std::string {to_string(command.op)} + " needs " + (*wanted ? "signed" : "unsigned") +
                     " operands, but " + slot_text(slot) + " is " + std::string {to_string(command.args[slot].type)});
        }
      }

      // 1. CFG.
      void check_cfg(const Body& body) {
        const Cfg& cfg = *body.cfg;
        if (cfg.blocks.empty()) {
          // An initializer with no CFG is a global the machine zeroes and nothing
          // writes, which `print` spells as "(no initializer)". A function is
          // never empty: without a block there is no RET, so it falls off.
          if (body.subject == Subject::Function) {
            report(body, At {0, std::nullopt}, "has no blocks, so it has no entry block");
          }
          return;
        }
        for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
          const BasicBlock& block = cfg.blocks[index];
          const At at {index, std::nullopt};
          const bool closed = !block.commands.empty() && closes_block(block.commands.back());
          if (block.edge_count() == 0 && !closed) {
            report(body, at,
                   "has no outgoing edge and does not end in RET or a terminating primitive, so a path "
                   "falls off the end");
          } else if (block.edge_count() != 0 && closed) {
            report(body, at, "ends in RET or a terminating primitive and also has an outgoing edge");
          }
          for (std::size_t slot = 0; slot < block.commands.size(); ++slot) {
            if (closes_block(block.commands[slot]) && slot + 1 != block.commands.size()) {
              report(body, At {index, slot}, "is not the last command in its block");
            }
          }
          if (block.next)
            check_edge(body, at, *block.next, "its next block is");
          if (block.branch) {
            check_edge(body, at, block.branch->target, "its branch target is");
            check_condition(body, at, block.branch->condition);
          }
        }
        const std::vector<bool> reachable = reachable_blocks(cfg);
        for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
          if (!reachable[index])
            report(body, At {index, std::nullopt}, "is not reachable from the entry block");
        }
      }

      void check_edge(const Body& body, At at, std::size_t target, std::string_view role) {
        if (target >= body.cfg->blocks.size()) {
          report(body, at, std::string {role} + " block " + std::to_string(target) + ", which does not exist");
        }
      }

      /// A branch test is what a `CMP_*` command is: one of the comparisons, on
      /// two values of one type.
      void check_condition(const Body& body, At at, const Condition& condition) {
        if (!is_compare(condition.cmp)) {
          report(body, at, "branches on " + std::string {to_string(condition.cmp)} + ", which is not a comparison");
        }
        for (const Operand& operand : {condition.a, condition.b}) {
          need_in_frame(body, at, operand, "the branch test");
          need_value(body, at, operand, "the branch test");
        }
        if (condition.a.type != condition.b.type) {
          report(body, at,
                 "branches on two types, " + std::string {to_string(condition.a.type)} + " and " +
                     std::string {to_string(condition.b.type)});
        }
      }

      // 3. Frames.
      void check_frame(const Body& body) {
        const Function& function = program_.functions[body.index];
        if (body.frame_size == 0) {
          report(body, At {0, std::nullopt}, "has a frame of no cells, and a frame is never empty");
          return;
        }
        // The params are `$0..n-1`, so the frame has to be at least that long.
        if (body.frame_size < function.params.size()) {
          report(body, At {0, std::nullopt},
                 "has a frame of " + std::to_string(body.frame_size) + " cells, which does not hold its " +
                     std::to_string(function.params.size()) + " parameters");
        }
      }

      // 2. Commands.
      void check_commands(const Body& body) {
        const Cfg& cfg = *body.cfg;
        for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
          const BasicBlock& block = cfg.blocks[index];
          for (std::size_t slot = 0; slot < block.commands.size(); ++slot) {
            check_command(body, At {index, slot}, block.commands[slot]);
          }
        }
      }

      void check_command(const Body& body, At at, const Command& command) {
        if (!arity(command.op).accepts(command.args.size())) {
          const std::size_t given = command.args.size();
          report(body, at,
                 std::string {to_string(command.op)} + " takes " + arity_text(command.op) + ", but " +
                     std::to_string(given) + (given == 1 ? " was given" : " were given"));
          return;
        }
        for (std::size_t slot = 0; slot < command.args.size(); ++slot) {
          need_in_frame(body, at, command.args[slot], to_string(command.op));
        }
        // An `EMPTY` operand names no value, so the roles and types of this
        // command's operands are not read at all -- a CALL or a PRIM result is
        // the one slot allowed to be empty, and `check_call` and `check_primitive`
        // have their own rules for it.
        if (command.op != Op::Call && command.op != Op::Primitive) {
          for (const Operand& operand : command.args) {
            if (!operand.is_empty())
              continue;
            report(body, at,
                   command.op == Op::Ret ? "the returned value is the EMPTY sentinel, which holds no value" :
                                           "an operand is the EMPTY sentinel, which holds no value");
            return;
          }
        }

        switch (command.op) {
          case Op::Mov: {
            // `MOV` is the only way to read or write a global, so its target
            // is the one slot that may be one.
            const Operand& target = command.args[0];
            if (target.kind != OperandKind::VReg && target.kind != OperandKind::Global) {
              report(body, at,
                     "MOV needs a vreg or a global to write, but " + slot_text(0) + " is " +
                         std::string {kind_text(target.kind)});
            }
            need_value(body, at, command.args[1], "MOV's source");
            need_same_type(body, at, command, 1, 0);
            break;
          }
          case Op::Inc:
          case Op::Dec:
          case Op::Neg:
          case Op::Not: {
            need_kind(body, at, command, 0, OperandKind::VReg, "the result");
            need_value(body, at, command.args[1], "the operand");
            need_same_type(body, at, command, 1, 0);
            break;
          }
          case Op::Add:
          case Op::Sub:
          case Op::Mul:
          case Op::And:
          case Op::Or:
          case Op::Xor:
          case Op::Sdiv:
          case Op::Smod:
          case Op::Udiv:
          case Op::Umod:
          case Op::Shl:
          case Op::Shr:
          case Op::Sar: {
            need_kind(body, at, command, 0, OperandKind::VReg, "the result");
            need_value(body, at, command.args[1], "an operand");
            need_value(body, at, command.args[2], "an operand");
            need_same_type(body, at, command, 1, 2);
            need_same_type(body, at, command, 1, 0);
            need_signedness(body, at, command);
            break;
          }
          case Op::CmpEq:
          case Op::CmpNe:
          case Op::CmpLt:
          case Op::CmpLe:
          case Op::CmpGt:
          case Op::CmpGe: {
            // A comparison answers 0 or 1 in an i8, and its signedness comes
            // from the operands rather than from the result.
            need_kind(body, at, command, 0, OperandKind::VReg, "the result");
            need_type(body, at, command, 0, Type::I8);
            need_value(body, at, command.args[1], "an operand");
            need_value(body, at, command.args[2], "an operand");
            need_same_type(body, at, command, 1, 2);
            break;
          }
          case Op::Cast: {
            // The one op whose two operands differ on purpose: args[1] is read
            // in its own type and args[0] names the target's. Both still hold
            // a value, and void has nothing to convert.
            need_kind(body, at, command, 0, OperandKind::VReg, "the result");
            need_value(body, at, command.args[0], "the result");
            need_value(body, at, command.args[1], "the source");
            break;
          }
          case Op::Call: check_call(body, at, command); break;
          case Op::Primitive: check_primitive(body, at, command); break;
          case Op::Ret: check_ret(body, at, command); break;
        }
      }

      // 5. Calls. A call states its own shape — the callee is named in `args[1]` —
      // so the arguments and the result are checked against the function named.
      void check_call(const Body& body, At at, const Command& command) {
        const Operand& callee = command.args[1];
        if (callee.kind != OperandKind::Fn) {
          report(body, at,
                 "CALL needs a function to call, but " + slot_text(1) + " is " + std::string {kind_text(callee.kind)});
          return;
        }
        if (callee.index() >= program_.functions.size()) {
          report(body, at, "calls function " + std::to_string(callee.index()) + ", which does not exist");
          return;
        }
        const Function& function = program_.functions[callee.index()];
        const std::string named = "function " + std::to_string(callee.index());
        if (callee.type != function.return_type) {
          report(body, at,
                 "calls a function returning " + std::string {to_string(callee.type)} + ", but " + named + " returns " +
                     std::string {to_string(function.return_type)});
        }
        const std::size_t given = command.args.size() - 2;
        if (given != function.params.size()) {
          report(body, at,
                 "gives " + std::to_string(given) + " arguments to " + named + ", which takes " +
                     std::to_string(function.params.size()));
        } else {
          for (std::size_t slot = 0; slot < given; ++slot) {
            need_value(body, at, command.args[slot + 2], "an argument");
            need_type(body, at, command, slot + 2, function.params[slot]);
          }
        }
        const Operand& result = command.args[0];
        if (function.return_type == Type::Void) {
          if (!result.is_empty()) {
            report(body, at, "leaves a result in " + slot_text(0) + ", but " + named + " returns void");
          }
          return;
        }
        if (result.is_empty()) {
          report(body, at,
                 "leaves " + slot_text(0) + " empty, but " + named + " returns " +
                     std::string {to_string(function.return_type)});
          return;
        }
        need_kind(body, at, command, 0, OperandKind::VReg, "the result");
        need_type(body, at, command, 0, function.return_type);
      }

      /// A `PRIM` states its shape in the catalog: the primitive's id names the
      /// signature the result slot and the arguments are checked against
      /// (`docs/primitive.md#catalog`).
      void check_primitive(const Body& body, At at, const Command& command) {
        const primitives::Signature* signature = primitives::by_id(command.prim);
        if (!signature) {
          report(body, at, "PRIM names primitive " + std::to_string(command.prim) + ", which does not exist");
          return;
        }
        const std::string named = "primitive " + std::string {signature->name};
        const Type returns = primitives::lower(signature->return_type);
        const Operand& result = command.args[0];
        if (returns == Type::Void) {
          if (!result.is_empty()) {
            report(body, at, "leaves a result in " + slot_text(0) + ", but " + named + " returns void");
          }
        } else if (result.is_empty()) {
          report(body, at,
                 "leaves " + slot_text(0) + " empty, but " + named + " returns " + std::string {to_string(returns)});
        } else {
          need_kind(body, at, command, 0, OperandKind::VReg, "the result");
          need_type(body, at, command, 0, returns);
        }
        const std::size_t given = command.args.size() - 1;
        if (signature->variadic_values) {
          for (std::size_t slot = 0; slot < given; ++slot) {
            need_value(body, at, command.args[slot + 1], "an argument to " + named);
          }
          return;
        }
        if (given != signature->param_count) {
          report(body, at,
                 "gives " + std::to_string(given) + " arguments to " + named + ", which takes " +
                     std::to_string(signature->param_count));
          return;
        }
        for (std::size_t slot = 0; slot < given; ++slot) {
          need_value(body, at, command.args[slot + 1], "an argument to " + named);
          need_type(body, at, command, slot + 1, primitives::lower(signature->params[slot]));
        }
      }

      /// A `RET` carries exactly what the function returns: nothing for void, one
      /// value of the return type otherwise.
      void check_ret(const Body& body, At at, const Command& command) {
        if (body.return_type == Type::Void) {
          if (!command.args.empty()) {
            report(body, at, "returns " + slot_text(0) + " from a function that returns void");
          }
          return;
        }
        if (command.args.size() != 1) {
          report(body, at, "returns nothing, but the function returns " + std::string {to_string(body.return_type)});
          return;
        }
        need_value(body, at, command.args[0], "the returned value");
        if (command.args[0].type != body.return_type) {
          report(body, at,
                 "RET needs " + slot_text(0) + " to be " + std::string {to_string(body.return_type)} + ", but it is " +
                     std::string {to_string(command.args[0].type)});
        }
      }

      // 4. Definitions.
      void check_definitions(const Body& body) {
        const Cfg& cfg = *body.cfg;
        if (cfg.blocks.empty())
          return;
        const std::size_t cells = cell_count(body);
        std::vector<std::optional<Type>> types(cells);
        std::vector<std::vector<bool>> entering = defined_on_entry(body, cells);

        const std::vector<bool> reachable = reachable_blocks(cfg);
        for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
          if (!reachable[index])
            continue;  // already reported as unreachable
          std::vector<bool> defined = entering[index];
          const BasicBlock& block = cfg.blocks[index];
          for (std::size_t slot = 0; slot < block.commands.size(); ++slot) {
            const Command& command = block.commands[slot];
            const At at {index, slot};
            const std::string what = std::string {to_string(command.op)};
            for (std::size_t arg = 0; arg < command.args.size(); ++arg) {
              const Operand& operand = command.args[arg];
              if (operand.kind != OperandKind::VReg || operand.is_empty())
                continue;
              if (!is_write(command.op, arg) && !defined[operand.value]) {
                report(
                    body, at,
                    what + " reads $" + std::to_string(operand.value) + ", which nothing has written on the way here");
              }
              need_one_type(body, at, operand, what, types);
              if (is_write(command.op, arg))
                defined[operand.value] = true;
            }
          }
          // The branch is taken after the commands, so its operands are read
          // with everything the block has written.
          if (!block.branch)
            continue;
          const std::string what = std::string {to_string(block.branch->condition.cmp)} + " in the branch test";
          for (const Operand& operand : {block.branch->condition.a, block.branch->condition.b}) {
            if (operand.kind != OperandKind::VReg || operand.is_empty())
              continue;
            if (!defined[operand.value]) {
              report(body, At {index, std::nullopt},
                     what + " reads $" + std::to_string(operand.value) + ", which nothing has written on the way here");
            }
            need_one_type(body, At {index, std::nullopt}, operand, what, types);
          }
        }
      }

      /// The cells defined on entry to each block, by fixpoint: the params are
      /// defined at entry, and every other cell has to be written on each path that
      /// reaches the block reading it.
      std::vector<std::vector<bool>> defined_on_entry(const Body& body, std::size_t cells) {
        const Cfg& cfg = *body.cfg;
        const std::vector<std::vector<std::size_t>> incoming = predecessors_of(cfg);
        const std::vector<bool> reachable = reachable_blocks(cfg);
        std::vector<std::vector<bool>> written(cfg.blocks.size());
        for (std::size_t index = 0; index < cfg.blocks.size(); ++index) {
          written[index] = written_by(cfg.blocks[index], cells);
        }

        std::vector<std::vector<bool>> entering(cfg.blocks.size(), std::vector<bool>(cells, true));
        entering[0].assign(cells, false);
        for (std::size_t slot = 0; slot < (body.params ? body.params->size() : 0); ++slot) {
          if (slot < cells)
            entering[0][slot] = true;
        }

        bool changed = true;
        while (changed) {
          changed = false;
          for (std::size_t index = 1; index < cfg.blocks.size(); ++index) {
            if (!reachable[index])
              continue;
            std::vector<bool> meeting = entering[index];
            bool first = true;
            for (const std::size_t previous : incoming[index]) {
              std::vector<bool> leaving = entering[previous];
              for (std::size_t cell = 0; cell < cells; ++cell) {
                const bool here = leaving[cell] || written[previous][cell];
                meeting[cell] = first ? here : (meeting[cell] && here);
              }
              first = false;
            }
            if (meeting != entering[index]) {
              entering[index] = std::move(meeting);
              changed = true;
            }
          }
        }
        return entering;
      }

      // 6. Program.
      void check_entrypoint() {
        if (program_.entrypoint >= program_.functions.size()) {
          report(Subject::Program, 0,
                 "the entrypoint is function " + std::to_string(program_.entrypoint) + ", which does not exist");
          return;
        }
        const Function& entry = program_.functions[program_.entrypoint];
        if (!entry.params.empty()) {
          report(Subject::Program, 0,
                 "the entrypoint takes " + std::to_string(entry.params.size()) + " arguments, but takes none");
        }
        if (entry.return_type != Type::I32) {
          report(Subject::Program, 0,
                 "the entrypoint returns " + std::string {to_string(entry.return_type)} + ", but must return i32");
        }
      }
    };

  }  // namespace

  std::vector<ValidationError> validate(const Program& program) {
    return Validator {program}.run();
  }

}  // namespace erax::middleend