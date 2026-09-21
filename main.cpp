#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "frontend/tokenizer/Tokenizer.hpp"

using erax::frontend::to_string;
using erax::frontend::Token;
using erax::frontend::Tokenizer;

int main(int argc, char* argv[]) {
    std::string source;
    std::string filename = "<stdin>";

    if (argc > 1) {
        std::ifstream in{argv[1]};
        if (!in) {
            std::cerr << "cannot open file: " << argv[1] << '\n';
            return EXIT_FAILURE;
        }
        filename = argv[1];
        source.assign(std::istreambuf_iterator<char>{in}, {});
    } else {
        source.assign(std::istreambuf_iterator<char>{std::cin}, {});
    }

    auto result = Tokenizer{}.tokenize(source, filename);
    if (!result) {
        const auto& error = result.error();
        std::cerr << error.location.to_string() << ": error: " << error.message << '\n';
        return EXIT_FAILURE;
    }

    for (const Token& token : *result) {
        std::cout << to_string(token.type) << " '" << token.text << "' " << token.location.to_string() << '\n';
    }
    return EXIT_SUCCESS;
}