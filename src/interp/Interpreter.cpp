#include "Interpreter.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "middleend/jir/Fold.hpp"
#include "primitives/Primitives.hpp"

namespace erax::interp {

  using middleend::BasicBlock;
  using middleend::Command;
  using middleend::Condition;
  using middleend::Op;
  using middleend::Operand;
  using middleend::OperandKind;
  using middleend::Program;
  using middleend::Subject;
  using primitives::Prim;

  namespace {

    /// One activation: where it is in the program, what runs next, and the cells it
    /// owns. The frame is the slice of `Machine::stack_` from `base` up, so a
    /// recursive call is another slice and a return is a resize.
    struct Frame {
      Subject subject = Subject::Function;
      std::size_t index = 0;
      std::size_t block = 0;
      std::size_t command = 0;
      std::size_t base = 0;  ///< where `$0` is in the cell array
      std::size_t cells = 0;  ///< how many cells the frame owns
      /// Where this activation's value goes when it returns: the `CALL`'s result
      /// slot, which lives in the caller's frame. The entrypoint has none.
      std::optional<Operand> result;
    };

    /// One past the highest cell a CFG names. A function's frame is exactly this size
    /// by contract; an initializer has no frame of its own in the IR, so the machine
    /// gives it the one its vregs need.
    std::size_t cells_named(const middleend::Cfg& cfg) {
      std::size_t highest = 0;
      bool named = false;
      auto consider = [&](const Operand& operand) {
        if (operand.kind != OperandKind::VReg || operand.is_empty()) {
          return;
        }
        if (!named || operand.value > highest) {
          highest = static_cast<std::size_t>(operand.value);
          named = true;
        }
      };
      for (const BasicBlock& block : cfg.blocks) {
        for (const Command& command : block.commands) {
          for (const Operand& operand : command.args) {
            consider(operand);
          }
        }
        if (!block.branch) {
          continue;
        }
        consider(block.branch->condition.a);
        consider(block.branch->condition.b);
      }
      return named ? highest + 1 : 0;
    }

    /// The machine of `JIR.md#frames`: one array of cells for every frame, one frame
    /// pointer walking it, and the CFG saying where to go next.
    class Machine {
  public:
      Machine(const Program& program, const Limits& limits, Input* input) noexcept :
          program_(program), limits_(limits), input_(input), globals_(program.globals.size(), Cell {0}) {}

      std::expected<Outcome, Failure> run() {
        // Every operand is read through an index, so the machine asks the
        // validator first: a program it rejects never gets near the cell array.
        auto valid = middleend::check(program_);
        if (!valid) {
          return std::unexpected(Failure {Rejected {std::move(valid.error())}});
        }

        for (std::size_t index = 0; index < program_.globals.size(); ++index) {
          const middleend::Cfg& init = program_.globals[index].init;
          if (init.blocks.empty()) {
            continue;  // zeroed by the machine, nothing to run
          }
          if (!push(Subject::GlobalInit, index, cells_named(init), Site {Subject::GlobalInit, index, 0, 0, 1})) {
            return std::unexpected(failure());
          }
          if (!execute()) {
            return std::unexpected(failure());
          }
          if (exiting_) {
            return Outcome {globals_, *exiting_, output_};
          }
        }

        if (program_.entrypoint >= program_.functions.size()) {
          break_("the entrypoint names a function that does not exist");
          return std::unexpected(failure());
        }
        returning_.reset();
        if (!push(Subject::Function, program_.entrypoint, program_.functions[program_.entrypoint].frame_size,
                  Site {Subject::Function, program_.entrypoint, 0, 0, 1})) {
          return std::unexpected(failure());
        }
        if (!execute()) {
          return std::unexpected(failure());
        }
        if (exiting_) {
          return Outcome {globals_, *exiting_, output_};
        }
        if (!returning_) {
          break_("the entrypoint returned no value to be its exit code");
          return std::unexpected(failure());
        }
        return Outcome {globals_, *returning_, output_};
      }

  private:
      /// Runs frames until the stack is empty. `false` means nothing was left to run.
      bool execute() {
        while (!frames_.empty()) {
          const Frame frame = frames_.back();
          const middleend::Cfg* cfg = cfg_of(frame);
          if (!cfg) {
            return false;
          }
          if (frame.block >= cfg->blocks.size()) {
            break_("a frame is in a block that does not exist");
            return false;
          }
          const BasicBlock& block = cfg->blocks[frame.block];
          if (frame.command == block.commands.size()) {
            if (!advance(frame)) {
              return false;
            }
            continue;
          }
          const Command& command = block.commands[frame.command];
          const Site site = site_of(frame);
          // The command advances its block before it runs: `RET` and `CALL` then
          // change the stack, and the loop reads whatever frame is on top next.
          ++frames_.back().command;
          if (!run_command(command, frame, site)) {
            return false;
          }
        }
        return !died();
      }

      /// A block that has run out of commands follows one of its edges. A `RET` has
      /// already popped the frame by now, so what is left here always has an edge.
      bool advance(Frame frame) {
        const middleend::Cfg* cfg = cfg_of(frame);
        const BasicBlock& block = cfg->blocks[frame.block];
        std::size_t target = 0;
        if (block.branch) {
          bool holds = false;
          if (!holds_(block.branch->condition, frame, holds)) {
            return false;
          }
          if (!block.next) {
            break_("a block with a branch has no fallthrough edge");
            return false;
          }
          target = holds ? block.branch->target : *block.next;
        } else if (block.next) {
          target = *block.next;
        } else {
          break_("a block ends without a RET and without an edge");
          return false;
        }
        if (target >= cfg->blocks.size()) {
          break_("an edge names a block that does not exist");
          return false;
        }
        frames_.back().block = target;
        frames_.back().command = 0;
        return true;
      }

      bool run_command(const Command& command, Frame frame, const Site& site) {
        if (!middleend::arity(command.op).accepts(command.args.size())) {
          break_("a command has the wrong number of operands");
          return false;
        }
        switch (command.op) {
          case Op::Ret: return ret(command, frame);
          case Op::Call: return call(command, frame, site);
          case Op::Primitive: return primitive(command, frame);
          case Op::Mov: {
            const Cell value = read(command.args[1], frame);
            write(command.args[0], value, frame);
            return !died();
          }
          case Op::Inc:
          case Op::Dec:
          case Op::Neg:
          case Op::Not: {
            const Cell value = read(command.args[1], frame);
            if (died()) {
              return false;
            }
            return land(middleend::fold::unary(command.op, command.args[1].type, value), command.args[0], frame, site);
          }
          case Op::Cast: {
            // Read in the source's type, re-wrap into the destination's — the
            // same `fold` code the front end folds constants with.
            const Cell source = read(command.args[1], frame);
            if (died()) {
              return false;
            }
            write(command.args[0], middleend::fold::convert(command.args[0].type, command.args[1].type, source), frame);
            return !died();
          }
          case Op::Addr: {
            // A cell's address is its region and its index, packed into one word; a
            // vreg is addressed relative to its frame's base in the stack array.
            const Operand& source = command.args[1];
            if (source.kind == OperandKind::VReg) {
              if (source.is_empty() || source.value >= frame.cells) {
                break_("ADDR names a cell outside its frame");
                return false;
              }
              write(command.args[0], encode(Region::Stack, frame.base + source.value), frame);
            } else if (source.kind == OperandKind::Global) {
              if (source.value >= globals_.size()) {
                break_("ADDR names a global that does not exist");
                return false;
              }
              write(command.args[0], encode(Region::Global, source.value), frame);
            } else {
              break_("ADDR names something that is not a cell");
              return false;
            }
            return !died();
          }
          case Op::Load: {
            const Cell address = read(command.args[1], frame);
            if (died()) {
              return false;
            }
            const Cell* cell = deref(address, site);
            if (!cell) {
              return false;
            }
            write(command.args[0], *cell, frame);
            return !died();
          }
          case Op::Offset: {
            const Cell address = read(command.args[1], frame);
            const Cell index = read(command.args[2], frame);
            if (died()) {
              return false;
            }
            const Cell result = offset_address(address, index, site);
            if (died()) {
              return false;
            }
            write(command.args[0], result, frame);
            return !died();
          }
          case Op::Store: {
            const Cell address = read(command.args[0], frame);
            if (died()) {
              return false;
            }
            const Cell value = read(command.args[1], frame);
            if (died()) {
              return false;
            }
            Cell* cell = deref(address, site);
            if (!cell) {
              return false;
            }
            *cell = value;
            return true;
          }
          default: {
            const Cell left = read(command.args[1], frame);
            const Cell right = read(command.args[2], frame);
            if (died()) {
              return false;
            }
            // The operands share a type (the validator saw to that), so its
            // signedness decides div, mod, and `SAR`.
            return land(middleend::fold::binary(command.op, command.args[1].type, left, right), command.args[0], frame,
                        site);
          }
        }
      }

      /// The fold decides the trap: division and signed overflow are the two ways
      /// arithmetic has no answer, and both are the machine dying.
      bool land(const middleend::fold::Result& result, const Operand& destination, Frame frame, const Site& site) {
        if (result.failure) {
          trap(
              *result.failure == middleend::fold::Failure::DivisionByZero ? Trap::DivisionByZero : Trap::SignedOverflow,
              site);
          return false;
        }
        write(destination, result.value.value_or(0), frame);
        return !died();
      }

      /// `RET` pops the frame. A value the `RET` carried belongs to the caller: the
      /// `CALL` said which of its cells it lands in (`JIR.md#calls`).
      bool ret(const Command& command, Frame frame) {
        Cell value = 0;
        const bool carries = !command.args.empty();
        if (carries) {
          value = read(command.args[0], frame);
          if (died()) {
            return false;
          }
        }
        stack_.resize(frame.base);
        frames_.pop_back();
        if (!carries) {
          return true;
        }
        // The outermost `RET` is the entrypoint's, and its value is the exit code.
        if (frames_.empty()) {
          returning_ = value;
        }
        if (frame.result && !frames_.empty()) {
          write(*frame.result, value, frames_.back());
        }
        return !died();
      }

      /// `CALL dst &f a0 …`: the arguments are read in the caller's frame, then
      /// copied into the callee's `$0..$n-1` (`JIR.md#calls`).
      bool call(const Command& command, Frame frame, const Site& site) {
        const Operand& callee = command.args[1];
        if (callee.kind != OperandKind::Fn || callee.index() >= program_.functions.size()) {
          break_("a call names a function that does not exist");
          return false;
        }
        const middleend::Function& function = program_.functions[callee.index()];
        std::vector<Cell> arguments;
        arguments.reserve(command.args.size() - 2);
        for (std::size_t slot = 2; slot < command.args.size(); ++slot) {
          arguments.push_back(read(command.args[slot], frame));
          if (died()) {
            return false;
          }
        }
        if (arguments.size() > function.frame_size) {
          break_("a call passes more arguments than the callee's frame holds");
          return false;
        }
        if (!push(Subject::Function, callee.index(), function.frame_size, site)) {
          return false;
        }
        const std::size_t base = frames_.back().base;
        frames_.back().result = command.args[0];
        for (std::size_t slot = 0; slot < arguments.size(); ++slot) {
          stack_[base + slot] = arguments[slot];
        }
        return true;
      }

      /// `PRIM p dst a0 …`: the catalog decides what runs (`docs/primitive.md`).
      /// The validator has already checked the id against it, so an unknown one
      /// here is the machine's own doing and is reported, not indexed.
      bool primitive(const Command& command, Frame frame) {
        const primitives::Signature* signature = primitives::by_id(command.prim);
        if (!signature) {
          break_("a command names a primitive that does not exist");
          return false;
        }
        switch (signature->id) {
          case Prim::Print: return print(command, frame);
          case Prim::PrintChar: return print_char(command, frame);
          case Prim::ReadChar: return read_char(command, frame);
          case Prim::Exit: return exit_(command, frame);
          case Prim::Abort: return abort_();
          case Prim::Alloc: return alloc_(command, frame);
          case Prim::Free: return free_(command, frame);
          case Prim::Offset: return offset_(command, frame);
        }
        break_("a command names a primitive that does not exist");
        return false;
      }

      /// `PRIM print dst a0 …`: writes each value as a signed or unsigned decimal
      /// — the interpreter's `decimal_text`, which renders an `Instant` the same
      /// way — separated by single spaces, then a newline. A bare `print` writes
      /// only the newline. The result slot is `EMPTY` and is never read.
      bool print(const Command& command, Frame frame) {
        for (std::size_t slot = 1; slot < command.args.size(); ++slot) {
          const Cell value = read(command.args[slot], frame);
          if (died()) {
            return false;
          }
          if (slot != 1) {
            output_ += ' ';
          }
          output_ += middleend::decimal_text(command.args[slot].type, value);
        }
        output_ += '\n';
        return true;
      }

      /// `PRIM print_char dst c`: appends the byte and nothing else.
      bool print_char(const Command& command, Frame frame) {
        const Cell value = read(command.args[1], frame);
        if (died()) {
          return false;
        }
        output_ += static_cast<char>(value & 0xFF);
        return true;
      }

      /// `PRIM read_char dst`: the next input byte as an `i32`, or -1 at the end
      /// of the input. An input that misbehaves is clamped rather than trusted.
      bool read_char(const Command& command, Frame frame) {
        const int byte = input_ ? input_->read_byte() : -1;
        write(command.args[0], byte < 0 ? static_cast<Cell>(-1) : static_cast<Cell>(byte) & 0xFF, frame);
        return !died();
      }

      /// `PRIM exit dst code`: the whole program stops now, with `code` as the
      /// exit code. Everything already written is delivered — unlike a trap,
      /// which the driver reports without the bytes — and no frame's `RET` runs.
      bool exit_(const Command& command, Frame frame) {
        const Cell value = read(command.args[1], frame);
        if (died()) {
          return false;
        }
        exiting_ = value;
        frames_.clear();
        return true;
      }

      /// `PRIM abort dst`: the same, with the fixed code 134 and no message of
      /// its own (`docs/primitive.md#abort`).
      bool abort_() {
        exiting_ = 134;
        frames_.clear();
        return true;
      }

      /// `PRIM alloc dst size`: \p size bytes from the heap, rounded up to whole
      /// cells (at least one). The result is a `ptr u8` into the heap region.
      bool alloc_(const Command& command, Frame frame) {
        const Cell bytes = read(command.args[1], frame);
        if (died()) {
          return false;
        }
        const Cell cells = bytes / 8 + (bytes % 8 != 0 ? 1 : 0);
        const Cell address = allocate(cells == 0 ? 1 : static_cast<std::size_t>(cells), site_of(frame));
        if (died()) {
          return false;
        }
        write(command.args[0], address, frame);
        return !died();
      }

      /// `PRIM free dst p`: returns \p p's heap block to the free list. `free(null)`
      /// does nothing; any other pointer that names no live block is a trap.
      bool free_(const Command& command, Frame frame) {
        const Cell address = read(command.args[1], frame);
        if (died()) {
          return false;
        }
        if (address == 0) {
          return true;
        }
        if (static_cast<Region>(address >> region_bits) != Region::Heap) {
          trap(Trap::InvalidPointer, site_of(frame));
          return false;
        }
        const auto live = heap_live_.find(static_cast<std::size_t>(address & index_mask_));
        if (live == heap_live_.end()) {
          trap(Trap::InvalidPointer, site_of(frame));
          return false;
        }
        heap_free_.push_back({live->first, live->second});
        heap_live_.erase(live);
        return true;
      }

      bool offset_(const Command& command, Frame frame) {
        const Cell address = read(command.args[1], frame);
        const Cell index = read(command.args[2], frame);
        if (died()) {
          return false;
        }
        const Cell result = offset_address(address, index, site_of(frame));
        if (died()) {
          return false;
        }
        write(command.args[0], result, frame);
        return !died();
      }

      /// Carves `cells` cells out of the heap: first from the first free block wide
      /// enough (splitting it), otherwise by growing the heap.
      Cell allocate(std::size_t cells, const Site& site) {
        for (std::size_t slot = 0; slot < heap_free_.size(); ++slot) {
          if (heap_free_[slot].second < cells) {
            continue;
          }
          const std::size_t start = heap_free_[slot].first;
          if (heap_free_[slot].second == cells) {
            heap_free_.erase(heap_free_.begin() + static_cast<std::ptrdiff_t>(slot));
          } else {
            heap_free_[slot].first += cells;
            heap_free_[slot].second -= cells;
          }
          heap_live_.emplace(start, cells);
          std::fill_n(heap_.begin() + static_cast<std::ptrdiff_t>(start), cells, Cell {0});
          return encode(Region::Heap, start);
        }
        if (cells > limits_.max_cells - heap_.size()) {
          trap(Trap::OutOfMemory, site);
          return 0;
        }
        const std::size_t start = heap_.size();
        heap_.resize(start + cells, Cell {0});
        heap_live_.emplace(start, cells);
        return encode(Region::Heap, start);
      }

      /// A branch test is a comparison whose nonzero result takes the edge.
      bool holds_(const Condition& condition, Frame frame, bool& holds) {
        const Cell left = read(condition.a, frame);
        const Cell right = read(condition.b, frame);
        if (died()) {
          return false;
        }
        const auto result = middleend::fold::binary(condition.cmp, condition.a.type, left, right);
        // A comparison always has an answer, and the validator keeps anything else
        // out of a branch test, so a failure here is the machine's own doing.
        if (result.failure) {
          break_("a branch test has no answer");
          return false;
        }
        holds = result.value.value_or(0) != 0;
        return true;
      }

      bool push(Subject subject, std::size_t index, std::size_t cells, const Site& site) {
        // A frame's size comes out of the IR, so it is asked about before it is
        // added to anything: a frame wider than the whole cell array can never fit,
        // and the addition below is the only place that could overflow.
        if (cells > limits_.max_cells) {
          trap(Trap::FrameTooLarge, site);
          return false;
        }
        if (frames_.size() >= limits_.max_depth || cells > limits_.max_cells - stack_.size()) {
          trap(Trap::StackExhausted, site);
          return false;
        }
        const std::size_t base = stack_.size();
        stack_.resize(base + cells, Cell {0});
        frames_.push_back(Frame {subject, index, 0, 0, base, cells, std::nullopt});
        return true;
      }

      const middleend::Cfg* cfg_of(const Frame& frame) noexcept {
        if (frame.subject == Subject::GlobalInit) {
          if (frame.index >= program_.globals.size()) {
            break_("an initializer names a global that does not exist");
            return nullptr;
          }
          return &program_.globals[frame.index].init;
        }
        if (frame.index >= program_.functions.size()) {
          break_("a frame names a function that does not exist");
          return nullptr;
        }
        return &program_.functions[frame.index].cfg;
      }

      /// The region a pointer's index belongs to, packed into the top two bits of a
      /// cell. Zero — no region, zero index — is the null pointer; every nonzero
      /// address names exactly one cell of exactly one region.
      enum class Region : Cell {
        Stack = 1,
        Global = 2,
        Heap = 3
      };

      static constexpr unsigned region_bits = 62;
      static constexpr Cell index_mask_ = (Cell {1} << region_bits) - 1;

      static Cell encode(Region region, std::size_t index) noexcept {
        return (static_cast<Cell>(region) << region_bits) | static_cast<Cell>(index);
      }

      /// Pointer arithmetic counts machine cells and never crosses the source's
      /// allocation. No backend address encoding is exposed to source code.
      Cell offset_address(Cell address, Cell distance, const Site& site) {
        const Region region = static_cast<Region>(address >> region_bits);
        const auto index = static_cast<std::size_t>(address & index_mask_);
        std::size_t remaining = 0;
        switch (region) {
          case Region::Stack:
            for (const auto& frame : frames_) {
              if (index >= frame.base && index - frame.base < frame.cells) {
                remaining = frame.cells - (index - frame.base);
                break;
              }
            }
            break;
          case Region::Global:
            if (index < globals_.size()) {
              remaining = 1;
            }
            break;
          case Region::Heap:
            if (auto block = heap_live_.upper_bound(index); block != heap_live_.begin()) {
              --block;
              if (index - block->first < block->second) {
                remaining = block->second - (index - block->first);
              }
            }
            break;
        }
        if (distance >= remaining || distance > index_mask_ - index) {
          trap(Trap::InvalidPointer, site);
          return 0;
        }
        return encode(region, index + static_cast<std::size_t>(distance));
      }

      /// The cell an address names, bounds-checked against its region. A null,
      /// misframed, or out-of-range address is a trap, never a wild read.
      Cell* deref(Cell address, const Site& site) {
        const Region region = static_cast<Region>(address >> region_bits);
        const std::size_t index = static_cast<std::size_t>(address & index_mask_);
        switch (region) {
          case Region::Stack:
            if (index < stack_.size()) {
              return &stack_[index];
            }
            break;
          case Region::Global:
            if (index < globals_.size()) {
              return &globals_[index];
            }
            break;
          case Region::Heap:
            if (auto block = heap_live_.upper_bound(index); block != heap_live_.begin()) {
              --block;
              if (index - block->first < block->second) {
                return &heap_[index];
              }
            }
            break;
        }
        trap(Trap::InvalidPointer, site);
        return nullptr;
      }

      /// A cell operand is a read of the frame the pointer is in; a global is a read
      /// of the process-wide array; an instant is its own value.
      Cell read(const Operand& operand, Frame frame) noexcept {
        switch (operand.kind) {
          case OperandKind::Instant: return operand.value;
          case OperandKind::VReg:
            if (operand.is_empty() || operand.value >= frame.cells) {
              break_("an operand names a cell outside its frame");
              return 0;
            }
            return stack_[frame.base + operand.value];
          case OperandKind::Global:
            if (operand.value >= globals_.size()) {
              break_("an operand names a global that does not exist");
              return 0;
            }
            return globals_[operand.value];
          case OperandKind::Fn: break_("an operand names a function where a value was read"); return 0;
        }
        return 0;
      }

      /// Only a vreg and a global are cells the machine can write, which is what the
      /// op table says: `MOV` writes either, every other op writes a vreg.
      void write(const Operand& operand, Cell value, Frame frame) noexcept {
        switch (operand.kind) {
          case OperandKind::VReg:
            if (operand.is_empty() || operand.value >= frame.cells) {
              break_("a result names a cell outside its frame");
              return;
            }
            stack_[frame.base + operand.value] = value;
            return;
          case OperandKind::Global:
            if (operand.value >= globals_.size()) {
              break_("a write names a global that does not exist");
              return;
            }
            globals_[operand.value] = value;
            return;
          default: break_("a result is not a cell of the machine"); return;
        }
      }

      Site site_of(const Frame& frame) const noexcept {
        return Site {frame.subject, frame.index, frame.block, frame.command, frames_.size()};
      }

      /// The first death is the one reported: a program that dies has no later state.
      void trap(Trap kind, const Site& site) {
        if (trap_) {
          return;
        }
        trap_ = Trapped {kind, site, {}};
        for (const Frame& frame : frames_) {
          trap_->stack.push_back(frame.index);
        }
      }

      void break_(std::string what) noexcept {
        if (broken_.empty()) {
          broken_ = std::move(what);
        }
      }

      bool died() const noexcept {
        return trap_.has_value() || !broken_.empty();
      }

      Failure failure() const {
        if (trap_) {
          return *trap_;
        }
        return Broken {broken_};
      }

      const Program& program_;
      const Limits& limits_;
      Input* input_;  ///< what `read_char` reads; null is an empty input
      std::vector<Cell> globals_;
      std::string output_;  ///< the bytes the output primitives wrote, for the `Outcome`
      std::vector<Cell> stack_;
      std::vector<Cell> heap_;  ///< the cells `alloc` has handed out; grows, reused via `heap_free_`
      /// Live heap blocks by their first cell, and the blocks `free` returned. A
      /// block's cell count is what `free` gives back and what a later `alloc` reuses.
      std::map<std::size_t, std::size_t> heap_live_;
      std::vector<std::pair<std::size_t, std::size_t>> heap_free_;
      std::vector<Frame> frames_;
      std::optional<Cell> returning_;  ///< the value the outermost `RET` carried
      std::optional<Cell> exiting_;  ///< the code `exit`/`abort` stopped the program with
      std::optional<Trapped> trap_;
      std::string broken_;
    };

  }  // namespace

  std::string_view to_string(Trap trap) noexcept {
    switch (trap) {
      case Trap::DivisionByZero: return "division by zero";
      case Trap::SignedOverflow: return "signed division overflowed";
      case Trap::StackExhausted: return "the stack outgrew the machine";
      case Trap::FrameTooLarge: return "a frame is larger than the machine allows";
      case Trap::OutOfMemory: return "the heap outgrew the machine";
      case Trap::InvalidPointer: return "a pointer named no live cell";
    }
    return "";
  }

  std::string to_string(const Site& site) {
    // The validator already knows how to name a place in a program; this is the same
    // place, plus the depth only the machine knows.
    const std::string place =
        middleend::location_of({site.subject, site.index, site.block, std::nullopt, std::string {}});
    return place + ", " + std::to_string(site.depth) + (site.depth == 1 ? " frame deep" : " frames deep");
  }

  std::string to_string(const Failure& failure) {
    if (const Broken* broken = std::get_if<Broken>(&failure)) {
      return broken->what;
    }
    if (const Trapped* trapped = std::get_if<Trapped>(&failure)) {
      std::string text = to_string(trapped->site) + ": " + std::string {to_string(trapped->trap)};
      if (trapped->stack.size() > 1) {
        text += ", called from ";
        for (std::size_t depth = 1; depth < trapped->stack.size(); ++depth) {
          if (depth > 1) {
            text += ", ";
          }
          text += "function " + std::to_string(trapped->stack[depth - 1]);
        }
      }
      return text;
    }
    const Rejected& rejected = std::get<Rejected>(failure);
    const std::string first =
        rejected.errors.empty() ? std::string {"no reason given"} : middleend::to_string(rejected.errors.front());
    return std::to_string(rejected.errors.size()) + " errors, the first: " + first;
  }

  std::expected<Outcome, Failure> run(const Program& program, const Limits& limits, Input* input) {
    return Machine {program, limits, input}.run();
  }

}  // namespace erax::interp
