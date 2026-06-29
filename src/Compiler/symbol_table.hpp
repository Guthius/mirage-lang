#pragma once

#include <unordered_map>

#include "ast.hpp"
#include "resolved_type.hpp"

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

    struct GlobalSymbol {
        ResolvedType type;
        bool is_mut = false;
        bool is_pub = false;
    };

    struct FunctionSymbol {
        std::vector<ResolvedType> params;
        std::vector<ResolvedType> return_types;
        bool is_pub = false;
    };

    struct ExtFunctionSymbol {
        std::vector<ResolvedType> params;
        std::vector<ResolvedType> return_types;
        bool is_pub = false;
    };

    struct MacroSymbol {
        std::vector<ResolvedType> params;
        ResolvedType result_type;
        bool is_pub = false;
    };

    struct ImportSymbol {
        std::string module_path;
        bool is_pub = false;
    };

    struct TypeSymbol {
        const ast::TypeDecl *decl = nullptr;
        std::optional<ResolvedType> resolved;
        bool is_pub = false;
        SourceLocation location;
    };

    using Symbol = std::variant<
        GlobalSymbol,
        FunctionSymbol,
        ExtFunctionSymbol,
        MacroSymbol,
        ImportSymbol,
        TypeSymbol>;

    using SymbolTable = std::unordered_map<std::string, Symbol>;
}
