#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"

#include <unordered_map>

namespace ast {
    using Module = std::vector<Decl>;

    struct Program {
        std::unordered_map<std::string, Module> modules;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> module_imports;
        std::string root_module_path;
        bool ok = false;
    };

    auto canonicalize(const std::string &path) -> std::string;
    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics) -> Program;
}
