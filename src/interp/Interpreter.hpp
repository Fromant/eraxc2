#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "middleend/jir/Jir.hpp"
#include "middleend/jir/Validator.hpp"

/// The reference executor for JIR (`docs/middleend/JIR.md`): one array of cells, a
/// frame pointer walking it, and the CFG saying where to go next. It is the
/// machine the spec describes, so its results are what a backend is compared to.
namespace erax::interp {

    /// One cell of the machine — the `u64` holding the raw bit pattern of whatever
    /// type the IR gave it (`JIR.md#lexicon`). A value is never held in a host type,
    /// so narrowing and wrapping are the IR's business, not the host's.
    using Cell = std::uint64_t;

    /// Where the machine was when it died. The IR is nameless, so a site is the same
    /// triple `print` walks, plus how deep the stack was.
    struct Site {
        middleend::Subject subject = middleend::Subject::Function;
        std::size_t index = 0;  ///< the function or global it was in
        std::size_t block = 0;
        std::size_t command = 0;
        std::size_t depth = 0;  ///< frames on the stack, the initializer's included
    };

    std::string to_string(const Site& site);

    /// The ways a run ends without a result. The first two are the language's, spelled
    /// out in `JIR.md#runtime-semantics`; the last two are the machine's own limits,
    /// which the contract leaves to the executor.
    enum class Trap {
        DivisionByZero,  ///< `/` or `%` by zero
        SignedOverflow,  ///< `iMIN / -1` or `iMIN % -1`: a result with no bit pattern
        StackExhausted,  ///< deeper than `Limits::max_depth`, or wider than `Limits::max_cells`
        FrameTooLarge,  ///< one frame bigger than `Limits::max_cells`
    };

    std::string_view to_string(Trap trap) noexcept;

    /// A program rejected before it ran: `Validator` is the only source of these.
    struct Rejected {
        std::vector<middleend::ValidationError> errors;
    };

    /// A program that ran and died, with the call stack it died on.
    struct Trapped {
        Trap trap = Trap::DivisionByZero;
        Site site;
        std::vector<std::size_t> stack;  ///< the functions on the stack, entrypoint first
    };

    /// The machine reading something the validator forbids. Unreachable on a program
    /// `Validator` accepted; reported rather than indexed so that a bug is a message
    /// and never a wild read.
    struct Broken {
        std::string what;
    };

    /// Why `run` has no `Outcome`.
    using Failure = std::variant<Rejected, Trapped, Broken>;

    std::string to_string(const Failure& failure);

    /// What a finished program leaves behind: the final global state, the entrypoint's
    /// return code, and the bytes its `PRINT`s wrote. That triple is the whole
    /// observable result of v0.1, and it is what every backend is compared on.
    struct Outcome {
        std::vector<Cell> globals;  ///< one cell per global, raw bit patterns
        Cell code = 0;  ///< the entrypoint's `i32` return, raw bit pattern
        std::string output = {};  ///< what `PRINT` wrote, flattened to \n-separated lines

        friend bool operator==(const Outcome&, const Outcome&) = default;
    };

    /// The machine's limits. The IR fixes neither a frame's size nor how deep recursion
    /// may go, so an executor has to say where it stops instead of asking the host for
    /// memory it may not have. Both limits count the entrypoint's own frame.
    struct Limits {
        std::size_t max_depth = 4096;
        std::size_t max_cells = 1 << 22;
    };

    /// Runs a program to completion: every global initializer in declaration order,
    /// then the entrypoint. The program is validated first — the machine reads every
    /// operand through an index, and `Validator` is what says those indices are in
    /// range — so a malformed program comes back as `Rejected` rather than as a crash.
    std::expected<Outcome, Failure> run(const middleend::Program& program, const Limits& limits = {});

}  // namespace erax::interp