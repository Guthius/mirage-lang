#pragma once

#include <Compiler/Ast/Decl.hpp>
#include <Compiler/Ast/Expr.hpp>
#include <Compiler/Ast/Parser.hpp>
#include <Compiler/Ast/Stmt.hpp>
#include <Compiler/Ast/Type.hpp>
#include <Compiler/Diagnostics/DiagnosticEngine.hpp>

#include <optional>
#include <span>

namespace Ast {
    auto Parse(std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl>;

    auto ParseType(Parser &parser) -> Type;
    auto ParseDecl(Parser &parser, bool top_level) -> std::optional<Decl>;
    auto ParseStmt(Parser &parser) -> Stmt;
    auto ParseExpr(Parser &parser) -> Expr;
}
