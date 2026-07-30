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
        bool is_variadic = false;             // true if the last param is native '...T'
        ResolvedType variadic_element_type{};  // T; only meaningful if is_variadic
        bool is_resolved = false;              // lazily/reentrantly resolved — see ensure_function_signature_resolved
        size_t required_params = 0;            // count of leading non-defaulted params; == params.size() if none are defaulted
        std::vector<bool> param_default_is_const; // parallel to params; meaningful only at i >= required_params
    };

    // True for a generic function TEMPLATE ('fn f[T: type](v: T)'). Such a symbol's
    // 'params'/'return_types' are deliberately never filled in: a template has no single
    // signature, only one per concrete instantiation (see GenericFunctionInstance). Anything
    // that reads those vectors — or calls ensure_function_signature_resolved — must check
    // this first and route to instantiate_generic_function instead.
    inline auto is_generic_function(const FunctionSymbol &sym) -> bool {
        return sym.decl != nullptr && !sym.decl->generic_params.empty();
    }

    struct ExtFunctionSymbol {
        const ast::ExtFunctionDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        std::optional<ResolvedType> return_type;
        bool is_pub = false;
        bool is_variadic = false;
        // Guards resolve_signatures_for_module's per-param/return-type resolution loop
        // (sema.cpp) against being run twice on the same symbol — needed once a bare
        // import can cause the SAME underlying decl to be visited via two different
        // symbol-table entries (the origin's own, and an alias's), unlike ordinary
        // (non-aliased) 'ext fn's, which this loop previously only ever visited once.
        bool is_resolved = false;
    };

    struct MacroSymbol {
        const ast::MacroDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        ResolvedType result_type;
        bool has_declared_result_type = false;
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
