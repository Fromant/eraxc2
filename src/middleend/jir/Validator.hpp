#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "Jir.hpp"

namespace erax::middleend {

    /// Which part of the program an error is about. The IR is nameless, so a
    /// location is the triple `print` walks: the function or initializer, the block,
    /// and the command in it.
    enum class Subject : std::uint8_t {
        Program,
        GlobalInit,
        Function,
    };

    /// One thing wrong with a `Program`. Every check that fails is reported, in
    /// program order, because a backend wants the whole list rather than the first
    /// surprise.
    struct ValidationError {
        Subject subject = Subject::Program;
        std::size_t index = 0;  ///< the global or function id; unused at program level
        std::size_t block = 0;
        std::optional<std::size_t> command;  ///< none when the error is about the CFG rather than a command
        std::string message;
    };

    /// `function 1, block 2, command 0`, or the shorter form when the error is not
    /// about one command.
    std::string location_of(const ValidationError& error);

    /// `location_of` and the message, as one line: what the driver prints.
    std::string to_string(const ValidationError& error);

    /// The seven checks of `docs/middleend/JIR.md#validator`, in that order. An
    /// empty list is a program no backend can fault on.
    std::vector<ValidationError> validate(const Program& program);

    /// The same as the `std::expected` every fallible step returns: no errors is
    /// success, and the errors are the reason it failed.
    std::expected<void, std::vector<ValidationError>> check(const Program& program);

}  // namespace erax::middleend