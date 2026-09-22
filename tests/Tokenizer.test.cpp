#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frontend/tokenizer/Tokenizer.hpp"

using namespace erax::frontend;

namespace {

using Tokens = std::vector<Token>;

Tokens lex_ok(std::string_view source, std::string_view file = {}) {
    auto result = Tokenizer{}.tokenize(source, file);
    REQUIRE(result.has_value());
    return *result;
}

std::string lex_error(std::string_view source, SourceLocation& location, std::string_view file = {}) {
    auto result = Tokenizer{}.tokenize(source, file);
    REQUIRE_FALSE(result.has_value());
    location = result.error().location;
    return result.error().message;
}

std::vector<std::string> texts(const Tokens& ts) {
    std::vector<std::string> out;
    for (const Token& t : ts) out.push_back(t.text);
    return out;
}

std::vector<TokenType> kinds(const Tokens& ts) {
    std::vector<TokenType> out;
    for (const Token& t : ts) out.push_back(t.type);
    return out;
}

bool single_lexeme(std::string_view source, TokenType type) {
    const auto ts = lex_ok(source);
    return ts.size() == 2 && ts[0].type == type && ts[0].text == source;
}

void check_kinds(std::vector<TokenType> actual, std::initializer_list<TokenType> expected) {
    REQUIRE(actual.size() == expected.size());
    std::size_t k = 0;
    for (const auto t : expected) {
        REQUIRE(actual[k] == t);
        ++k;
    }
}

}  // namespace

namespace Catch {

template <>
struct StringMaker<TokenType> {
    static std::string convert(TokenType type) { return std::string(erax::frontend::to_string(type)); }
};

template <>
struct StringMaker<Token> {
    static std::string convert(const Token& token) { return token.describe(); }
};

}  // namespace Catch

TEST_CASE("end of input") {
    const auto ts = lex_ok("");

    REQUIRE(ts.size() == 1);
    REQUIRE(ts[0].type == TokenType::EndOfFile);
    REQUIRE(ts[0].text.empty());
    REQUIRE(ts[0].location.row == 1);
    REQUIRE(ts[0].location.col == 1);
    REQUIRE(ts[0].location.offset == 0);
}

TEST_CASE("whitespace is skipped but still counted") {
    const auto ts = lex_ok("  \n\t ");

    REQUIRE(ts.size() == 1);
    REQUIRE(ts[0].type == TokenType::EndOfFile);
    REQUIRE(ts[0].location.row == 2);
    REQUIRE(ts[0].location.col == 3);
    REQUIRE(ts[0].location.offset == 5);
}

TEST_CASE("CRLF counts as a single newline") {
    const auto ts = lex_ok("\r\nx");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].text == "x");
    REQUIRE(ts[0].location.row == 2);
    REQUIRE(ts[0].location.col == 1);
}

TEST_CASE("a trailing newline moves the eof token to the next row") {
    const auto ts = lex_ok("a\n");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[1].type == TokenType::EndOfFile);
    REQUIRE(ts[1].location.row == 2);
    REQUIRE(ts[1].location.col == 1);
    REQUIRE(ts[1].location.offset == 2);
}

TEST_CASE("comment-only input yields only end-of-file") {
    const auto ts = lex_ok("// x\n/* y */\n ");

    REQUIRE(ts.size() == 1);
    REQUIRE(ts[0].type == TokenType::EndOfFile);
    REQUIRE(ts[0].location.row == 3);
    REQUIRE(ts[0].location.col == 2);
}

TEST_CASE("punctuation kinds") {
    const auto ts = lex_ok("; : , . ( ) [ ] { }");

    REQUIRE(kinds(ts) == std::vector<TokenType>{
                             TokenType::Semicolon, TokenType::Colon,  TokenType::Comma,   TokenType::Dot,
                             TokenType::LParen,    TokenType::RParen, TokenType::LBracket, TokenType::RBracket,
                             TokenType::LBrace,    TokenType::RBrace, TokenType::EndOfFile});
}

TEST_CASE("operator chars merge greedily") {
    const std::vector<std::string_view> samples = {
        "&&&", "====", "=>", "**", "??", "<<=", ">>>", "!=", "!==", "<=>", "%-+", "~~",
    };
    for (const auto s : samples) {
        INFO("operator sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Operator));
    }
}

TEST_CASE("keywords remain identifiers") {
    const auto ts = lex_ok("fn let return i32 string");

    REQUIRE(texts(ts) == std::vector<std::string>{"fn", "let", "return", "i32", "string", ""});
    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Identifier, TokenType::Identifier,
                            TokenType::Identifier, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("colon pairs are two tokens - no '::' operator") {
    const auto ts = lex_ok("::");

    REQUIRE(texts(ts) == std::vector<std::string>{":", ":", ""});
    check_kinds(kinds(ts), {TokenType::Colon, TokenType::Colon, TokenType::EndOfFile});
}

TEST_CASE("optional chaining dot is not merged into the operator") {
    const auto ts = lex_ok("a?.b");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", "?", ".", "b", ""});
    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Operator, TokenType::Dot,
                            TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("spread dots stay separate tokens") {
    const auto ts = lex_ok("...");

    check_kinds(kinds(ts), {TokenType::Dot, TokenType::Dot, TokenType::Dot, TokenType::EndOfFile});
}

TEST_CASE("member access keeps identifiers separate") {
    const auto ts = lex_ok("a.b");

    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Dot, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("a single slash is an operator, not a comment") {
    const auto ts = lex_ok("a / b");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", "/", "b", ""});
    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Operator, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("decimal integers") {
    const std::vector<std::string_view> samples = {"42", "0", "00", "007", "1_000", "12_34_56"};
    for (const auto s : samples) {
        INFO("integer sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Number));
    }
}

TEST_CASE("floats") {
    const std::vector<std::string_view> samples = {"3.14", "0.5", "0.05", "100.0"};
    for (const auto s : samples) {
        INFO("float sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Number));
    }
}

TEST_CASE("scientific notation") {
    const std::vector<std::string_view> samples = {"1e10", "1e-10", "1e+10", "1E+4", "1.5e-3", "0.5e2"};
    for (const auto s : samples) {
        INFO("exponent sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Number));
    }
}

TEST_CASE("exponent needs digits to be an exponent") {
    const auto ts = lex_ok("2e");

    REQUIRE(texts(ts) == std::vector<std::string>{"2", "e", ""});
    check_kinds(kinds(ts), {TokenType::Number, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("radix literals") {
    const std::vector<std::string_view> samples = {"0x1F", "0Xff", "0xFF_FF", "0b1010", "0b1_0", "0o17", "0x0"};
    for (const auto s : samples) {
        INFO("radix sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Number));
    }
}

TEST_CASE("radix literals need digits") {
    const std::vector<std::string_view> samples = {"0x", "0b", "0o", "0O"};
    for (const auto s : samples) {
        INFO("bad radix sample: " << s);
        SourceLocation location;
        const auto message = lex_error(s, location);
        REQUIRE(message.find("digit") != std::string::npos);
        REQUIRE(location.row == 1);
        REQUIRE(location.col == 1);
    }
}

TEST_CASE("radix literals reject off-base digits") {
    const std::vector<std::string_view> samples = {"0b12", "0b21", "0x1G", "0o98"};
    for (const auto s : samples) {
        INFO("off-base sample: " << s);
        SourceLocation location;
        REQUIRE(lex_error(s, location).find("digit") != std::string::npos);
    }
}

TEST_CASE("a trailing dot is member access, not a fraction") {
    const auto ts = lex_ok("1.");

    check_kinds(kinds(ts), {TokenType::Number, TokenType::Dot, TokenType::EndOfFile});
    REQUIRE(ts[0].text == "1");
}

TEST_CASE("a dot before a digit is a member access after an identifier") {
    const auto ts = lex_ok("a.5");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", ".", "5", ""});
    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Dot, TokenType::Number, TokenType::EndOfFile});
}

TEST_CASE("C-like number suffixes are gone") {
    const auto ts = lex_ok("64ul");

    REQUIRE(texts(ts) == std::vector<std::string>{"64", "ul", ""});
    check_kinds(kinds(ts), {TokenType::Number, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("digit separators must sit between digits") {
    const std::vector<std::string_view> samples = {
        "1_", "12_", "1__2", "1e10_", "1.5_", "0x1_", "0xFF_", "0b_10", "0x_1",
    };
    for (const auto s : samples) {
        INFO("separator sample: " << s);
        SourceLocation location;
        const auto message = lex_error(s, location);
        REQUIRE(message.find("separator") != std::string::npos);
    }
}

TEST_CASE("digit separators still work properly placed") {
    const std::vector<std::string_view> samples = {"1_000", "12_34_56", "1_0_0", "1e1_0", "1.5_0"};
    for (const auto s : samples) {
        INFO("separator sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Number));
    }
}

TEST_CASE("identifiers may contain digits, underscores and dollar signs") {
    const std::vector<std::string_view> samples = {"foo", "_x", "$el", "a$b", "bar1", "__"};
    for (const auto s : samples) {
        INFO("identifier sample: " << s);
        REQUIRE(single_lexeme(s, TokenType::Identifier));
    }
}

TEST_CASE("strings keep their contents without quotes") {
    const auto ts = lex_ok("\"hello world\"");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].type == TokenType::String);
    REQUIRE(ts[0].text == "hello world");
}

TEST_CASE("empty string literal") {
    const auto ts = lex_ok("\"\"");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].type == TokenType::String);
    REQUIRE(ts[0].text.empty());
}

TEST_CASE("string escapes are validated but kept raw") {
    const struct {
        std::string_view input;
        std::string_view expected;
    } samples[] = {
        {"\"a\\nb\"", "a\\nb"},
        {"\"a\\tb\"", "a\\tb"},
        {"\"a\\\\b\"", "a\\\\b"},
        {"\"a\\\"b\"", "a\\\"b"},
    };
    for (const auto& s : samples) {
        INFO("escape sample: " << s.input);
        const auto ts = lex_ok(s.input);
        REQUIRE(ts.size() == 2);
        REQUIRE(ts[0].type == TokenType::String);
        REQUIRE(ts[0].text == s.expected);
    }
}

TEST_CASE("hex and unicode escapes are kept raw") {
    const auto ts = lex_ok("\"\\x41\\u0041\"");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].text == "\\x41\\u0041");
}

TEST_CASE("escaped quotes do not end the string") {
    const auto ts = lex_ok("\"hello, \\\"world\\\"!\"");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].text == "hello, \\\"world\\\"!");
}

TEST_CASE("consecutive strings are separate tokens") {
    const auto ts = lex_ok("\"a\" \"b\"");

    REQUIRE(kinds(ts) == std::vector<TokenType>{TokenType::String, TokenType::String, TokenType::EndOfFile});
    REQUIRE(texts(ts) == std::vector<std::string>{"a", "b", ""});
}

TEST_CASE("a string may be followed directly by other tokens") {
    const auto ts = lex_ok("\"a\"+x");

    REQUIRE(kinds(ts) == std::vector<TokenType>{TokenType::String, TokenType::Operator, TokenType::Identifier,
                                                TokenType::EndOfFile});
    REQUIRE(texts(ts) == std::vector<std::string>{"a", "+", "x", ""});
}

TEST_CASE("invalid escapes are errors") {
    SourceLocation location;
    const auto message = lex_error("\"a\\qb\"", location);

    REQUIRE(message.find("escape") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("hex escape without digits is an error") {
    SourceLocation location;
    const auto message = lex_error("\"\\x\"", location);

    REQUIRE(message.find("hexadecimal") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("dangling backslash is an error") {
    SourceLocation location;
    const auto message = lex_error("\"\\", location);

    REQUIRE(message.find("backslash") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("unterminated string at end of input is an error") {
    SourceLocation location;
    const auto message = lex_error("\"abc", location);

    REQUIRE(message.find("string") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("a raw newline inside a string is an error") {
    SourceLocation location;
    const auto message = lex_error("\"ab\nc\"", location);

    REQUIRE(message.find("string") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("single quotes are character literals") {
    const auto ts = lex_ok("'a'");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].type == TokenType::Char);
    REQUIRE(ts[0].text == "a");
}

TEST_CASE("char literals accept escapes") {
    const auto ts = lex_ok("'\\n'");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].type == TokenType::Char);
    REQUIRE(ts[0].text == "\\n");
}

TEST_CASE("char literals accept escaped quotes and backslashes") {
    const struct {
        std::string_view input;
        std::string_view expected;
    } samples[] = {
        {"'\\''", "\\'"},
        {"'\\\\'", "\\\\"},
        {"'\\x41'", "\\x41"},
    };
    for (const auto& s : samples) {
        INFO("char sample: " << s.input);
        const auto ts = lex_ok(s.input);
        REQUIRE(ts.size() == 2);
        REQUIRE(ts[0].type == TokenType::Char);
        REQUIRE(ts[0].text == s.expected);
    }
}

TEST_CASE("empty char literal is an error") {
    SourceLocation location;
    const auto message = lex_error("''", location);

    REQUIRE(message.find("empty") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("multi-character literals are an error") {
    SourceLocation location;
    const auto message = lex_error("'ab'", location);

    REQUIRE(message.find("exactly") != std::string::npos);
    REQUIRE(location.row == 1);
    REQUIRE(location.col == 1);
}

TEST_CASE("unterminated char literal is an error") {
    SourceLocation location;
    REQUIRE(lex_error("'a", location).find("exactly") != std::string::npos);
}

TEST_CASE("line comments are skipped up to the newline") {
    const auto ts = lex_ok("a // hello world\nb");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", "b", ""});
    check_kinds(kinds(ts),
                {TokenType::Identifier, TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("adjacent block comments disappear") {
    const auto ts = lex_ok("a/**/b");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", "b", ""});
}

TEST_CASE("block comments may span lines and are counted") {
    const auto ts = lex_ok("/* one\n two */ x");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].text == "x");
    REQUIRE(ts[0].location.row == 2);
    REQUIRE(ts[0].location.col == 9);
}

TEST_CASE("token position after a single-line block comment") {
    const auto ts = lex_ok("/* c */x");

    REQUIRE(ts[0].text == "x");
    REQUIRE(ts[0].location.row == 1);
    REQUIRE(ts[0].location.col == 8);
}

TEST_CASE("slash-star separated by a space is not a comment") {
    const auto ts = lex_ok("a / * b");

    REQUIRE(texts(ts) == std::vector<std::string>{"a", "/", "*", "b", ""});
    check_kinds(kinds(ts), {TokenType::Identifier, TokenType::Operator, TokenType::Operator,
                            TokenType::Identifier, TokenType::EndOfFile});
}

TEST_CASE("unterminated block comments are errors") {
    const std::vector<std::string_view> samples = {"/* oops", "/*/"};
    for (const auto s : samples) {
        INFO("bad comment sample: " << s);
        SourceLocation location;
        const auto message = lex_error(s, location);
        REQUIRE(message.find("block") != std::string::npos);
        REQUIRE(location.row == 1);
        REQUIRE(location.col == 1);
    }
}

TEST_CASE("unknown characters are errors with their position") {
    const struct {
        std::string_view input;
        char bad;
        std::size_t row;
        std::size_t col;
        std::size_t offset;
    } samples[] = {
        {"a @ b", '@', 1, 3, 2},
        {"#define x", '#', 1, 1, 0},
        {"back`tick", '`', 1, 5, 4},
    };
    for (const auto& s : samples) {
        INFO("bad char sample: " << s.input);
        SourceLocation location;
        const auto message = lex_error(s.input, location);
        REQUIRE(message.find(s.bad) != std::string::npos);
        REQUIRE(location.row == s.row);
        REQUIRE(location.col == s.col);
        REQUIRE(location.offset == s.offset);
    }
}

TEST_CASE("error location may carry a source file name") {
    SourceLocation location;
    lex_error("a @ b", location, "prog.erax");

    REQUIRE(location.file == "prog.erax");
}

TEST_CASE("positions span multiple lines") {
    const auto ts = lex_ok("let a = 1;\nlet b = 2;");

    struct Expect {
        std::string_view text;
        std::size_t row;
        std::size_t col;
    };
    const std::vector<Expect> expected = {
        {"let", 1, 1}, {"a", 1, 5}, {"=", 1, 7}, {"1", 1, 9}, {";", 1, 10},
        {"let", 2, 1}, {"b", 2, 5}, {"=", 2, 7}, {"2", 2, 9}, {";", 2, 10},
    };

    REQUIRE(ts.size() == expected.size() + 1);
    for (std::size_t k = 0; k < expected.size(); ++k) {
        INFO("token " << k << " '" << ts[k].text << "'");
        REQUIRE(ts[k].text == expected[k].text);
        REQUIRE(ts[k].location.row == expected[k].row);
        REQUIRE(ts[k].location.col == expected[k].col);
    }

    REQUIRE(ts.back().type == TokenType::EndOfFile);
    REQUIRE(ts.back().location.row == 2);
    REQUIRE(ts.back().location.col == 11);
}

TEST_CASE("byte offsets count from zero") {
    const auto ts = lex_ok("a = 1");

    REQUIRE(ts[0].location.offset == 0);
    REQUIRE(ts[1].location.offset == 2);
    REQUIRE(ts[2].location.offset == 4);
}

TEST_CASE("string literals keep their position") {
    const auto ts = lex_ok("x = \"hi\"");

    REQUIRE(ts.size() == 4);
    REQUIRE(ts[2].type == TokenType::String);
    REQUIRE(ts[2].location.row == 1);
    REQUIRE(ts[2].location.col == 5);
}

TEST_CASE("char literals keep their position") {
    const auto ts = lex_ok("x = 'a'");

    REQUIRE(ts.size() == 4);
    REQUIRE(ts[2].type == TokenType::Char);
    REQUIRE(ts[2].location.row == 1);
    REQUIRE(ts[2].location.col == 5);
}

TEST_CASE("tokens remember the source file name") {
    const auto ts = lex_ok("a", "main.era");

    REQUIRE(ts[0].location.file == "main.era");
    REQUIRE(ts[1].location.file == "main.era");
}

TEST_CASE("position after leading whitespace and comments") {
    const auto ts = lex_ok("  // c\nx");

    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].text == "x");
    REQUIRE(ts[0].location.row == 2);
    REQUIRE(ts[0].location.col == 1);
}

TEST_CASE("a small program lexes as expected") {
    const auto ts = lex_ok("fn main(): i32 {\n  let x = \"hi\";\n  return 0;\n}");

    REQUIRE(kinds(ts) == std::vector<TokenType>{
                             TokenType::Identifier, TokenType::Identifier, TokenType::LParen,    TokenType::RParen,
                             TokenType::Colon,      TokenType::Identifier, TokenType::LBrace,   TokenType::Identifier,
                             TokenType::Identifier, TokenType::Operator,   TokenType::String,    TokenType::Semicolon,
                             TokenType::Identifier, TokenType::Number,     TokenType::Semicolon, TokenType::RBrace,
                             TokenType::EndOfFile});
    REQUIRE(texts(ts) == std::vector<std::string>{"fn",  "main", "(",   ")",   ":",  "i32", "{",
                                                  "let", "x",    "=",   "hi",  ";",  "return", "0",
                                                  ";",   "}",    ""});
}