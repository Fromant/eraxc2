#include "Tokenizer.hpp"

#include <cctype>
#include <utility>

namespace erax::frontend {

  namespace {

    constexpr std::string_view operator_chars = "<=>|&^%*/~+-!?";

    constexpr std::string_view valid_escapes = "ntr0bfv'\"\\";

    bool is_digit(char c) {
      return c >= '0' && c <= '9';
    }

    bool is_hex_digit(char c) {
      return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool is_whitespace(char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    bool is_identifier_start(char c) {
      return c == '_' || c == '$' || std::isalpha(static_cast<unsigned char>(c)) != 0;
    }

    bool is_identifier_char(char c) {
      return c == '_' || c == '$' || std::isalnum(static_cast<unsigned char>(c)) != 0;
    }

    bool is_operator_char(char c) {
      return operator_chars.contains(c);
    }

    std::optional<TokenType> punctuation_type(char c) {
      switch (c) {
        case '(': return TokenType::LParen;
        case ')': return TokenType::RParen;
        case '[': return TokenType::LBracket;
        case ']': return TokenType::RBracket;
        case '{': return TokenType::LBrace;
        case '}': return TokenType::RBrace;
        case ';': return TokenType::Semicolon;
        case ':': return TokenType::Colon;
        case ',': return TokenType::Comma;
        case '.': return TokenType::Dot;
        default: return std::nullopt;
      }
    }

    class Scanner {
  public:
      Scanner(std::string_view source, std::string_view file_name) : source_(source) {
        loc_.file = file_name;
      }

      Tokenizer::result run() {
        while (!error_.has_value()) {
          if (!skip_trivia())
            break;
          if (at_end())
            break;

          begin_token();
          const char c = peek();

          if (c == '"') {
            scan_string();
            continue;
          }
          if (c == '\'') {
            scan_char();
            continue;
          }
          if (is_digit(c)) {
            scan_number();
            continue;
          }
          if (is_identifier_start(c)) {
            scan_identifier();
            continue;
          }
          if (is_operator_char(c)) {
            scan_operator();
            continue;
          }
          if (const auto type = punctuation_type(c); type.has_value()) {
            consume();
            tokens_.push_back(Token {*type, std::string(1, c), token_start_});
            continue;
          }
          fail(std::string("unexpected character '") + c + "'", loc_);
        }

        if (error_.has_value()) {
          return std::unexpected(*error_);
        }
        tokens_.push_back(Token {TokenType::EndOfFile, "", loc_});
        return tokens_;
      }

  private:
      bool at_end() const {
        return pos_ >= source_.size();
      }

      char peek(std::size_t ahead = 0) const {
        const std::size_t at = pos_ + ahead;
        return at < source_.size() ? source_[at] : '\0';
      }

      char consume() {
        const char c = source_[pos_++];
        ++loc_.offset;
        if (c == '\n') {
          ++loc_.row;
          loc_.col = 1;
        } else {
          ++loc_.col;
        }
        return c;
      }

      void begin_token() {
        begin_ = pos_;
        token_start_ = loc_;
      }

      Token make(TokenType type) const {
        return Token {type, std::string(source_.substr(begin_, pos_ - begin_)), token_start_};
      }

      Token make_quoted(TokenType type) const {
        return Token {type, std::string(source_.substr(begin_ + 1, pos_ - begin_ - 2)), token_start_};
      }

      bool skip_trivia() {
        while (!at_end()) {
          const char c = peek();
          if (is_whitespace(c)) {
            consume();
            continue;
          }
          if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n')
              consume();
            continue;
          }
          if (c == '/' && peek(1) == '*') {
            if (!skip_block_comment())
              return false;
            continue;
          }
          return true;
        }
        return true;
      }

      bool skip_block_comment() {
        const SourceLocation at = loc_;
        consume();
        consume();
        while (!at_end()) {
          if (peek() == '*' && peek(1) == '/') {
            consume();
            consume();
            return true;
          }
          consume();
        }
        return fail("unterminated block comment", at);
      }

      void scan_string() {
        const SourceLocation at = token_start_;
        consume();
        while (!at_end() && peek() != '"') {
          if (peek() == '\n') {
            fail("unterminated string literal", at);
            return;
          }
          if (peek() == '\\') {
            consume();
            if (!scan_escape(at))
              return;
            continue;
          }
          consume();
        }
        if (at_end()) {
          fail("unterminated string literal: expected '\"' before end of input", at);
          return;
        }
        consume();
        tokens_.push_back(make_quoted(TokenType::String));
      }

      void scan_char() {
        const SourceLocation at = token_start_;
        consume();
        if (at_end() || peek() == '\n') {
          fail("unterminated character literal", at);
          return;
        }
        if (peek() == '\'') {
          fail("empty character literal", at);
          return;
        }
        if (peek() == '\\') {
          consume();
          if (!scan_escape(at))
            return;
        } else {
          consume();
        }
        if (at_end() || peek() != '\'') {
          fail("character literal must contain exactly one character", at);
          return;
        }
        consume();
        tokens_.push_back(make_quoted(TokenType::Char));
      }

      bool scan_escape(SourceLocation at) {
        if (at_end()) {
          return fail("dangling backslash", at);
        }
        const char c = peek();
        if (valid_escapes.contains(c)) {
          consume();
          return true;
        }
        if (c == 'x' || c == 'u') {
          consume();
          const std::size_t before = pos_;
          while (!at_end() && is_hex_digit(peek()))
            consume();
          if (pos_ == before) {
            return fail(std::string("expected hexadecimal digits after \\") + c, at);
          }
          return true;
        }
        return fail(std::string("invalid escape sequence '\\") + c + "'", at);
      }

      void scan_number() {
        const char prefix = peek(1);
        if (peek() == '0' &&
            (prefix == 'x' || prefix == 'X' || prefix == 'b' || prefix == 'B' || prefix == 'o' || prefix == 'O')) {
          const int radix = (prefix == 'x' || prefix == 'X') ? 16 : (prefix == 'b' || prefix == 'B') ? 2 : 8;
          consume();
          consume();
          const std::size_t before = pos_;
          if (!consume_digits(radix))
            return;
          if (pos_ == before) {
            fail("radix literal requires at least one digit", token_start_);
            return;
          }
          // A digit the base does not have is a typo and is caught here, because a
          // suffix cannot start with a digit: `0b12` is `0b1` followed by `2`, and
          // reporting it as an off-base digit names the mistake better than letting
          // the parser see two literals.
          if (!at_end() && is_digit(peek())) {
            fail(std::string("invalid digit '") + peek() + "' in radix literal", loc_);
            return;
          }
          consume_type_suffix();
          tokens_.push_back(make(TokenType::Number));
          return;
        }

        if (!consume_digits(10))
          return;
        if (peek() == '.' && is_digit(peek(1))) {
          consume();
          if (!consume_digits(10))
            return;
        }
        if (peek() == 'e' || peek() == 'E') {
          std::size_t j = pos_ + 1;
          if (j < source_.size() && (source_[j] == '+' || source_[j] == '-'))
            ++j;
          if (j < source_.size() && is_digit(source_[j])) {
            consume();
            if (peek() == '+' || peek() == '-')
              consume();
            if (!consume_digits(10))
              return;
            consume_type_suffix();
            tokens_.push_back(make(TokenType::Number));
            return;
          }
          // `e` with no exponent after it is the start of a name -- `2e` is `2` and
          // `e` -- so the literal ends here and the suffix is not read from it.
          tokens_.push_back(make(TokenType::Number));
          return;
        }
        consume_type_suffix();
        tokens_.push_back(make(TokenType::Number));
      }

      /// Takes a type suffix off a numeric literal: `5i64`, `0xFFu8`, `1_000i32`.
      ///
      /// The suffix is the type name itself rather than a C-style `L`/`UL`, because
      /// erax has no `long` and the width is already in the name -- `5i64` says what
      /// `5ll` would, without inventing a second vocabulary. Which runs are *legal*
      /// suffixes is the parser's business: a keyword is an `Identifier` token the
      /// parser classifies, so the lexer only decides that `5i64` is one token and
      /// leaves what it means to the parser. A run that is not a legal suffix is
      /// consumed anyway, so `64ul` is one literal to reject instead of a literal
      /// followed by a name the parser would have to explain away.
      void consume_type_suffix() {
        if (at_end() || !is_identifier_start(peek()))
          return;
        while (!at_end() && is_identifier_char(peek()))
          consume();
      }

      bool consume_digits(int radix) {
        const auto digit = [radix](char c) {
          if (radix == 16)
            return is_hex_digit(c);
          if (radix == 2)
            return c == '0' || c == '1';
          if (radix == 8)
            return c >= '0' && c <= '7';
          return is_digit(c);
        };
        bool previous = pos_ > 0 && digit(source_[pos_ - 1]);
        while (!at_end()) {
          const char c = peek();
          if (c == '_') {
            if (!previous || !digit(peek(1))) {
              return fail("digit separator '_' must be placed between digits", loc_);
            }
            consume();
            previous = false;
            continue;
          }
          if (!digit(c))
            break;
          consume();
          previous = true;
        }
        return true;
      }

      void scan_operator() {
        while (!at_end() && is_operator_char(peek()))
          consume();
        tokens_.push_back(make(TokenType::Operator));
      }

      void scan_identifier() {
        while (!at_end() && is_identifier_char(peek()))
          consume();
        tokens_.push_back(make(TokenType::Identifier));
      }

      bool fail(std::string message, SourceLocation at) {
        error_ = LexError {std::move(message), at};
        return false;
      }

      std::string_view source_;
      std::size_t pos_ = 0;
      std::size_t begin_ = 0;
      SourceLocation loc_;
      SourceLocation token_start_;
      std::vector<Token> tokens_;
      std::optional<LexError> error_;
    };

  }  // namespace

  std::string SourceLocation::to_string() const {
    if (file.empty()) {
      return std::to_string(row) + ":" + std::to_string(col);
    }
    return file + ":" + std::to_string(row) + ":" + std::to_string(col);
  }

  bool Token::operator==(const Token& other) const {
    return type == other.type && text == other.text;
  }

  std::string Token::describe() const {
    return std::string(to_string(type)) + " '" + text + "' at " + location.to_string();
  }

  std::string_view to_string(TokenType type) {
    switch (type) {
      case TokenType::EndOfFile: return "end of file";
      case TokenType::Identifier: return "identifier";
      case TokenType::Number: return "number";
      case TokenType::String: return "string";
      case TokenType::Char: return "char";
      case TokenType::Operator: return "operator";
      case TokenType::Semicolon: return "';'";
      case TokenType::Colon: return "':'";
      case TokenType::Comma: return "','";
      case TokenType::Dot: return "'.'";
      case TokenType::LParen: return "'('";
      case TokenType::RParen: return "')'";
      case TokenType::LBracket: return "'['";
      case TokenType::RBracket: return "']'";
      case TokenType::LBrace: return "'{'";
      case TokenType::RBrace: return "'}'";
    }
    return "unknown";
  }

  Tokenizer::result Tokenizer::tokenize(std::string_view source, std::string_view file_name) const {
    return Scanner {source, file_name}.run();
  }

}  // namespace erax::frontend