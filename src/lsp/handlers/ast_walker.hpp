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
        // Match/switch arm variant patterns ('.Variant', '.Variant(&v)'). A pattern is NOT an
        // expression, so on_expr never sees one -- and match arms are where variants are most
        // used, so a visitor relying on on_expr alone finds almost none of their uses.
        // Literal patterns still arrive through on_expr, since those really are expressions.
        //
        // The enclosing match/switch operand comes with it: '.Variant' is contextual, so
        // resolving the name to a declaration needs the operand's type, and the pattern
        // carries no back-pointer to it.
        std::function<void(const ast::MatchExpr::VariantPattern &, const ast::Expr &operand)> on_pattern =
            [](const ast::MatchExpr::VariantPattern &, const ast::Expr &) {};

        // Invoked by walk_module_bodies immediately before each function/method BODY, naming
        // the declaration that body belongs to (exactly one pointer is non-null). Not invoked
        // by walk_expr/walk_stmt, which start from a node whose owner the caller already knows,
        // nor for a module-scope VarDecl initializer or a macro template - neither is a
        // function body.
        //
        // Exists because a whole-module walk otherwise has no idea which declaration any given
        // statement came from, and for a generic declaration that is precisely what selects the
        // ExprSideTables its expression types live in (see LocalLookupContext::template_exprs).
        // The alternative - re-deriving the enclosing decl per statement from the token stream
        // via find_enclosing_function - rebuilds a bracket index every time.
        std::function<void(const ast::FunctionDecl *, const ast::ImplDecl::Function *)> on_body_begin =
            [](const ast::FunctionDecl *, const ast::ImplDecl::Function *) {};
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
