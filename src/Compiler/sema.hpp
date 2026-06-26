#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"
#include "resolved_type.hpp"

#include <unordered_map>
#include <vector>

namespace sema {
    struct SemaResult {
        struct FunctionSignature {
            std::vector<ResolvedType> params;
            std::vector<ResolvedType> return_types;
        };

        std::unordered_map<const void *, ResolvedType> expr_types;
        std::vector<ResolvedType> pointer_pointees;
        std::unordered_map<const ast::FunctionDecl *, FunctionSignature> functions;
        bool ok = false;
    };

    inline auto get_expr_key(const ast::Expr &expr) -> const void * {
        return std::visit(
            [](const auto &ptr) -> const void * {
                return &ptr;
            },
            expr);
    }

    auto check(const std::vector<ast::Decl> &decls, DiagnosticEngine &diagnostics) -> SemaResult;

    /**
     * Resolves the type of the given type node.
     * @param type The type node
     * @param result The result of the semantic analysis
     * @return The resolved type
     */
    auto resolve_type(const ast::Type &type, SemaResult &result) -> ResolvedType;
}
