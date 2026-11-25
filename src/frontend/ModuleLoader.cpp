#include "ModuleLoader.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace erax::frontend {

  namespace fs = std::filesystem;

  namespace {

    struct RequestedImport {
      std::string name;
      std::string alias;
      SourceLocation location;
    };

    std::expected<std::vector<RequestedImport>, ParseError> read_import_names(TokenCursor& cursor) {
      if (auto open = expect(cursor, TokenType::LBrace); !open) {
        return std::unexpected(open.error());
      }
      std::vector<RequestedImport> names;
      do {
        auto location = cursor.location();
        auto name = expect_identifier(cursor);
        if (!name) {
          return std::unexpected(name.error());
        }
        auto alias = name;
        if (cursor.at_keyword(Keyword::As)) {
          cursor.take();
          location = cursor.location();
          alias = expect_identifier(cursor);
          if (!alias) {
            return std::unexpected(alias.error());
          }
        }
        names.push_back({std::move(*name), std::move(*alias), location});
      } while (cursor.match(TokenType::Comma) && !cursor.at(TokenType::RBrace));
      if (auto close = expect(cursor, TokenType::RBrace); !close) {
        return std::unexpected(close.error());
      }
      return names;
    }

    struct Module {
      std::string name;
      bool complete = false;
    };

    class Loader {
  public:
      ModuleSources sources;

      std::expected<std::string, ParseError> visit(const fs::path& file, const SourceLocation& location,
                                                   const std::vector<Token>* entry_tokens = nullptr) {
        std::error_code error;
        auto resolved = file.empty() ? file : fs::canonical(file, error);
        if (error || (!file.empty() && !fs::is_regular_file(resolved, error))) {
          return std::unexpected(ParseError {"cannot open module '" + file.generic_string() + "'", location});
        }
        auto found = modules_.find(resolved);
        if (found == modules_.end() && !resolved.empty()) {
          // Canonical paths can still differ for the same file, for example
          // by case on Windows or when two paths are hard links.
          found = std::ranges::find_if(modules_, [&](const auto& known) {
            return !known.first.empty() && fs::equivalent(known.first, resolved, error);
          });
        }
        if (found != modules_.end()) {
          if (found->second.complete) {
            return found->second.name;
          }
          std::string chain;
          for (auto active = std::ranges::find(active_, found->first); active != active_.end(); ++active) {
            chain += active->generic_string() + " -> ";
          }
          return std::unexpected(ParseError {"import cycle: " + chain + found->first.generic_string(), location});
        }
        const auto name = "/" + std::to_string(modules_.size());
        auto& module = modules_.emplace(resolved, Module {name}).first->second;
        active_.push_back(resolved);

        std::vector<Token> dependency_tokens;
        if (!entry_tokens) {
          std::ifstream input {resolved, std::ios::binary};
          if (!input) {
            return std::unexpected(ParseError {"cannot open module '" + resolved.generic_string() + "'", location});
          }
          const std::string source {std::istreambuf_iterator<char> {input}, {}};
          auto tokenized = Tokenizer {}.tokenize(source, resolved.generic_string());
          if (!tokenized) {
            return std::unexpected(ParseError {tokenized.error().message, tokenized.error().location});
          }
          dependency_tokens = std::move(*tokenized);
        }
        const auto& tokens = entry_tokens ? *entry_tokens : dependency_tokens;
        TokenCursor cursor(tokens);
        std::vector<ModuleImport> imports;
        while (cursor.at_keyword(Keyword::Import)) {
          cursor.take();
          auto names = read_import_names(cursor);
          if (!names) {
            return std::unexpected(names.error());
          }
          if (!cursor.at(TokenType::Identifier) || cursor.current().text != "from") {
            return std::unexpected(ParseError {"expected 'from' after import names", cursor.location()});
          }
          cursor.take();
          auto path = expect(cursor, TokenType::String);
          if (!path) {
            return std::unexpected(path.error());
          }
          if (auto end = expect(cursor, TokenType::Semicolon); !end) {
            return std::unexpected(end.error());
          }
          const auto& specifier = (*path)->text;
          const auto& start = (*path)->location;
          if (!(specifier.starts_with("./") || specifier.starts_with("../")) || !specifier.ends_with(".erx") ||
              specifier.find_first_of("\\:") != std::string::npos || specifier.find('\0') != std::string::npos) {
            return std::unexpected(ParseError {"expected a relative .erx path using '/'", start});
          }
          if (resolved.empty()) {
            return std::unexpected(ParseError {"relative imports require a source file", start});
          }
          auto imported = visit(resolved.parent_path() / fs::path(specifier), start);
          if (!imported) {
            return std::unexpected(imported.error());
          }
          for (auto& binding : *names) {
            imports.push_back({*imported, std::move(binding.name), std::move(binding.alias), binding.location});
          }
        }

        // Keep synthetic scope delimiters outside the source's own braces.
        const auto body = cursor.index();
        auto& all = sources.tokens;
        if (entry_tokens && imports.empty()) {
          all.insert(all.end(), tokens.begin(), std::prev(tokens.end()));
          return name;
        }
        std::size_t braces = 0;
        while (!cursor.at_end()) {
          const auto& token = cursor.take();
          if (token.type == TokenType::LBrace) {
            ++braces;
          } else if (token.type == TokenType::RBrace) {
            if (braces == 0) {
              return std::unexpected(ParseError {"unexpected '}' at file scope", token.location});
            }
            --braces;
          }
        }
        if (braces != 0) {
          return std::unexpected(ParseError {"unterminated body", cursor.location()});
        }

        const auto& start = tokens[body].location;
        all.push_back({TokenType::Identifier, "namespace", start});
        all.push_back({TokenType::Identifier, name, start});
        all.push_back({TokenType::LBrace, "{", start});
        all.insert(all.end(), tokens.begin() + body, std::prev(tokens.end()));
        all.push_back({TokenType::RBrace, "}", tokens.back().location});
        sources.imports.emplace(name, std::move(imports));
        module.complete = true;
        active_.pop_back();
        return name;
      }

  private:
      std::map<fs::path, Module> modules_;
      std::vector<fs::path> active_;
    };

  }  // namespace

  std::expected<ModuleSources, ParseError> load_modules(const fs::path& entry, const std::vector<Token>& tokens,
                                                        std::vector<Token> standard_library) {
    Loader loader;
    standard_library.pop_back();
    loader.sources.tokens = std::move(standard_library);
    if (auto loaded = loader.visit(entry, tokens.front().location, &tokens); !loaded) {
      return std::unexpected(loaded.error());
    }
    loader.sources.tokens.push_back(tokens.back());
    return std::move(loader.sources);
  }

}  // namespace erax::frontend
