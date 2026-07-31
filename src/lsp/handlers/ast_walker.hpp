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

        // Named-type occurrences ('x: MyType', 'p: *mod.List[i32]', a cast's target, an
        // impl's target...). A type annotation is neither an Expr nor a Stmt, so without
        // this callback no type position is ever visited and Find References on a type
        // misses every annotation. Receives the OUTERMOST segment of a dotted chain
        // ('mod.Type' arrives once, as 'mod' with its member chain attached), letting the
        // callback resolve the chain the same way the expression side resolves
        // module-qualified members. Fired via walk_type/walk_named_type below.
        std::function<void(const ast::NamedType &)> on_type = [](const ast::NamedType &) {};

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

    // Structural traversal of a type annotation: fires on_type for every NamedType
    // reachable from `type` (including generic args, function-type params/returns,
    // array/slice/pointer bases, error members, a bitset's member enum, a trait's
    // composed traits), walk_expr for every Expr embedded in it (array sizes, field
    // defaults, generic value args). walk_named_type is the entry point for the
    // NamedType-shaped fields that are not stored as an ast::Type (an impl's target,
    // a trait-impl's names). Both are also invoked by walk_expr/walk_stmt for the
    // type positions inside expressions and statements (casts, sizeof-style type
    // operands, generic instantiation args, a var decl's annotation).
    void walk_type(const ast::Type &type, const AstVisitor &visitor);
    void walk_named_type(const ast::NamedType &named, const AstVisitor &visitor);

    // Walks every expression-bearing top-level decl in `module`: FunctionDecl and
    // ImplDecl::Function bodies (via both plain 'impl TYPE' and 'impl TRAIT for TYPE'
    // blocks), a top-level VarDecl's initializer (if any), and a MacroDecl's expression
    // template. ExtFunctionDecl and TypeDecl have no expression content to walk.
    void walk_module_bodies(const ast::Module &module, const AstVisitor &visitor);
}
