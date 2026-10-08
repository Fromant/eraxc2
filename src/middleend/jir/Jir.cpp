#include "Jir.hpp"

#include <string>

#include "primitives/Primitives.hpp"

namespace erax::middleend {

  namespace {

    std::string signed_text(std::uint64_t bits, std::size_t width) {
      const std::size_t used = width * 8;
      if (used == 64)
        return std::to_string(static_cast<std::int64_t>(bits));
      std::uint64_t value = bits & ((std::uint64_t {1} << used) - 1);
      const std::uint64_t sign = std::uint64_t {1} << (used - 1);
      if ((value & sign) != 0)
        value |= ~((std::uint64_t {1} << used) - 1);
      return std::to_string(static_cast<std::int64_t>(value));
    }

    std::string unsigned_text(std::uint64_t bits, std::size_t width) {
      const std::size_t used = width * 8;
      return std::to_string(used == 64 ? bits : bits & ((std::uint64_t {1} << used) - 1));
    }

    void print_cfg(const Cfg& cfg, std::string& out, const std::string& indent) {
      for (std::size_t block = 0; block < cfg.blocks.size(); ++block) {
        const BasicBlock& node = cfg.blocks[block];
        out += indent + std::to_string(block) + ":\n";
        for (const Command& command : node.commands) {
          out += indent + "  " + std::string {to_string(command.op)};
          // The text form of a `PRIM` spells the primitive's catalog name;
          // an id with no row is printed as the number it is.
          if (command.op == Op::Primitive) {
            const primitives::Signature* signature = primitives::by_id(command.prim);
            out += signature ? " " + std::string {signature->name} : " " + std::to_string(command.prim);
          }
          for (const Operand& arg : command.args)
            out += " " + to_string(arg);
          out += "\n";
        }
        if (node.branch) {
          out += indent + "  if " + std::string {to_string(node.branch->condition.cmp)} + " " +
              to_string(node.branch->condition.a) + ", " + to_string(node.branch->condition.b) + " then " +
              std::to_string(node.branch->target) + "\n";
        }
        if (node.next)
          out += indent + "  -> " + std::to_string(*node.next) + "\n";
      }
    }

  }  // namespace

  std::string_view to_string(Type type) noexcept {
    switch (type) {
      case Type::Void: return "void";
      case Type::I8: return "i8";
      case Type::U8: return "u8";
      case Type::I16: return "i16";
      case Type::U16: return "u16";
      case Type::I32: return "i32";
      case Type::U32: return "u32";
      case Type::I64: return "i64";
      case Type::U64: return "u64";
    }
    return "unknown";
  }

  std::string_view to_string(Op op) noexcept {
    switch (op) {
      case Op::Mov: return "MOV";
      case Op::Add: return "ADD";
      case Op::Sub: return "SUB";
      case Op::Mul: return "MUL";
      case Op::Inc: return "INC";
      case Op::Dec: return "DEC";
      case Op::Neg: return "NEG";
      case Op::Not: return "NOT";
      case Op::And: return "AND";
      case Op::Or: return "OR";
      case Op::Xor: return "XOR";
      case Op::Sdiv: return "SDIV";
      case Op::Smod: return "SMOD";
      case Op::Udiv: return "UDIV";
      case Op::Umod: return "UMOD";
      case Op::Shl: return "SHL";
      case Op::Shr: return "SHR";
      case Op::Sar: return "SAR";
      case Op::Cast: return "CAST";
      case Op::CmpEq: return "CMP_EQ";
      case Op::CmpNe: return "CMP_NE";
      case Op::CmpLt: return "CMP_LT";
      case Op::CmpLe: return "CMP_LE";
      case Op::CmpGt: return "CMP_GT";
      case Op::CmpGe: return "CMP_GE";
      case Op::Call: return "CALL";
      case Op::Primitive: return "PRIM";
      case Op::Ret: return "RET";
    }
    return "unknown";
  }

  std::string to_string(const Operand& operand) {
    switch (operand.kind) {
      case OperandKind::Instant:
        return std::string {to_string(operand.type)} + " " + decimal_text(operand.type, operand.value);
      case OperandKind::VReg:
        if (operand.is_empty())
          return "EMPTY";
        return std::string {to_string(operand.type)} + " $" + std::to_string(operand.value);
      case OperandKind::Global: return "@" + std::to_string(operand.value);
      case OperandKind::Fn: return "&" + std::to_string(operand.value);
    }
    return "unknown";
  }

  std::string decimal_text(Type type, std::uint64_t bits) {
    if (is_signed(type))
      return signed_text(bits, size_of(type));
    return unsigned_text(bits, size_of(type));
  }

  std::string print(const Program& program) {
    std::string out;
    for (std::size_t index = 0; index < program.globals.size(); ++index) {
      const Global& global = program.globals[index];
      out += "global " + std::to_string(index) + ": " + std::string {to_string(global.type)} + "\n";
      if (global.init.blocks.empty()) {
        out += "  (no initializer)\n";
        continue;
      }
      print_cfg(global.init, out, "  init ");
    }
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      const Function& function = program.functions[index];
      out += "function " + std::to_string(index) + ": (";
      for (std::size_t param = 0; param < function.params.size(); ++param) {
        if (param != 0)
          out += ", ";
        out += to_string(function.params[param]);
      }
      out += ") -> " + std::string {to_string(function.return_type)} + ", frame " +
          std::to_string(function.frame_size) + "\n";
      print_cfg(function.cfg, out, "  ");
    }
    if (!program.functions.empty()) {
      out += "entrypoint: " + std::to_string(program.entrypoint) + "\n";
    }
    return out;
  }

}  // namespace erax::middleend
