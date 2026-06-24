#pragma once

#include <Compiler/Ast/Expr.hpp>
#include <Compiler/SourceLocation.hpp>

#include <memory>
#include <variant>

namespace Ast {
    struct BlockStmt;
    struct AsmStmt;
    struct ExprStmt;

    using Stmt = std::variant<
        std::unique_ptr<BlockStmt>,
        std::unique_ptr<AsmStmt>,
        std::unique_ptr<ExprStmt>>;

    struct BlockStmt {
        std::vector<Stmt> Statements;
        SourceLocation Location;
    };

    struct AsmStmt {
        SourceLocation Location;
    };

    struct ExprStmt {
        Expr Expr;
        SourceLocation Location;
    };
}
