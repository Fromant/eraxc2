#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace erax::frontend {

enum class TokenType {
    EndOfFile,
    Identifier,
    Number,
    String,
    Char,
    Operator,
    Semicolon,
    Colon,
    Comma,
    Dot,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
};

/// 1-indexed row/column plus 0-indexed byte offset into the source.
struct SourceLocation {
    std::string file;
    std::size_t row = 1;
    std::size_t col = 1;
    std::size_t offset = 0;

    std::string to_string() const;
};

struct Token {
    TokenType type = TokenType::EndOfFile;
    std::string text;
    SourceLocation location;

    bool operator==(const Token& other) const;

    std::string describe() const;
};

struct LexError {
    std::string message;
    SourceLocation location;
};

/// Human-readable name of a token type, e.g. "operator".
std::string_view to_string(TokenType type);

class Tokenizer {
public:
    using result = std::expected<std::vector<Token>, LexError>;

    /// Tokenizes \p source, appending a trailing EndOfFile token on success.
    result tokenize(std::string_view source, std::string_view file_name = {}) const;
};

}  // namespace erax::frontend