#include "Sema.hpp"

#include <format>

namespace Sema {
    namespace {
        namespace Types {
            constexpr auto Bool = ResolvedType{.Kind = TypeKind::Bool};
            constexpr auto Void = ResolvedType{.Kind = TypeKind::Void};
            constexpr auto Anyptr = ResolvedType{.Kind = TypeKind::Anyptr};
        }

        struct TypeChecker {
            struct LocalVar {
                ResolvedType Type;
                bool IsMutable = false;
            };

            using LocalMap = std::unordered_map<std::string, LocalVar>;

            DiagnosticEngine &diagnostics_;

            explicit TypeChecker(DiagnosticEngine &diagnostics) : diagnostics_(diagnostics) {
            }

            auto Check(const std::vector<ast::Decl> &decls) -> SemaResult {
                SemaResult result;

                for (auto &decl : decls) {
                    const auto function_decl = std::get_if<ast::FunctionDecl>(&decl);
                    if (function_decl == nullptr) {
                        continue;
                    }

                    CheckFunction(*function_decl, result);
                }

                return result;
            }

            void CheckFunction(const ast::FunctionDecl &decl, SemaResult &result) {
                LocalMap locals;

                SemaResult::FunctionSignature signature;
                for (auto &param : decl.params) {
                    auto type = resolve_type(param.type, result);

                    signature.Params.push_back(type);

                    locals[param.name] = LocalVar{
                        .Type = type,
                        .IsMutable = param.is_mut,
                    };
                }

                for (auto &return_type : decl.return_types) {
                    signature.ReturnTypes.push_back(resolve_type(return_type, result));
                }

                result.Functions[&decl] = signature;
            }

            void CheckStatement(const std::unique_ptr<ast::BlockStmt> &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                auto inner = locals;

                for (auto &inner_stmt : stmt->stmts) {
                    CheckStatement(inner_stmt, inner, result, expected_returns);
                }
            }

            void CheckStatement(const std::unique_ptr<ast::ExprStmt> &expr_stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                CheckExpression(expr_stmt->expr, locals, result);
            }

            void CheckStatement(const ast::Stmt &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                std::visit(
                    [&](const auto &v) {
                        CheckStatement(v, locals, result, expected_returns);
                    },
                    stmt);
            }

            auto CheckExpression(const std::unique_ptr<ast::LiteralIntegerExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                if (expected && expected->IsInteger()) {
                    return *expected;
                }

                return ResolvedType{.Kind = TypeKind::I32};
            }

            auto CheckExpression(const std::unique_ptr<ast::LiteralFloatExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                if (expected && expected->IsFloat()) {
                    return *expected;
                }

                return ResolvedType{.Kind = TypeKind::F32};
            }

            auto CheckExpression(const std::unique_ptr<ast::LiteralStringExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                diagnostics_.report_error(DiagnosticStage::Sema, expr->location, "string literals are not supported yet");

                return Types::Void;
            }

            auto CheckExpression(const std::unique_ptr<ast::LiteralBoolExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return Types::Bool;
            }

            auto CheckExpression(const std::unique_ptr<ast::LiteralNilExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return Types::Anyptr;
            }

            auto CheckExpression(const std::unique_ptr<ast::IdentExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                const auto it = locals.find(expr->name);
                if (it == locals.end()) {
                    diagnostics_.report_error(
                        DiagnosticStage::Sema, expr->location,
                        std::format("unknown identifier '{}'", expr->name));

                    return Types::Void;
                }

                return it->second.Type;
            }

            auto CheckExpression(const std::unique_ptr<ast::UnaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckUnary(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<ast::BinaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckBinary(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<ast::TernaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                CheckExpression(expr->condition, locals, result, ResolvedType{.Kind = TypeKind::Bool});

                const auto type_a = CheckExpression(expr->then_expr, locals, result);
                const auto type_b = CheckExpression(expr->else_expr, locals, result);

                if (type_a != type_b) {
                    diagnostics_.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "ternary branches have different types");
                }

                return type_a;
            }

            auto CheckExpression(const std::unique_ptr<ast::AssignExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckAssign(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<ast::CallExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                diagnostics_.report_error(DiagnosticStage::Sema, expr->location, "call expressions are not supported yet");

                return Types::Void;
            }

            auto CheckExpression(const std::unique_ptr<ast::IncrDecrExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                const auto operand_type = CheckExpression(expr->operand, locals, result);

                if (!operand_type.IsInteger()) {
                    diagnostics_.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "operand of increment/decrement expression must be an integer");
                }

                return operand_type;
            }

            auto CheckExpression(const ast::Expr &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected = std::nullopt) -> ResolvedType {
                const auto resolved_type = std::visit(
                    [&](const auto &v) {
                        return CheckExpression(v, locals, result, expected);
                    },
                    expr);

                result.expr_types[GetExprKey(expr)] = resolved_type;

                return resolved_type;
            }

            auto CheckUnary(const std::unique_ptr<ast::UnaryExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                switch (expr->op) {
                case ast::UnaryOp::Negate:
                    {
                        const auto operand_type = CheckExpression(expr->operand, locals, result);
                        if (!operand_type.IsInteger()) {
                            diagnostics_.report_error(DiagnosticStage::Sema, expr->location, "unary '-' requires an integer operand");
                        }

                        return operand_type;
                    }

                case ast::UnaryOp::LogicalNot:
                    {
                        CheckExpression(expr->operand, locals, result, Types::Bool);

                        return Types::Bool;
                    }

                case ast::UnaryOp::BitwiseNot:
                    {
                        const auto operand_type = CheckExpression(expr->operand, locals, result);
                        if (!operand_type.IsInteger()) {
                            diagnostics_.report_error(DiagnosticStage::Sema, expr->location, "unary '~' requires an integer operand");
                        }

                        return operand_type;
                    }

                case ast::UnaryOp::AddressOf:
                    {
                        const auto operand_type = CheckExpression(expr->operand, locals, result);
                        const auto pointer_type = ResolvedType{
                            .Kind = TypeKind::Pointer,
                            .PointeeIndex = static_cast<int>(result.PointerPointees.size()),
                        };

                        result.PointerPointees.push_back(operand_type);

                        return pointer_type;
                    }

                case ast::UnaryOp::Deref:
                    {
                        const auto operand_type = CheckExpression(expr->operand, locals, result);
                        if (operand_type.Kind != TypeKind::Pointer) {
                            diagnostics_.report_error(
                                DiagnosticStage::Sema, expr->location,
                                "cannot dereference a non-pointer value");

                            return Types::Void;
                        }

                        return result.PointerPointees[operand_type.PointeeIndex];
                    }
                }

                diagnostics_.report_error(
                    DiagnosticStage::Sema, expr->location,
                    "invalid unary operator");

                return Types::Void;
            }

            auto CheckBinary(const std::unique_ptr<ast::BinaryExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                const auto lhs_type = CheckExpression(expr->lhs, locals, result);
                const auto rhs_type = CheckExpression(expr->rhs, locals, result);

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
                        diagnostics_.report_error(
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
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "operand type mismatch in comparison");
                    }
                    return Types::Bool;

                case ast::BinaryOp::LogicalAnd:
                case ast::BinaryOp::LogicalOr:
                    if (lhs_type.Kind != TypeKind::Bool || rhs_type.Kind != TypeKind::Bool) {
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "&&/|| require bool operands");
                    }
                    return Types::Bool;
                }

                return Types::Void;
            }

            auto CheckAssign(const std::unique_ptr<ast::AssignExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                ResolvedType target_type;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr->target)) {
                    const auto it = locals.find(ident->name);
                    if (it == locals.end()) {
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            std::format("unknown identifier '{}'t", ident->name));

                        return Types::Void;
                    }

                    if (!it->second.IsMutable) {
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            std::format("cannot assign to '{}': not declared mut", ident->name));
                    }

                    target_type = it->second.Type;
                } else {
                    diagnostics_.report_error(
                        DiagnosticStage::Sema, expr->location,
                        "invalid assignment target");

                    return Types::Void;
                }

                const auto value_type = CheckExpression(expr->value, locals, result);
                if (expr->op == ast::AssignOp::Assign) {
                    if (value_type != target_type) {
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "type mismatch in assignment");
                    }
                } else {
                    if (value_type != target_type) {
                        diagnostics_.report_error(
                            DiagnosticStage::Sema, expr->location,
                            "type mismatch in compound assignment");
                    }
                }

                return target_type;
            }
        };
    }

    auto Check(const std::vector<ast::Decl> &decls, DiagnosticEngine &diagnostics) -> SemaResult {
        return TypeChecker(diagnostics).Check(decls);
    }
}
