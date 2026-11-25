#pragma once

#include <filesystem>

#include "frontend/parser/ProgramParser.hpp"

namespace erax::frontend {

  struct ModuleSources {
    std::vector<Token> tokens;
    ModuleImports imports;
  };

  // Collects the entry and its dependencies in dependency-first order.
  std::expected<ModuleSources, ParseError> load_modules(const std::filesystem::path& entry,
                                                        const std::vector<Token>& tokens,
                                                        std::vector<Token> standard_library);

}  // namespace erax::frontend
