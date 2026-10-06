#include "Interpreter.hpp"

#include <optional>
#include <utility>

#include "middleend/jir/Fold.hpp"

namespace erax::interp {

    using middleend::BasicBlock;
    using middleend::Command;
    using middleend::Condition;
    using middleend::Op;
    using middleend::Operand;
    using middleend::OperandKind;
    using middleend::Program;
    using middleend::Subject;

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
                if (operand.kind != OperandKind::VReg || operand.is_empty())
                    return;
                if (!named || operand.value > highest) {
                    highest = static_cast<std::size_t>(operand.value);
                    named = true;
                }
            };
            for (const BasicBlock& block : cfg.blocks) {
                for (const Command& command : block.commands) {
                    for (const Operand& operand : command.args)
                        consider(operand);
                }
                if (!block.branch)
                    continue;
                consider(block.branch->condition.a);
                consider(block.branch->condition.b);
            }
            return named ? highest + 1 : 0;
        }

        /// The machine of `JIR.md#frames`: one array of cells for every frame, one frame
        /// pointer walking it, and the CFG saying where to go next.
        class Machine {
        public:
            Machine(const Program& program, const Limits& limits) noexcept :
                program_(program), limits_(limits), globals_(program.globals.size(), Cell {0}) {}

            std::expected<Outcome, Failure> run() {
                // Every operand is read through an index, so the machine asks the
                // validator first: a program it rejects never gets near the cell array.
                auto valid = middleend::check(program_);
                if (!valid)
                    return std::unexpected(Failure {Rejected {std::move(valid.error())}});

                for (std::size_t index = 0; index < program_.globals.size(); ++index) {
                    const middleend::Cfg& init = program_.globals[index].init;
                    if (init.blocks.empty())
                        continue;  // zeroed by the machine, nothing to run
                    if (!push(Subject::GlobalInit, index, cells_named(init),
                              Site {Subject::GlobalInit, index, 0, 0, 1})) {
                        return std::unexpected(failure());
                    }
                    if (!execute())
                        return std::unexpected(failure());
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
                if (!execute())
                    return std::unexpected(failure());
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
                    if (!cfg)
                        return false;
                    if (frame.block >= cfg->blocks.size()) {
                        break_("a frame is in a block that does not exist");
                        return false;
                    }
                    const BasicBlock& block = cfg->blocks[frame.block];
                    if (frame.command == block.commands.size()) {
                        if (!advance(frame))
                            return false;
                        continue;
                    }
                    const Command& command = block.commands[frame.command];
                    const Site site = site_of(frame);
                    // The command advances its block before it runs: `RET` and `CALL` then
                    // change the stack, and the loop reads whatever frame is on top next.
                    ++frames_.back().command;
                    if (!run_command(command, frame, site))
                        return false;
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
                    if (!holds_(block.branch->condition, frame, holds))
                        return false;
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
                    case Op::Print: return print(command, frame);
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
                        if (died())
                            return false;
                        return land(middleend::fold::unary(command.op, command.args[1].type, value), command.args[0],
                                    frame, site);
                    }
                    default: {
                        const Cell left = read(command.args[1], frame);
                        const Cell right = read(command.args[2], frame);
                        if (died())
                            return false;
                        // The operands share a type (the validator saw to that), so its
                        // signedness decides div, mod, and `SAR`.
                        return land(middleend::fold::binary(command.op, command.args[1].type, left, right),
                                    command.args[0], frame, site);
                    }
                }
            }

            /// The fold decides the trap: division and signed overflow are the two ways
            /// arithmetic has no answer, and both are the machine dying.
            bool land(const middleend::fold::Result& result, const Operand& destination, Frame frame,
                      const Site& site) {
                if (result.failure) {
                    trap(*result.failure == middleend::fold::Failure::DivisionByZero ? Trap::DivisionByZero :
                                                                                       Trap::SignedOverflow,
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
                    if (died())
                        return false;
                }
                stack_.resize(frame.base);
                frames_.pop_back();
                if (!carries)
                    return true;
                // The outermost `RET` is the entrypoint's, and its value is the exit code.
                if (frames_.empty())
                    returning_ = value;
                if (frame.result && !frames_.empty())
                    write(*frame.result, value, frames_.back());
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
                    if (died())
                        return false;
                }
                if (arguments.size() > function.frame_size) {
                    break_("a call passes more arguments than the callee's frame holds");
                    return false;
                }
                if (!push(Subject::Function, callee.index(), function.frame_size, site))
                    return false;
                const std::size_t base = frames_.back().base;
                frames_.back().result = command.args[0];
                for (std::size_t slot = 0; slot < arguments.size(); ++slot) {
                    stack_[base + slot] = arguments[slot];
                }
                return true;
            }

            /// `PRINT a0 …`: writes each value as a signed or unsigned decimal — the
            /// interpreter's `decimal_text`, which renders an `Instant` the same way —
            /// separated by single spaces, then a newline. A bare `PRINT` writes only
            /// the newline.
            bool print(const Command& command, Frame frame) {
                for (std::size_t slot = 0; slot < command.args.size(); ++slot) {
                    const Cell value = read(command.args[slot], frame);
                    if (died())
                        return false;
                    if (slot != 0)
                        output_ += ' ';
                    output_ += middleend::decimal_text(command.args[slot].type, value);
                }
                output_ += '\n';
                return true;
            }

            /// A branch test is a comparison whose nonzero result takes the edge.
            bool holds_(const Condition& condition, Frame frame, bool& holds) {
                const Cell left = read(condition.a, frame);
                const Cell right = read(condition.b, frame);
                if (died())
                    return false;
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
                if (trap_)
                    return;
                trap_ = Trapped {kind, site, {}};
                for (const Frame& frame : frames_)
                    trap_->stack.push_back(frame.index);
            }

            void break_(std::string what) noexcept {
                if (broken_.empty())
                    broken_ = std::move(what);
            }

            bool died() const noexcept {
                return trap_.has_value() || !broken_.empty();
            }

            Failure failure() const {
                if (trap_)
                    return *trap_;
                return Broken {broken_};
            }

            const Program& program_;
            const Limits& limits_;
            std::vector<Cell> globals_;
            std::string output_;  ///< the bytes `PRINT` wrote, for the `Outcome`
            std::vector<Cell> stack_;
            std::vector<Frame> frames_;
            std::optional<Cell> returning_;  ///< the value the outermost `RET` carried
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
        if (const Broken* broken = std::get_if<Broken>(&failure))
            return broken->what;
        if (const Trapped* trapped = std::get_if<Trapped>(&failure)) {
            std::string text = to_string(trapped->site) + ": " + std::string {to_string(trapped->trap)};
            if (trapped->stack.size() > 1) {
                text += ", called from ";
                for (std::size_t depth = 1; depth < trapped->stack.size(); ++depth) {
                    if (depth > 1)
                        text += ", ";
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

    std::expected<Outcome, Failure> run(const Program& program, const Limits& limits) {
        return Machine {program, limits}.run();
    }

}  // namespace erax::interp