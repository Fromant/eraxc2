#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The shared IR of erax (`docs/middleend/JIR.md`): a register-style, stack-framed
/// machine program. Every operand is self-describing `(kind, type, value)`, so
/// nothing is resolved at load or run time and a `Program` serializes verbatim.
namespace erax::middleend {

  /// A machine type: one of the scalar kinds (`Void` plus the integers), or a
  /// pointer to another type. `pointee` is non-null exactly when `kind` is `Ptr`,
  /// and equality is structural, so two separately built `ptr i32`s are the same
  /// type. A pointer is one cell wide, whatever it points at.
  struct Type {
    enum class Kind : std::uint8_t {
      Void,
      I8,
      U8,
      I16,
      U16,
      I32,
      U32,
      I64,
      U64,
      Ptr,
    };

    Kind kind = Kind::Void;
    std::shared_ptr<const Type> pointee;

    /// The scalar types, named where a type is spelled (`Type::I32`). A pointer is
    /// built with `pointer`, never named directly.
    static const Type Void;
    static const Type I8;
    static const Type U8;
    static const Type I16;
    static const Type U16;
    static const Type I32;
    static const Type U32;
    static const Type I64;
    static const Type U64;

    /// A pointer to \p pointee.
    static Type pointer(Type pointee) noexcept {
      return Type {Kind::Ptr, std::make_shared<const Type>(pointee)};
    }

    bool is_pointer() const noexcept {
      return kind == Kind::Ptr;
    }
  };

  inline const Type Type::Void {Type::Kind::Void, {}};
  inline const Type Type::I8 {Type::Kind::I8, {}};
  inline const Type Type::U8 {Type::Kind::U8, {}};
  inline const Type Type::I16 {Type::Kind::I16, {}};
  inline const Type Type::U16 {Type::Kind::U16, {}};
  inline const Type Type::I32 {Type::Kind::I32, {}};
  inline const Type Type::U32 {Type::Kind::U32, {}};
  inline const Type Type::I64 {Type::Kind::I64, {}};
  inline const Type Type::U64 {Type::Kind::U64, {}};

  /// Structural equality, so a type built in one place matches the same type built
  /// in another: the scalar kinds compare by kind, a pointer also by its pointee.
  inline bool operator==(const Type& a, const Type& b) noexcept {
    if (a.kind != b.kind)
      return false;
    if (a.kind != Type::Kind::Ptr)
      return true;
    return *a.pointee == *b.pointee;
  }

  /// Width of a cell of this type, in bytes (`Void` is 0, a pointer is one word).
  inline std::size_t size_of(Type type) noexcept {
    switch (type.kind) {
      case Type::Kind::Void: return 0;
      case Type::Kind::I8:
      case Type::Kind::U8: return 1;
      case Type::Kind::I16:
      case Type::Kind::U16: return 2;
      case Type::Kind::I32:
      case Type::Kind::U32: return 4;
      case Type::Kind::I64:
      case Type::Kind::U64:
      case Type::Kind::Ptr: return 8;
    }
    return 0;
  }

  /// `Void` and a pointer are the non-integer types; everything else is a
  /// two's-complement integer whose signedness is this flag.
  inline bool is_integer(Type type) noexcept {
    return type.kind != Type::Kind::Void && type.kind != Type::Kind::Ptr;
  }

  inline bool is_signed(Type type) noexcept {
    switch (type.kind) {
      case Type::Kind::I8:
      case Type::Kind::I16:
      case Type::Kind::I32:
      case Type::Kind::I64: return true;
      default: return false;
    }
  }

  /// Masks a shift count to the operand width, as the machine does.
  inline std::uint64_t shift_mask(Type type) noexcept {
    return size_of(type) * 8 - 1;
  }

  std::string to_string(Type type);

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
    Cast,
    Load,
    Store,
    Addr,
    CmpEq,
    CmpNe,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,
    Call,
    Primitive,
    Ret,
  };

  std::string_view to_string(Op op) noexcept;

  /// Argument count of an op. `Call` is `2` plus at least `0` arguments and
  /// `Primitive` is its result slot plus any number of arguments; `Unbounded`
  /// marks an op whose arity depends on its operands.
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
      case Op::Not:
      case Op::Cast:
      case Op::Load:
      case Op::Store:
      case Op::Addr: return {2, 2};
      case Op::Ret: return {0, 1};
      case Op::Call: return {2, Arity::unbounded};
      case Op::Primitive: return {1, Arity::unbounded};
    }
    return {0, 0};
  }

  /// True when `args[0]` is the op's result slot. `Ret` has none, and `Store` has
  /// none either — it writes through its pointer rather than producing a value. A
  /// `Primitive`'s result is the `EMPTY` sentinel when the primitive returns void.
  constexpr bool has_result(Op op) noexcept {
    return op != Op::Ret && op != Op::Store;
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

    static Operand instant(Type type, std::uint64_t bits) noexcept {
      return {OperandKind::Instant, type, bits};
    }

    static Operand vreg(Type type, std::uint64_t offset) noexcept {
      return {OperandKind::VReg, type, offset};
    }

    static Operand global(Type type, std::size_t id) noexcept {
      return {OperandKind::Global, type, id};
    }

    /// A function reference; `type` is the callee's return type.
    static Operand function(Type return_type, std::size_t id) noexcept {
      return {OperandKind::Fn, return_type, id};
    }

    static Operand empty() noexcept {
      return {OperandKind::VReg, Type::Void, empty_value};
    }

    bool is_empty() const noexcept {
      return *this == empty();
    }

    constexpr std::size_t index() const noexcept {
      return static_cast<std::size_t>(value);
    }

    friend bool operator==(const Operand&, const Operand&) = default;
  };

  std::string to_string(const Operand& operand);

  /// The decimal rendering of a bit pattern read as \p type: two's-complement for a
  /// signed type, zero-extended for an unsigned one. It is the number a `PRIM
  /// print` writes and the number an `Instant` renders after its type name, so the
  /// two can never disagree.
  std::string decimal_text(Type type, std::uint64_t bits);

  /// `OP arg0 arg1 ...`, where `args[0]` is the result slot of an op that has one.
  /// `prim` is the catalog id of the primitive a `Primitive` command runs, and 0
  /// for every other op (`docs/primitive.md`).
  struct Command {
    Op op = Op::Ret;
    std::vector<Operand> args;
    std::uint32_t prim = 0;
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
