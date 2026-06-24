#pragma once

#include <Compiler/SourceLocation.hpp>

#include <memory>
#include <variant>

namespace Ast {
    struct BlockStmt;
    struct ExprStmt;

    using Stmt = std::variant<
        std::unique_ptr<BlockStmt>,
        std::unique_ptr<ExprStmt>>;

    struct BlockStmt {
        SourceLocation Location;
    };

    struct ExprStmt {
        SourceLocation Location;
    };
}
