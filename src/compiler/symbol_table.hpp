#pragma once

#include <unordered_map>

#include "ast.hpp"
#include "resolved_type.hpp"

namespace sema {
    struct GlobalSymbol {
        const ast::VarDecl *decl = nullptr;
        ResolvedType type;
        bool is_mut = false;
        bool is_pub = false;
        bool is_resolved = false;
    };

    struct FunctionSymbol {
        const ast::FunctionDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        std::vector<ResolvedType> return_types;
        bool is_pub = false;
    };

    struct ExtFunctionSymbol {
        const ast::ExtFunctionDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        std::optional<ResolvedType> return_type;
        bool is_pub = false;
        bool is_variadic = false;
    };

    struct MacroSymbol {
        const ast::MacroDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        ResolvedType result_type;
        bool is_pub = false;
        bool is_resolved = false;
    };

    struct ImportSymbol {
        const ast::ImportExpr *expr = nullptr;
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
