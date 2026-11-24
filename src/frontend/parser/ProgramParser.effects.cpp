#include "ProgramParser.hpp"

#include <algorithm>

namespace erax::frontend {

  void ProgramParser::infer_function_effects() {
    struct Dependencies {
      bool direct = false;
      std::vector<std::size_t> functions;
      std::vector<const TemplateInfo*> templates;
    };
    std::vector<Dependencies> graph(effect_sources_.size());
    const auto& tokens = context_.cursor.tokens();
    for (std::size_t source_index = 0; source_index < effect_sources_.size(); ++source_index) {
      const auto& source = effect_sources_[source_index];
      auto& effect = graph[source_index];
      const bool mutable_record = std::ranges::any_of(source.params, [](const SemType& type) {
        return is_mutable_record_reference(type);
      });
      if (!mutable_record && !source.generic_mutable) {
        continue;
      }
      const auto saved_namespace = context_.set_namespace_path(source.ns_path);
      int depth = 0;
      for (std::size_t index = source.body; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.type == TokenType::LBrace) {
          ++depth;
        }
        if (token.type == TokenType::RBrace && --depth == 0) {
          break;
        }
        if (index + 1 >= tokens.size()) {
          break;
        }
        if ((token.text == "free" && tokens[index + 1].type == TokenType::LParen) ||
            (token.text == "move" && tokens[index + 1].text == "*")) {
          effect.direct = true;
        }
        if (token.text == "*" && index + 2 < tokens.size() && tokens[index + 2].text == "=") {
          // Replacing a value through an alias may end the old record's lifetime.
          effect.direct = true;
        }
        if (token.type != TokenType::Identifier) {
          continue;
        }
        if (index > source.body && tokens[index - 1].type == TokenType::Dot) {
          if (index >= 2 && tokens[index - 2].type != TokenType::Identifier &&
              tokens[index + 1].type == TokenType::LParen) {
            for (const auto& record : context_.records()) {
              if (const auto found = record->methods.find(token.text); found != record->methods.end()) {
                for (const auto& method : found->second) {
                  effect.direct |= method.invalidates_references;
                }
              }
            }
          }
          continue;
        }
        std::vector<std::string> parts {token.text};
        std::size_t end = index + 1;
        while (end + 1 < tokens.size() && tokens[end].type == TokenType::Dot &&
               tokens[end + 1].type == TokenType::Identifier) {
          parts.push_back(tokens[end + 1].text);
          end += 2;
        }
        if (parts.size() > 1 && end < tokens.size() && tokens[end].text == "=") {
          for (const auto& record : context_.records()) {
            if (std::ranges::any_of(record->fields, [&](const FieldInfo& field) {
                  return field.name == parts.back() && field.type.is_record();
                })) {
              effect.direct = true;
            }
          }
        }
        if (end >= tokens.size() || tokens[end].type != TokenType::LParen) {
          continue;
        }
        const std::vector<FunctionInfo>* functions = nullptr;
        const TemplateInfo* tmpl = nullptr;
        if (parts.size() == 1) {
          functions = context_.find_functions(parts.front());
          tmpl = context_.find_template(parts.front());
        } else {
          const std::vector<std::string> path(parts.begin(), parts.end() - 1);
          auto space = context_.resolve_namespace(path, token.location);
          if (space) {
            functions = context_.find_functions_in(**space, parts.back());
            tmpl = context_.find_template_in(**space, parts.back());
          }
        }
        if (functions || tmpl) {
          if (functions) {
            for (const auto& function : *functions) {
              effect.functions.push_back(function.id);
            }
          }
          if (tmpl) {
            effect.templates.push_back(tmpl);
          }
          continue;
        }
        if (parts.size() < 2) {
          continue;  // An undeclared call is diagnosed by lowering.
        }
        RecordInfo* receiver = nullptr;
        for (std::size_t parameter = 0; parameter < source.names.size() && parameter < source.params.size();
             ++parameter) {
          if (source.names[parameter] != parts.front()) {
            continue;
          }
          auto type = source.params[parameter];
          while (type.is_reference()) {
            type = *type.pointee;
          }
          if (type.is_record()) {
            receiver = type.record;
          }
        }
        const auto inspect = [&](const RecordInfo& record) {
          if (const auto found = record.methods.find(parts.back()); found != record.methods.end()) {
            for (const auto& method : found->second) {
              effect.direct |= method.invalidates_references;
              if (method.factory) {
                effect.direct |= std::ranges::any_of(method.function.params, [](const SemType& type) {
                  return is_mutable_record_reference(type);
                });
              }
            }
          }
        };
        if (receiver) {
          inspect(*receiver);
        } else {
          for (const auto& record : context_.records()) {
            inspect(*record);
          }
          // Type parameters or unresolved local aliases need a conservative summary.
          effect.direct |= source.generic_mutable;
        }
      }
      context_.set_namespace_path(saved_namespace);
    }
    bool changed;
    do {
      changed = false;
      for (std::size_t index = 0; index < effect_sources_.size(); ++index) {
        auto& source = effect_sources_[index];
        const auto& dependencies = graph[index];
        bool invalidates = dependencies.direct;
        for (const auto id : dependencies.functions) {
          if (const auto* callee = context_.function_by_id(id)) {
            invalidates |= callee->invalidates_references;
          }
        }
        for (const auto* callee : dependencies.templates) {
          invalidates |= callee->invalidates_references;
        }
        auto* concrete = source.tmpl ? nullptr : context_.function_by_id(source.id);
        bool& state = source.tmpl ? source.tmpl->invalidates_references : concrete->invalidates_references;
        if (state != invalidates) {
          state = invalidates;
          changed = true;
        }
      }
    } while (changed);
  }

}  // namespace erax::frontend
