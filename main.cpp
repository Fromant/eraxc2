#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "StdSource.hpp"
#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "interp/Interpreter.hpp"
#include "middleend/jir/Jir.hpp"
#include "middleend/jir/Validator.hpp"

using erax::frontend::ParseError;
using erax::frontend::ProgramParser;
using erax::frontend::to_string;
using erax::frontend::Token;
using erax::frontend::Tokenizer;
using erax::interp::Failure;
using erax::interp::Outcome;
using erax::middleend::ValidationError;

namespace {

  /// How far the driver takes the program. Executing is the default, and a flag
  /// only ever stops the pipeline earlier — so a backend (interpreter today, VM and
  /// AoT later) is a way of `Run`, not a mode of its own: the observable result a
  /// backend is compared on is what running produces.
  enum class Action {
    Run,  ///< parse, validate, execute (no flag, or `--run`)
    Jir,  ///< parse and validate, print the JIR, stop
    Tokenize,  ///< tokenize, print the tokens, stop
  };

  /// Reports an error the same way whatever produced it, as
  /// `file:row:col: error: text`.
  template<typename Error>
  int report(const Error& error) {
    std::cerr << error.location.to_string() << ": error: " << error.message << '\n';
    return EXIT_FAILURE;
  }

  /// A rejected program, one line per error. The IR is nameless, so a validation
  /// error names a function, a block, and a command instead of a source position.
  int report(const std::vector<ValidationError>& errors) {
    for (const ValidationError& error : errors) {
      std::cerr << "error: " << erax::middleend::to_string(error) << '\n';
    }
    return EXIT_FAILURE;
  }

  /// A program that died. Where it died is a place in the IR, not in the source,
  /// because that is all the IR has.
  int report(const Failure& failure) {
    if (const auto* rejected = std::get_if<erax::interp::Rejected>(&failure)) {
      return report(rejected->errors);
    }
    std::cerr << "error: " << erax::interp::to_string(failure) << '\n';
    return EXIT_FAILURE;
  }

  /// The program's own stdin, one byte at a time: `read_char` sees the stream the
  /// driver was given, not a copy of the source. A byte is 0-255, and end of input
  /// is -1 — the same contract a backend's stdin has to honor for the programs to
  /// be comparable (`docs/primitive.md#read_char`).
  ///
  /// When the source itself came from stdin there is nothing left to read, and the
  /// first `read_char` is an immediate end of input: the stream is one stream.
  class StdinInput : public erax::interp::Input {
public:
    int read_byte() override {
      char byte = 0;
      std::cin.read(&byte, 1);
      if (!std::cin) {
        return -1;
      }
      return static_cast<unsigned char>(byte);
    }
  };

  /// The program's whole observable result, printed as the driver sees it: the bytes
  /// the program itself wrote, then the final global state. The exit code is the
  /// program's own voice, so it does not get a line of its own.
  int report(const Outcome& outcome, const erax::middleend::Program& program) {
    std::cout << outcome.output;
    for (std::size_t index = 0; index < outcome.globals.size(); ++index) {
      std::cout << "global " << index << ": " << erax::middleend::to_string(program.globals[index].type) << " 0x"
                << std::hex << outcome.globals[index] << std::dec << '\n';
    }
    // The code is an `i32` cell, so the exit code is the value read back as one.
    return static_cast<int>(static_cast<std::uint32_t>(outcome.code));
  }

}  // namespace

int main(int argc, char* argv[]) {
  Action action = Action::Run;
  bool action_chosen = false;
  std::string filename = "<stdin>";
  std::string path;

  // `--run` is the action the no-flag case takes, so it is listed mostly so the
  // help can say so. Two different flags are a conflict, not a compound.
  auto action_named = [](const char* argument) -> std::optional<Action> {
    if (std::strcmp(argument, "--tokenize") == 0) {
      return Action::Tokenize;
    }
    if (std::strcmp(argument, "--jir") == 0) {
      return Action::Jir;
    }
    if (std::strcmp(argument, "--run") == 0) {
      return Action::Run;
    }
    return std::nullopt;
  };

  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    if (const auto chosen = action_named(argument)) {
      if (action_chosen && *chosen != action) {
        std::cerr << "conflicting modes: pick one of --tokenize, --jir, --run\n";
        return EXIT_FAILURE;
      }
      action = *chosen;
      action_chosen = true;
    } else if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      std::cout << "usage: eraxc [--tokenize | --jir | --run] [file]\n"
                   "  no flags   run the program: the exit code is the entrypoint's return\n"
                   "  --run      the no-flag behavior, written out\n"
                   "  --jir      stop after lowering: print the validated JIR\n"
                   "  --tokenize stop after the lexer: print the token list\n";
      return EXIT_SUCCESS;
    } else if (path.empty()) {
      path = argument;
    } else {
      std::cerr << "unexpected argument: " << argument << '\n';
      return EXIT_FAILURE;
    }
  }

  std::string source;
  if (!path.empty()) {
    std::ifstream in {path};
    if (!in) {
      std::cerr << "cannot open file: " << path << '\n';
      return EXIT_FAILURE;
    }
    filename = path;
    source.assign(std::istreambuf_iterator<char> {in}, {});
  } else {
    source.assign(std::istreambuf_iterator<char> {std::cin}, {});
  }

  auto tokens = Tokenizer {}.tokenize(source, filename);
  if (!tokens) {
    return report(tokens.error());
  }

  if (action == Action::Tokenize) {
    // The lexer's view is the user's file alone: auto-import is a front-of-parse
    // step, so what tokenization shows is what was written (docs/std.md).
    for (const Token& token : *tokens) {
      std::cout << to_string(token.type) << " '" << token.text << "' " << token.location.to_string() << '\n';
    }
    return EXIT_SUCCESS;
  }

  // Auto-import: std is parsed before the file it was given, so its declarations
  // (in `namespace std`) are part of the program and every program may call them
  // (`docs/std.md`). Its tokens carry the name `std.erx`, so an error inside the
  // library says so.
  auto imported = Tokenizer {}.tokenize(erax::std_source, "std.erx");
  if (!imported) {
    return report(imported.error());
  }
  std::vector<Token> all;
  all.reserve(imported->size() + tokens->size());
  // The trailing end-of-file of each half would stop the parser in the middle,
  // so only std's copy goes; the user's ends the stream.
  all.insert(all.end(), imported->begin(), std::prev(imported->end()));
  all.insert(all.end(), tokens->begin(), tokens->end());

  auto program = ProgramParser {all}.parse();
  if (!program) {
    return report(program.error());
  }
  // `JIR.md#validator`: the program is checked before anything is done with it,
  // so `--jir` prints IR that a backend can be handed, and running executes IR
  // that is already known to be sound. A new backend plugs in next to the
  // interpreter, after the same check.
  auto valid = erax::middleend::check(*program);
  if (!valid) {
    return report(valid.error());
  }
  if (action == Action::Jir) {
    std::cout << erax::middleend::print(*program);
    return EXIT_SUCCESS;
  }
  StdinInput input;
  auto outcome = erax::interp::run(*program, {}, &input);
  if (!outcome) {
    return report(outcome.error());
  }
  return report(*outcome, *program);
}
