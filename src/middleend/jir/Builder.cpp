#include "Builder.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

namespace erax::middleend {

    CfgBuilder::CfgBuilder(std::size_t param_count) {
        vreg_types_.resize(param_count, Type::Void);
        next_vreg_ = param_count;
        frame_size_ = std::max<std::size_t>(1, param_count);
        cfg_.blocks.emplace_back();
    }

    CfgBuilder::CfgBuilder(std::vector<Type> params) {
        for (std::size_t index = 0; index < params.size(); ++index)
            vreg_types_.push_back(params[index]);
        next_vreg_ = params.size();
        frame_size_ = std::max<std::size_t>(1, params.size());
        cfg_.blocks.emplace_back();
    }

    std::uint64_t CfgBuilder::alloc(Type type) {
        // A recycled cell is only reusable at its own type: one vreg id means one
        // type across the whole function.
        for (auto cell = free_.begin(); cell != free_.end(); ++cell) {
            if (vreg_types_[*cell] == type) {
                const std::uint64_t offset = *cell;
                free_.erase(cell);
                return offset;
            }
        }
        const std::uint64_t offset = next_vreg_++;
        vreg_types_.resize(next_vreg_, Type::Void);
        vreg_types_[offset] = type;
        frame_size_ = std::max(frame_size_, static_cast<std::size_t>(offset) + 1);
        return offset;
    }

    Type CfgBuilder::type_of(std::uint64_t offset) const {
        return offset < vreg_types_.size() ? vreg_types_[offset] : Type::Void;
    }

    std::size_t CfgBuilder::set_current(std::size_t block) noexcept {
        current_ = block;
        return current_;
    }

    void CfgBuilder::emit(Op op, std::vector<Operand> args) {
        assert(arity(op).accepts(args.size()));
        if (terminated())
            open_block();
        BasicBlock& block = cfg_.blocks[current_];
        block.commands.push_back(Command {op, std::move(args)});
        if (op == Op::Ret) {
            assert(!block.next && !block.branch);
        }
    }

    std::size_t CfgBuilder::new_block() {
        cfg_.blocks.emplace_back();
        return cfg_.blocks.size() - 1;
    }

    std::size_t CfgBuilder::open_block() {
        current_ = new_block();
        return current_;
    }

    bool CfgBuilder::link_next(std::size_t target) {
        BasicBlock& block = cfg_.blocks[current_];
        if (block.next || block.branch)
            return false;
        assert(target < cfg_.blocks.size());
        block.next = target;
        return true;
    }

    bool CfgBuilder::link_branch(std::size_t target, Condition condition, std::size_t fallthrough) {
        BasicBlock& block = cfg_.blocks[current_];
        if (block.next || block.branch)
            return false;
        assert(target < cfg_.blocks.size());
        assert(fallthrough < cfg_.blocks.size());
        block.branch = Branch {condition, target};
        block.next = fallthrough;
        return true;
    }

    bool CfgBuilder::terminated() const noexcept {
        const BasicBlock& block = cfg_.blocks[current_];
        if (block.next || block.branch)
            return true;
        return !block.commands.empty() && block.commands.back().op == Op::Ret;
    }

    void CfgBuilder::recycle(std::uint64_t high_water) noexcept {
        // The cells stay allocated — they are on the free list, not unallocated —
        // so the frame keeps its high-water mark. Only their ids are handed back,
        // and a cell that is already free is left alone: closing a scope recycles
        // the cells of every scope inside it, one range at a time.
        for (std::uint64_t offset = high_water; offset < next_vreg_; ++offset) {
            if (vreg_types_[offset] == Type::Void)
                continue;
            if (std::find(free_.begin(), free_.end(), offset) != free_.end())
                continue;
            free_.push_back(offset);
        }
    }

    CfgBuilder::Mark CfgBuilder::mark() const noexcept {
        return Mark {current_, cfg_.blocks[current_].commands.size(), next_vreg_, cfg_.blocks.size()};
    }

    void CfgBuilder::rollback(Mark mark) noexcept {
        for (std::size_t index = cfg_.blocks.size(); index-- > mark.blocks;) {
            cfg_.blocks.pop_back();
        }
        BasicBlock& block = cfg_.blocks[mark.block];
        block.commands.resize(mark.commands);
        block.next.reset();
        block.branch.reset();
        current_ = mark.block;
        next_vreg_ = mark.vregs;
        vreg_types_.resize(mark.vregs, Type::Void);
        // A rolled-back cell was never really allocated, so it cannot be free.
        free_.erase(std::remove_if(free_.begin(), free_.end(),
                                   [mark](std::uint64_t offset) {
                                       return offset >= mark.vregs;
                                   }),
                    free_.end());
    }

    void CfgBuilder::prune() {
        std::vector<std::size_t> remap(cfg_.blocks.size(), static_cast<std::size_t>(-1));
        std::vector<std::size_t> keep;
        std::vector<std::size_t> stack {0};

        remap[0] = 0;
        keep.push_back(0);
        while (!stack.empty()) {
            const std::size_t block = stack.back();
            stack.pop_back();
            const BasicBlock& node = cfg_.blocks[block];
            const std::size_t targets[2] = {node.next.value_or(static_cast<std::size_t>(-1)),
                                            node.branch ? node.branch->target : static_cast<std::size_t>(-1)};
            for (const std::size_t target : targets) {
                if (target == static_cast<std::size_t>(-1) || remap[target] != static_cast<std::size_t>(-1))
                    continue;
                remap[target] = keep.size();
                keep.push_back(target);
                stack.push_back(target);
            }
        }

        Cfg pruned;
        pruned.blocks.reserve(keep.size());
        for (const std::size_t block : keep) {
            BasicBlock node = cfg_.blocks[block];
            if (node.next)
                node.next = remap[*node.next];
            if (node.branch)
                node.branch->target = remap[node.branch->target];
            pruned.blocks.push_back(std::move(node));
        }
        cfg_ = std::move(pruned);
        current_ = 0;
    }

}  // namespace erax::middleend
