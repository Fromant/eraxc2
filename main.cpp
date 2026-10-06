#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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
  bool print_jir = false;
  bool execute = false;
  std::string filename = "<stdin>";
  std::string path;

  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    if (std::strcmp(argument, "--jir") == 0) {
      print_jir = true;
    } else if (std::strcmp(argument, "--run") == 0) {
      execute = true;
    } else if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      std::cout << "usage: eraxc [--jir | --run] [file]\n"
                   "  no arguments   tokenize the file, or stdin\n"
                   "  --jir          lower the program to IR and print it\n"
                   "  --run          run the program; the exit code is the entrypoint's return\n";
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
  if (!tokens)
    return report(tokens.error());

  if (!print_jir && !execute) {
    for (const Token& token : *tokens) {
      std::cout << to_string(token.type) << " '" << token.text << "' " << token.location.to_string() << '\n';
    }
    return EXIT_SUCCESS;
  }

  auto program = ProgramParser {*tokens}.parse();
  if (!program)
    return report(program.error());
  // `JIR.md#validator`: the program is checked before anything is done with it,
  // so `--jir` prints IR that a backend can be handed, and `--run` runs IR that
  // is already known to be sound.
  auto valid = erax::middleend::check(*program);
  if (!valid)
    return report(valid.error());
  if (execute) {
    auto outcome = erax::interp::run(*program);
    if (!outcome)
      return report(outcome.error());
    return report(*outcome, *program);
  }
  std::cout << erax::middleend::print(*program);
  return EXIT_SUCCESS;
}
