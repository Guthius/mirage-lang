#pragma once

#include "module_resolver.hpp"
#include "symbol_table.hpp"

namespace sema {
    struct StructInfo {
        struct Field {
            std::string name;
            ResolvedType type;
            size_t offset = 0;
        };

        std::vector<Field> fields;
        size_t size = 0;
        size_t align = 1;
        bool is_packed = false;
        bool layout_done = false;
    };

    struct ProgramModule {
        SymbolTable symbols;
        std::vector<StructInfo> structs;
    };

    struct ProgramResult {
        std::unordered_map<std::string, ProgramModule> modules;
    };

    auto check_program(const ast::Program &program, DiagnosticEngine &diagnostics) -> ProgramResult;
}
