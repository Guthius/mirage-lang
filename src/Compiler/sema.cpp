#include "sema.hpp"

#include <format>

namespace sema {
    namespace {
        struct Local {
            ResolvedType type;
            bool is_mut = false;
        };

        using LocalMap = std::unordered_map<std::string, Local>;

        void check_stmt(const ast::Stmt &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns, DiagnosticEngine &diagnostics);
        auto check_expr(const ast::Expr &expr, LocalMap &locals, SemaResult &result, DiagnosticEngine &diagnostics, std::optional<ResolvedType> expected = std::nullopt) -> ResolvedType;

        struct StmtChecker {
            LocalMap &locals;
            SemaResult &result;
            const std::vector<ResolvedType> &expected_returns;
            DiagnosticEngine &diagnostics;

            void operator()(const std::unique_ptr<ast::BlockStmt> &stmt) {
                for (auto &inner_stmt : stmt->stmts) {
                    std::visit(*this, inner_stmt);
                }
            }

            void operator()(const std::unique_ptr<ast::IfStmt> &) const {
            }

            void operator()(const std::unique_ptr<ast::WhileStmt> &) const {
            }

            void operator()(const ast::ExprStmt &stmt) const {
                check_expr(stmt.expr, locals, result, diagnostics);
            }

            void operator()(const ast::VarDeclStmt &) const {
            }

            void operator()(const ast::ContinueStmt &) const {
            }

            void operator()(const ast::ReturnStmt &) const {
            }
        };

        struct ExprChecker {
            LocalMap &locals;
            SemaResult &result;
            std::optional<ResolvedType> expected;
            DiagnosticEngine &diagnostics;

            auto operator()(const ast::LiteralIntegerExpr &) const -> ResolvedType {
                if (expected && expected->is_integer()) {
                    return *expected;
                }

                return ResolvedType{
                    .kind = TypeKind::I32,
                };
            }

            auto operator()(const ast::LiteralFloatExpr &) const -> ResolvedType {
                if (expected && expected->is_float()) {
                    return *expected;
                }

                return ResolvedType{
                    .kind = TypeKind::F32,
                };
            }

            auto operator()(const ast::LiteralStringExpr &expr) const -> ResolvedType {
                diagnostics.report_error(DiagnosticStage::Sema, expr.location, "string literals are not supported yet");

                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const ast::LiteralBoolExpr &) const -> ResolvedType {
                return ResolvedType{
                    .kind = TypeKind::Bool,
                };
            }

            auto operator()(const ast::LiteralNilExpr &) const -> ResolvedType {
                return ResolvedType{
                    .kind = TypeKind::Anyptr,
                };
            }

            auto operator()(const ast::IdentExpr &expr) const -> ResolvedType {
                const auto it = locals.find(expr.name);
                if (it == locals.end()) {
                    // diagnostics_.report_error(
                    //     DiagnosticStage::Sema, expr.location,
                    //     std::format("unknown identifier '{}'", expr.name));

                    return ResolvedType{
                        .kind = TypeKind::Void,
                    };
                }

                return it->second.type;
            }

            auto operator()(const std::unique_ptr<ast::UnaryExpr> &expr) -> ResolvedType {
                switch (expr->op) {
                case ast::UnaryOp::Negate:
                    {
                        const auto operand_type = check_expr(expr->operand, locals, result, diagnostics);
                        if (!operand_type.is_integer()) {
                            diagnostics.report_error(
                                DiagnosticStage::Sema, expr->location,
                                "unary '-' requires an integer operand");
                        }

                        return operand_type;
                    }

                case ast::UnaryOp::LogicalNot:
                    {
                        check_expr(expr->operand, locals, result, diagnostics, ResolvedType{.kind = TypeKind::Bool});

                        return ResolvedType{
                            .kind = TypeKind::Bool,
                        };
                    }

                case ast::UnaryOp::BitwiseNot:
                    {
                        const auto operand_type = check_expr(expr->operand, locals, result, diagnostics);
                        if (!operand_type.is_integer()) {
                            diagnostics.report_error(
                                DiagnosticStage::Sema, expr->location,
                                "unary '~' requires an integer operand");
                        }

                        return operand_type;
                    }

                case ast::UnaryOp::AddressOf:
                    {
                        const auto operand_type = check_expr(expr->operand, locals, result, diagnostics);
                        const auto pointer_type = ResolvedType{
                            .kind = TypeKind::Pointer,
                            .pointee_index = static_cast<int>(result.pointer_pointees.size()),
                        };

                        result.pointer_pointees.push_back(operand_type);

                        return pointer_type;
                    }

                case ast::UnaryOp::Deref:
                    {
                        const auto operand_type = check_expr(expr->operand, locals, result, diagnostics);
                        if (operand_type.kind != TypeKind::Pointer) {
                            diagnostics.report_error(
                                DiagnosticStage::Sema, expr->location,
                                "cannot dereference a non-pointer value");

                            return ResolvedType{
                                .kind = TypeKind::Void,
                            };
                        }

                        return result.pointer_pointees[operand_type.pointee_index];
                    }
                }

                diagnostics.report_error(
                    DiagnosticStage::Sema, expr->location,
                    "invalid unary operator");

                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const std::unique_ptr<ast::BinaryExpr> &expr) const -> ResolvedType {
                const auto lhs_type = check_expr(expr->lhs, locals, result, diagnostics);
                const auto rhs_type = check_expr(expr->rhs, locals, result, diagnostics);

                switch (expr->op) {
                case ast::BinaryOp::Add:
                case ast::BinaryOp::Sub:
                case ast::BinaryOp::Mul:
                case ast::BinaryOp::Div:
                case ast::BinaryOp::Mod:
                case ast::BinaryOp::BitwiseAnd:
                case ast::BinaryOp::BitwiseOr:
                case ast::BinaryOp::BitwiseXor:
                case ast::BinaryOp::ShiftLeft:
                case ast::BinaryOp::ShiftRight:
                    if (lhs_type != rhs_type) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "operand type mismatch in binary expression");
                    }
                    return lhs_type;

                case ast::BinaryOp::Equal:
                case ast::BinaryOp::NotEqual:
                case ast::BinaryOp::Less:
                case ast::BinaryOp::Greater:
                case ast::BinaryOp::LessEqual:
                case ast::BinaryOp::GreaterEqual:
                    if (lhs_type != rhs_type) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "operand type mismatch in comparison");
                    }
                    return ResolvedType{
                        .kind = TypeKind::Bool,
                    };

                case ast::BinaryOp::LogicalAnd:
                case ast::BinaryOp::LogicalOr:
                    if (lhs_type.kind != TypeKind::Bool || rhs_type.kind != TypeKind::Bool) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "&&/|| require bool operands");
                    }
                    return ResolvedType{
                        .kind = TypeKind::Bool,
                    };
                }

                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const std::unique_ptr<ast::TernaryExpr> &expr) const -> ResolvedType {
                check_expr(expr->condition, locals, result, diagnostics, ResolvedType{.kind = TypeKind::Bool});

                const auto type_a = check_expr(expr->then_expr, locals, result, diagnostics);
                const auto type_b = check_expr(expr->else_expr, locals, result, diagnostics);

                if (type_a != type_b) {
                    diagnostics.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "ternary branches have different types");
                }

                return type_a;
            }

            auto operator()(const std::unique_ptr<ast::AssignExpr> &expr) const -> ResolvedType {
                ResolvedType target_type;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr->target)) {
                    const auto it = locals.find(ident->name);
                    if (it == locals.end()) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            std::format("unknown identifier '{}'t", ident->name));

                        return ResolvedType{
                            .kind = TypeKind::Void,
                        };
                    }

                    if (!it->second.is_mut) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            std::format("cannot assign to '{}': not declared mut", ident->name));
                    }

                    target_type = it->second.type;
                } else {
                    diagnostics.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "invalid assignment target");

                    return ResolvedType{
                        .kind = TypeKind::Void,
                    };
                }

                const auto value_type = check_expr(expr->value, locals, result, diagnostics);
                if (expr->op == ast::AssignOp::Assign) {
                    if (value_type != target_type) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "type mismatch in assignment");
                    }
                } else {
                    if (value_type != target_type) {
                        diagnostics.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "type mismatch in compound assignment");
                    }
                }

                return target_type;
            }

            auto operator()(const std::unique_ptr<ast::CallExpr> &expr) const -> ResolvedType {
                diagnostics.report_error(DiagnosticStage::Sema, expr->location, "call expressions are not supported yet");

                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const std::unique_ptr<ast::IncrDecrExpr> &expr) const -> ResolvedType {
                const auto operand_type = check_expr(expr->operand, locals, result, diagnostics);

                if (!operand_type.is_integer()) {
                    diagnostics.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "operand of increment/decrement expression must be an integer");
                }

                return operand_type;
            }

            auto operator()(const ast::ImportExpr &) const -> ResolvedType {
                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const std::unique_ptr<ast::SizeOfExpr> &) const -> ResolvedType {
                return ResolvedType{
                    .kind = TypeKind::USize,
                };
            }

            auto operator()(const std::unique_ptr<ast::CastExpr> &expr) const -> ResolvedType {
                return resolve_type(expr->as_type, result);
            }

            auto operator()(const std::unique_ptr<ast::MemberExpr> &expr) const -> ResolvedType {
                diagnostics.report_error(DiagnosticStage::Sema, expr->location, "member expressions are not supported yet");

                return ResolvedType{
                    .kind = TypeKind::Void,
                };
            }

            auto operator()(const std::unique_ptr<ast::DerefExpr> &expr) const -> ResolvedType {
                const auto [kind, pointee_index] = check_expr(expr->operand, locals, result, diagnostics);

                if (kind != TypeKind::Pointer) {
                    diagnostics.report_error(DiagnosticStage::Sema, expr->location, "cannot dereference non-pointer type");
                    return ResolvedType{
                        .kind = TypeKind::Void,
                    };
                }

                return result.pointer_pointees[pointee_index];
            }
        };

        void check_stmt(const ast::Stmt &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns, DiagnosticEngine &diagnostics) {
            std::visit(
                StmtChecker{
                    .locals = locals,
                    .result = result,
                    .expected_returns = expected_returns,
                    .diagnostics = diagnostics,
                },
                stmt);
        }

        auto check_expr(const ast::Expr &expr, LocalMap &locals, SemaResult &result, DiagnosticEngine &diagnostics, const std::optional<ResolvedType> expected) -> ResolvedType {
            const auto resolved_type = std::visit(
                ExprChecker{
                    .locals = locals,
                    .result = result,
                    .expected = expected,
                    .diagnostics = diagnostics,
                },
                expr);

            result.expr_types[get_expr_key(expr)] = resolved_type;

            return resolved_type;
        }

        void check_function_decl(const ast::FunctionDecl &decl, SemaResult &result, DiagnosticEngine &diagnostics) {
            LocalMap locals;

            SemaResult::FunctionSignature signature;
            for (auto &param : decl.params) {
                auto type = resolve_type(param.type, result);

                signature.params.push_back(type);

                locals[param.name] = Local{
                    .type = type,
                    .is_mut = param.is_mut,
                };
            }

            for (auto &return_type : decl.return_types) {
                signature.return_types.push_back(resolve_type(return_type, result));
            }

            result.functions[&decl] = signature;

            check_stmt(decl.body, locals, result, signature.return_types, diagnostics);
        }
    }

    auto check(const std::vector<ast::Decl> &decls, DiagnosticEngine &diagnostics) -> SemaResult {
        SemaResult result;

        for (auto &decl : decls) {
            const auto function_decl = std::get_if<ast::FunctionDecl>(&decl);
            if (function_decl == nullptr) {
                continue;
            }

            check_function_decl(*function_decl, result, diagnostics);
        }

        return result;
    }
}
