#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/// The shared IR of erax (`docs/middleend/JIR.md`): a register-style, stack-framed
/// machine program. Every operand is self-describing `(kind, type, value)`, so
/// nothing is resolved at load or run time and a `Program` serializes verbatim.
namespace erax::middleend {

    enum class Type : std::uint8_t {
        Void,
        I8,
        U8,
        I16,
        U16,
        I32,
        U32,
        I64,
        U64,
    };

    /// Width of a cell of this type, in bytes (`Void` is 0).
    constexpr std::size_t size_of(Type type) noexcept {
        switch (type) {
            case Type::Void: return 0;
            case Type::I8:
            case Type::U8: return 1;
            case Type::I16:
            case Type::U16: return 2;
            case Type::I32:
            case Type::U32: return 4;
            case Type::I64:
            case Type::U64: return 8;
        }
        return 0;
    }

    /// `Void` is the only non-integer type; everything else is a two's-complement
    /// integer whose signedness is this flag.
    constexpr bool is_integer(Type type) noexcept {
        return type != Type::Void;
    }

    constexpr bool is_signed(Type type) noexcept {
        switch (type) {
            case Type::I8:
            case Type::I16:
            case Type::I32:
            case Type::I64: return true;
            default: return false;
        }
    }

    /// Masks a shift count to the operand width, as the machine does.
    constexpr std::uint64_t shift_mask(Type type) noexcept {
        return size_of(type) * 8 - 1;
    }

    std::string_view to_string(Type type) noexcept;

    /// The op table: the single set of commands the IR has. It is frozen by the
    /// `.jir` format, so a new op is a format version bump.
    enum class Op : std::uint8_t {
        Mov,
        Add,
        Sub,
        Mul,
        Inc,
        Dec,
        Neg,
        Not,
        And,
        Or,
        Xor,
        Sdiv,
        Smod,
        Udiv,
        Umod,
        Shl,
        Shr,
        Sar,
        CmpEq,
        CmpNe,
        CmpLt,
        CmpLe,
        CmpGt,
        CmpGe,
        Call,
        Ret,
    };

    std::string_view to_string(Op op) noexcept;

    /// Argument count of an op. `Call` is `2` plus at least `0` arguments;
    /// `Unbounded` marks an op whose arity depends on its operands.
    struct Arity {
        std::uint8_t min;
        std::uint8_t max;

        static constexpr std::uint8_t unbounded = 0xFF;

        constexpr bool accepts(std::size_t count) const noexcept {
            return count >= min && (max == unbounded || count <= max);
        }
    };

    constexpr Arity arity(Op op) noexcept {
        switch (op) {
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
            case Op::Sar:
            case Op::CmpEq:
            case Op::CmpNe:
            case Op::CmpLt:
            case Op::CmpLe:
            case Op::CmpGt:
            case Op::CmpGe: return {3, 3};
            case Op::Mov:
            case Op::Inc:
            case Op::Dec:
            case Op::Neg:
            case Op::Not: return {2, 2};
            case Op::Ret: return {0, 1};
            case Op::Call: return {2, Arity::unbounded};
        }
        return {0, 0};
    }

    /// True when `args[0]` is the op's result slot.
    constexpr bool has_result(Op op) noexcept {
        return op != Op::Ret;
    }

    /// True for `CmpEq`..`CmpGe`, the only ops legal as a `Condition`'s `cmp`.
    constexpr bool is_compare(Op op) noexcept {
        return op >= Op::CmpEq && op <= Op::CmpGe;
    }

    enum class OperandKind : std::uint8_t {
        Instant,
        VReg,
        Global,
        Fn,
    };

    /// A self-describing operand. `value` is the immediate's raw bit pattern, the
    /// frame offset of a `VReg`, or a global/function id — never a resolved address.
    struct Operand {
        OperandKind kind = OperandKind::Instant;
        Type type = Type::Void;
        std::uint64_t value = 0;

        /// The `EMPTY` sentinel: a vreg offset that names no cell (`(VReg, void, -1)`).
        /// Legal only as the result slot of a `Call`.
        static constexpr std::uint64_t empty_value = std::numeric_limits<std::uint64_t>::max();

        static constexpr Operand instant(Type type, std::uint64_t bits) noexcept {
            return {OperandKind::Instant, type, bits};
        }

        static constexpr Operand vreg(Type type, std::uint64_t offset) noexcept {
            return {OperandKind::VReg, type, offset};
        }

        static constexpr Operand global(Type type, std::size_t id) noexcept {
            return {OperandKind::Global, type, id};
        }

        /// A function reference; `type` is the callee's return type.
        static constexpr Operand function(Type return_type, std::size_t id) noexcept {
            return {OperandKind::Fn, return_type, id};
        }

        static constexpr Operand empty() noexcept {
            return {OperandKind::VReg, Type::Void, empty_value};
        }

        constexpr bool is_empty() const noexcept {
            return *this == empty();
        }

        constexpr std::size_t index() const noexcept {
            return static_cast<std::size_t>(value);
        }

        friend constexpr bool operator==(const Operand&, const Operand&) = default;
    };

    static_assert(std::is_trivially_copyable_v<Operand>);

    std::string to_string(const Operand& operand);

    /// `OP arg0 arg1 ...`, where `args[0]` is the result slot of an op that has one.
    struct Command {
        Op op = Op::Ret;
        std::vector<Operand> args;
    };

    /// An edge test. `cmp` is one of `CmpEq`..`CmpGe` — the same ops as the `Cmp*`
    /// commands, so the IR has a single op table.
    struct Condition {
        Op cmp = Op::CmpNe;
        Operand a;
        Operand b;
    };

    /// The taken edge of a two-edge block.
    struct Branch {
        Condition condition;
        std::size_t target = 0;
    };

    /// Outgoing edges: none (the block ends in `Ret`), `next` only (unconditional),
    /// or `branch` plus `next` (the branch is taken when the condition holds, else
    /// the fallthrough). The shape makes a third edge, or a second conditional one,
    /// unrepresentable.
    struct BasicBlock {
        std::vector<Command> commands;
        std::optional<std::size_t> next;
        std::optional<Branch> branch;

        std::size_t edge_count() const noexcept {
            return (next ? 1u : 0u) + (branch ? 1u : 0u);
        }
    };

    /// Blocks in creation order; block 0 is the entry.
    struct Cfg {
        std::vector<BasicBlock> blocks;

        std::size_t size() const noexcept {
            return blocks.size();
        }
    };

    /// A function: `frame_size` cells, params in `$0..n-1`, one CFG.
    struct Function {
        std::vector<Type> params;
        Type return_type = Type::Void;
        std::size_t frame_size = 0;
        Cfg cfg;
    };

    /// A global: zeroed by the machine, then written by `init` in declaration order.
    struct Global {
        Type type = Type::Void;
        Cfg init;
    };

    /// Globals and functions, plus the entrypoint's id. Deliberately nameless: the
    /// contract identifies things by index, and names would only weigh the `.jir`
    /// format down.
    struct Program {
        std::vector<Global> globals;
        std::vector<Function> functions;
        std::size_t entrypoint = 0;
    };

    /// Human-readable rendering of the whole program, block by block — one line per
    /// command, the shape used in the IR spec's examples.
    std::string print(const Program& program);

}  // namespace erax::middleend
