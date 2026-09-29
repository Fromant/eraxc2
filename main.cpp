#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "frontend/parser/ProgramParser.hpp"
#include "frontend/tokenizer/Tokenizer.hpp"
#include "middleend/jir/Jir.hpp"

using erax::frontend::ParseError;
using erax::frontend::ProgramParser;
using erax::frontend::to_string;
using erax::frontend::Token;
using erax::frontend::Tokenizer;

namespace {

/// Reports an error the same way whatever produced it, as
/// `file:row:col: error: text`.
template <typename Error>
int report(const Error& error) {
    std::cerr << error.location.to_string() << ": error: " << error.message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char* argv[]) {
    bool print_jir = false;
    std::string filename = "<stdin>";
    std::string path;

    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--jir") == 0) {
            print_jir = true;
        } else if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
            std::cout << "usage: eraxc [--jir] [file]\n"
                         "  no arguments   tokenize the file, or stdin\n"
                         "  --jir          lower the program to IR and print it\n";
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
        std::ifstream in{path};
        if (!in) {
            std::cerr << "cannot open file: " << path << '\n';
            return EXIT_FAILURE;
        }
        filename = path;
        source.assign(std::istreambuf_iterator<char>{in}, {});
    } else {
        source.assign(std::istreambuf_iterator<char>{std::cin}, {});
    }

    auto tokens = Tokenizer{}.tokenize(source, filename);
    if (!tokens) return report(tokens.error());

    if (!print_jir) {
        for (const Token& token : *tokens) {
            std::cout << to_string(token.type) << " '" << token.text << "' " << token.location.to_string()
                      << '\n';
        }
        return EXIT_SUCCESS;
    }

    auto program = ProgramParser{*tokens}.parse();
    if (!program) return report(program.error());
    std::cout << erax::middleend::print(*program);
    return EXIT_SUCCESS;
}
