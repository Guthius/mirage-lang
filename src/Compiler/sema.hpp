#pragma once

#include "module_resolver.hpp"
#include "symbol_table.hpp"

namespace sema {
    struct ProgramModule {
        std::string path;
        SymbolTable symbols;
        std::vector<StructInfo> structs;
    };

    struct ProgramResult {
        std::unordered_map<std::string, ProgramModule> modules;
    };

    auto check_program(const ast::Program &program, DiagnosticEngine &diagnostics) -> ProgramResult;
}
