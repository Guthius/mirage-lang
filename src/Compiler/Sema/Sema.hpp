#pragma once

#include <Compiler/Ast/Decl.hpp>
#include <Compiler/Ast/Expr.hpp>
#include <Compiler/Diagnostics/DiagnosticEngine.hpp>
#include <Compiler/Sema/ResolvedType.hpp>

#include <unordered_map>
#include <vector>

namespace Sema {
    struct SemaResult {
        struct FunctionSignature {
            std::vector<ResolvedType> Params;
            std::vector<ResolvedType> ReturnTypes;
        };

        std::unordered_map<const void *, ResolvedType> expr_types;
        std::vector<ResolvedType> PointerPointees;
        std::unordered_map<const Ast::FunctionDecl *, FunctionSignature> Functions;
        bool Ok = false;
    };

    inline auto GetExprKey(const Ast::Expr &expr) -> const void * {
        return std::visit(
            [](const auto &ptr) -> const void * {
                return ptr.get();
            },
            expr);
    }

    auto Check(const std::vector<Ast::Decl> &decls, DiagnosticEngine &diagnostics) -> SemaResult;
}
