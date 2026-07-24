#pragma once

#include "compiler/ast.hpp"
#include "compiler/module_resolver.hpp"

#include <functional>

namespace lsp::handlers {
    // Callbacks invoked by walk_expr/walk_stmt/walk_module_bodies for every node visited
    // (pre-order: a node is visited before its children). Either callback may be left
    // default (a no-op) if a caller only cares about one node kind.
    struct AstVisitor {
        std::function<void(const ast::Expr &)> on_expr = [](const ast::Expr &) {};
        std::function<void(const ast::Stmt &)> on_stmt = [](const ast::Stmt &) {};
    };

    // Generic traversal over every Expr/Stmt node reachable from `expr`/`stmt`/a whole
    // module's function and method bodies. This is the one shared walker Find References,
    // Call Hierarchy's call-site index, Semantic Tokens' identifier classification, and
    // Inlay Hints all build on, instead of each hand-rolling their own copy of the
    // recursive std::visit dispatch that common.cpp's find_expr_by_location uses for its
    // narrower single-location search.
    void walk_expr(const ast::Expr &expr, const AstVisitor &visitor);
    void walk_stmt(const ast::Stmt &stmt, const AstVisitor &visitor);

    // Walks every expression-bearing top-level decl in `module`: FunctionDecl and
    // ImplDecl::Function bodies (via both plain 'impl TYPE' and 'impl TRAIT for TYPE'
    // blocks), a top-level VarDecl's initializer (if any), and a MacroDecl's expression
    // template. ExtFunctionDecl and TypeDecl have no expression content to walk.
    void walk_module_bodies(const ast::Module &module, const AstVisitor &visitor);
}
