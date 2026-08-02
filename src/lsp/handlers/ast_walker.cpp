#include "ast_walker.hpp"

#include <type_traits>

namespace lsp::handlers {
    namespace {
        void walk_arm_pattern(const ast::MatchExpr::ArmPattern &pattern, const ast::Expr &operand, const AstVisitor &visitor) {
            if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&pattern)) {
                if (lit->expr) walk_expr(*lit->expr, visitor);
            } else if (const auto *variant = std::get_if<ast::MatchExpr::VariantPattern>(&pattern)) {
                visitor.on_pattern(*variant, operand);
            }
            // DefaultPattern ('_') names nothing.
        }

        void walk_match_arms(const std::vector<ast::MatchExpr::Arm> &arms, const ast::Expr &operand, const AstVisitor &visitor) {
            for (const auto &arm : arms) {
                walk_arm_pattern(arm.pattern, operand, visitor);
                walk_expr(arm.value, visitor);
            }
        }
    }

    void walk_named_type(const ast::NamedType &named, const AstVisitor &visitor) {
        visitor.on_type(named);
        // Generic args live on the LEAF segment of a dotted chain (see ast::NamedType);
        // the chain itself is the callback's to resolve, but the args are independent
        // type/expr occurrences of their own.
        for (const auto *segment = &named; segment; segment = segment->member.get()) {
            for (const auto &arg : segment->generic_args) {
                if (const auto *type_arg = std::get_if<ast::Type>(&arg->value)) {
                    walk_type(*type_arg, visitor);
                } else if (const auto *expr_arg = std::get_if<ast::Expr>(&arg->value)) {
                    walk_expr(*expr_arg, visitor);
                }
            }
        }
    }

    void walk_type(const ast::Type &type, const AstVisitor &visitor) {
        std::visit(
            [&]<typename T>(const T &node) {
                using U = std::decay_t<T>;
                if constexpr (std::is_same_v<U, ast::NamedType>) {
                    walk_named_type(node, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::PointerType>>) {
                    walk_type(node->pointee, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::StructType>>) {
                    for (const auto &field : node->fields) {
                        walk_type(field.type, visitor);
                        if (field.init) walk_expr(*field.init, visitor);
                    }
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ArrayType>>) {
                    walk_type(node->base_type, visitor);
                    if (node->size) walk_expr(*node->size, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SliceType>>) {
                    walk_type(node->base_type, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::EnumType>>) {
                    if (node->underlying_type) walk_type(*node->underlying_type, visitor);
                    for (const auto &field : node->fields) {
                        if (field.init) walk_expr(*field.init, visitor);
                    }
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::UnionType>>) {
                    for (const auto &member : node->members) walk_type(member.type, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::FunctionType>>) {
                    for (const auto &param : node->param_types) walk_type(param, visitor);
                    for (const auto &ret : node->return_types) walk_type(ret, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TraitType>>) {
                    for (const auto &composed : node->composed_traits) walk_named_type(composed, visitor);
                    for (const auto &m : node->methods) {
                        for (const auto &p : m.params) {
                            if (p.type) walk_type(*p.type, visitor);
                            if (p.default_value) walk_expr(*p.default_value, visitor);
                        }
                        for (const auto &ret : m.return_types) walk_type(ret, visitor);
                    }
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ErrorType>>) {
                    for (const auto &member : node->members) walk_named_type(member, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::OptionalErrorType>>) {
                    walk_type(node->inner, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BitsetType>>) {
                    walk_named_type(node->member_type, visitor);
                    if (node->storage_type) walk_type(*node->storage_type, visitor);
                }
                // monostate, BuiltinType - nothing named to visit.
            },
            type);
    }

    void walk_expr(const ast::Expr &expr, const AstVisitor &visitor) {
        visitor.on_expr(expr);

        std::visit(
            [&]<typename T>(const T &node) {
                using U = std::decay_t<T>;
                if constexpr (std::is_same_v<U, std::unique_ptr<ast::UnaryExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BinaryExpr>>) {
                    walk_expr(node->lhs, visitor);
                    walk_expr(node->rhs, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TernaryExpr>>) {
                    walk_expr(node->condition, visitor);
                    walk_expr(node->then_expr, visitor);
                    walk_expr(node->else_expr, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AssignExpr>>) {
                    walk_expr(node->target, visitor);
                    walk_expr(node->value, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CallExpr>>) {
                    walk_expr(node->callee, visitor);
                    for (const auto &arg : node->args) walk_expr(arg, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IncrDecrExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SizeOfExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AlignOfExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::LenExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::StackAllocExpr>>) {
                    walk_expr(node->size, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CastExpr>>) {
                    walk_expr(node->value, visitor);
                    walk_type(node->as_type, visitor);
                    if (node->len_expr) walk_expr(*node->len_expr, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                    walk_expr(node->operand, visitor);
                    for (const auto &arg : node->args) {
                        if (const auto *expr_arg = std::get_if<ast::Expr>(&arg.value)) {
                            walk_expr(*expr_arg, visitor);
                        } else if (const auto *type_arg = std::get_if<ast::Type>(&arg.value)) {
                            walk_type(*type_arg, visitor);
                        }
                    }
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SliceExpr>>) {
                    walk_expr(node->operand, visitor);
                    if (node->start) walk_expr(*node->start, visitor);
                    if (node->end) walk_expr(*node->end, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MemberExpr>>) {
                    walk_expr(node->object, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MatchExpr>>) {
                    walk_expr(node->operand, visitor);
                    walk_match_arms(node->arms, node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    std::visit(
                        [&]<typename V>(const V &alt) {
                            using W = std::decay_t<V>;
                            if constexpr (std::is_same_v<W, ast::StructExpr>) {
                                for (const auto &field : alt.fields) walk_expr(field.expr, visitor);
                            } else if constexpr (std::is_same_v<W, ast::ArrayExpr>) {
                                for (const auto &v : alt.values) walk_expr(v, visitor);
                            }
                            // EmptyExpr ('{}') has no nested content.
                        },
                        *node);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TaggedVariantExpr>>) {
                    for (const auto &field : node->payload.fields) walk_expr(field.expr, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TryExpr>>) {
                    walk_expr(node->call, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::RangeExpr>>) {
                    if (node->lower) walk_expr(*node->lower, visitor);
                    walk_expr(node->upper, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SpreadExpr>>) {
                    walk_expr(node->operand, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TypeExpr>>) {
                    walk_type(node->type, visitor);
                }
                // Literals, IdentExpr, ImportExpr, ImportBinExpr, IotaExpr,
                // DotIdentExpr, DefaultExpr, UndefinedExpr - no nested Expr to recurse into.
            },
            expr);
    }

    void walk_stmt(const ast::Stmt &stmt, const AstVisitor &visitor) {
        visitor.on_stmt(stmt);

        std::visit(
            [&]<typename T>(const T &node) {
                using U = std::decay_t<T>;
                if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                    for (const auto &s : node->stmts) walk_stmt(s, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                    walk_expr(node->condition, visitor);
                    walk_stmt(node->then_stmt, visitor);
                    if (node->else_stmt) walk_stmt(*node->else_stmt, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                    walk_expr(node->condition, visitor);
                    walk_stmt(node->body, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                    walk_expr(node->iterable, visitor);
                    walk_stmt(node->body, visitor);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                    walk_expr(node->operand, visitor);
                    for (const auto &arm : node->arms) {
                        walk_arm_pattern(arm.pattern, node->operand, visitor);
                        walk_stmt(arm.body, visitor);
                    }
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                    walk_stmt(node->body, visitor);
                } else if constexpr (std::is_same_v<U, ast::ExprStmt>) {
                    walk_expr(node.expr, visitor);
                } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                    if (node.type) walk_type(*node.type, visitor);
                    if (node.init) walk_expr(*node.init, visitor);
                } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                    walk_expr(node.init, visitor);
                } else if constexpr (std::is_same_v<U, ast::ReturnStmt>) {
                    for (const auto &e : node.return_values) walk_expr(e, visitor);
                } else if constexpr (std::is_same_v<U, ast::ReturnErrStmt>) {
                    walk_expr(node.error_value, visitor);
                } else if constexpr (std::is_same_v<U, ast::ReturnOkStmt>) {
                    for (const auto &e : node.return_values) walk_expr(e, visitor);
                }
                // ContinueStmt, BreakStmt declare no expressions.
            },
            stmt);
    }

    void walk_module_bodies(const ast::Module &module, const AstVisitor &visitor) {
        for (const auto &decl : ast::all_decls(module)) {
            if (const auto *fn = std::get_if<ast::FunctionDecl>(&decl)) {
                visitor.on_body_begin(fn, nullptr);
                walk_stmt(fn->body, visitor);
            } else if (const auto *var = std::get_if<ast::VarDecl>(&decl)) {
                if (var->init) walk_expr(*var->init, visitor);
            } else if (const auto *macro = std::get_if<ast::MacroDecl>(&decl)) {
                walk_expr(macro->expr_template, visitor);
            } else if (const auto *impl = std::get_if<ast::ImplDecl>(&decl)) {
                for (const auto &fn : impl->functions) {
                    visitor.on_body_begin(nullptr, &fn);
                    walk_stmt(fn.body, visitor);
                }
            } else if (const auto *trait_impl = std::get_if<ast::TraitImplDecl>(&decl)) {
                for (const auto &fn : trait_impl->functions) {
                    visitor.on_body_begin(nullptr, &fn);
                    walk_stmt(fn.body, visitor);
                }
            }
            // ExtFunctionDecl, TypeDecl have no expression content to walk.
        }
    }
}
