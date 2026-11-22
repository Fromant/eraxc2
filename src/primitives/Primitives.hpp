#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "middleend/jir/Jir.hpp"

/// The closed catalog of primitives — the functions a backend provides
/// (`docs/primitive.md`). The frontend calls them by name, the IR carries them by
/// id, and the validator and the interpreter read the same table, so a signature
/// is stated once and cannot drift between the three.
namespace erax::primitives {

  /// A type inside a primitive's signature, kept distinct from both the source
  /// type (`frontend::SemType`) and the IR type: the catalog is shared by the
  /// frontend, the validator, and the interpreter, and so names none of their
  /// vocabulary.
  enum class SigType : std::uint8_t {
    Void,
    Bool,
    Char,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    /// A pointer to a byte (`ptr u8`). The catalog has no element type of its own;
    /// a pointer is one word wide whatever it points at, and the byte pointer is the
    /// one the allocator deals in.
    Ptr,
  };

  /// Catalog ids, fixed by `docs/primitive.md#catalog` and frozen by the `.jir`
  /// format: a primitive is never removed or renumbered, only appended to.
  enum class Prim : std::uint32_t {
    Print = 0,
    PrintChar = 1,
    ReadChar = 2,
    Exit = 3,
    Abort = 4,
    Alloc = 5,
    Free = 6,
    Offset = 7,
  };

  /// How many parameters a signature can hold. The widest primitive today takes
  /// two; raise this, and the table's arrays, if the catalog ever needs more.
  inline constexpr std::size_t max_params = 2;

  /// One primitive's contract. `params` holds the fixed parameters; a
  /// `variadic_values` primitive takes any number of value arguments after the
  /// result slot instead. `terminating` primitives end the flow of the block they
  /// are in, exactly like `RET` (`docs/primitive.md#flow`).
  struct Signature {
    Prim id = Prim::Print;
    std::string_view name;
    std::array<SigType, max_params> params {};
    std::uint8_t param_count = 0;
    SigType return_type = SigType::Void;
    bool variadic_values = false;
    bool terminating = false;
  };

  /// The whole catalog, in id order — its index *is* the id.
  std::span<const Signature> catalog() noexcept;

  /// The signature of \p name, or null when no primitive has that name.
  const Signature* by_name(std::string_view name) noexcept;

  /// The signature of the primitive with id \p id, or null when the id is out of
  /// range — the form the validator and the interpreter use, since the IR
  /// carries ids.
  const Signature* by_id(std::uint32_t id) noexcept;

  /// The IR type a signature type lowers to: `bool` → `i8`, `char` → `u8`, and
  /// every integer to itself, exactly as the source types do.
  middleend::Type lower(SigType type) noexcept;

  /// True when \p command is a terminating `PRIM` (`exit`/`abort`): it closes its
  /// block exactly like `RET` — it is the block's last command and the block takes
  /// no edges (`docs/primitive.md#flow`).
  bool terminates(const middleend::Command& command) noexcept;

}  // namespace erax::primitives
