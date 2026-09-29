#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Jir.hpp"

namespace erax::middleend {

/// Builds one CFG and the frame it indexes into: vreg allocation, command
/// emission, and the block edges that make control flow.
///
/// Every producer of a `Cfg` goes through this, so the invariants the IR
/// validator checks are established as the graph is built: a vreg has one type,
/// a block never grows a third edge or a second conditional one, and `Ret`
/// closes a block instead of leaving it to fall off the end.
class CfgBuilder {
public:
    /// \p param_count cells are already occupied by the params, in `$0..n-1`.
    explicit CfgBuilder(std::size_t param_count = 0);
    /// The same, with each param's type recorded in its cell.
    explicit CfgBuilder(std::vector<Type> params);

    /// A frame cell of type \p type, reusing a recycled cell of that same type
    /// when there is one — a vreg id means one type for the whole function
    /// (`docs/middleend/JIR.md#validator`), so a cell is only ever reused at its
    /// own type. Ids are never reused within a scope; `recycle` hands them back.
    std::uint64_t alloc(Type type);

    /// The type recorded for a vreg, or `Type::Void` if it has none.
    Type type_of(std::uint64_t offset) const;

    /// Appends a command to the current block, starting a fresh block if the
    /// previous one was closed by `Ret` — unreachable code lands in a block no
    /// edge reaches, and `prune` drops it.
    void emit(Op op, std::vector<Operand> args);

    /// The id of the current block, the one commands are appended to.
    std::size_t current() const noexcept { return current_; }

    /// Starts a new empty block and returns its id; it does not become current.
    std::size_t new_block();

    /// Makes a new block current, starting one if the current block is closed.
    std::size_t open_block();

    /// Marks the current block as falling through to \p target. Fails if the
    /// block already has edges.
    bool link_next(std::size_t target);

    /// Marks the current block as branching to \p target, falling through to
    /// \p fallthrough. Fails if the block already has edges.
    bool link_branch(std::size_t target, Condition condition, std::size_t fallthrough);

    /// True when the current block ends in `Ret` and takes no more commands.
    bool terminated() const noexcept;

    /// One more than the highest vreg id handed out, and never 0: a frame is
    /// never empty.
    std::size_t frame_size() const noexcept { return frame_size_; }

    /// The first id no scope has handed out yet: a scope's high-water mark, and
    /// everything from here on is that scope's to recycle.
    std::uint64_t next_vreg() const noexcept { return next_vreg_; }

    /// Hands a scope's cells back for reuse by later scopes, each at its own
    /// type. Ids that any emitted command still names must not be recycled.
    void recycle(std::uint64_t high_water) noexcept;

    struct Mark {
        std::size_t block;
        std::size_t commands;
        std::uint64_t vregs;
        std::size_t blocks;
    };

    /// A position to undo emission to.
    Mark mark() const noexcept;

    /// Undoes everything `mark` covers, including blocks created since. Safe
    /// because a block that exists but is unreachable is dropped by `prune`.
    void rollback(Mark mark) noexcept;

    /// Drops the blocks no edge reaches, remapping every edge target. Block 0
    /// is the entry and is always kept.
    void prune();

    const Cfg& cfg() const noexcept { return cfg_; }
    Cfg take() noexcept { return std::move(cfg_); }

    /// Starts a fresh block as current, abandoning the current one (which is
    /// then reachable only through the edges already written to it).
    std::size_t set_current(std::size_t block) noexcept;

private:
    Cfg cfg_;
    std::size_t current_ = 0;
    std::vector<Type> vreg_types_;
    /// Recycled cell ids, each still carrying its own recorded type.
    std::vector<std::uint64_t> free_;
    std::uint64_t next_vreg_ = 0;
    std::size_t frame_size_ = 1;
};

}  // namespace erax::middleend
