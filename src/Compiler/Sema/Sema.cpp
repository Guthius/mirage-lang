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

            auto Check(const std::vector<Ast::Decl> &decls) -> SemaResult {
                SemaResult result;

                for (auto &decl : decls) {
                    const auto function_decl = std::get_if<Ast::FunctionDecl>(&decl);
                    if (function_decl == nullptr) {
                        continue;
                    }

                    CheckFunction(*function_decl, result);
                }

                return result;
            }

            void CheckFunction(const Ast::FunctionDecl &decl, SemaResult &result) {
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

            void CheckStatement(const std::unique_ptr<Ast::BlockStmt> &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                auto inner = locals;

                for (auto &inner_stmt : stmt->Statements) {
                    CheckStatement(inner_stmt, inner, result, expected_returns);
                }
            }

            void CheckStatement(const std::unique_ptr<Ast::AsmStmt> &asm_stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
            }

            void CheckStatement(const std::unique_ptr<Ast::ExprStmt> &expr_stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                CheckExpression(expr_stmt->Expr, locals, result);
            }

            void CheckStatement(const Ast::Stmt &stmt, LocalMap &locals, SemaResult &result, const std::vector<ResolvedType> &expected_returns) {
                std::visit(
                    [&](const auto &v) {
                        CheckStatement(v, locals, result, expected_returns);
                    },
                    stmt);
            }

            auto CheckExpression(const std::unique_ptr<Ast::LiteralIntegerExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                if (expected && expected->IsInteger()) {
                    return *expected;
                }

                return ResolvedType{.Kind = TypeKind::I32};
            }

            auto CheckExpression(const std::unique_ptr<Ast::LiteralFloatExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                if (expected && expected->IsFloat()) {
                    return *expected;
                }

                return ResolvedType{.Kind = TypeKind::F32};
            }

            auto CheckExpression(const std::unique_ptr<Ast::LiteralStringExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                diagnostics_.ReportError(DiagnosticStage::Sema, expr->Location, "string literals are not supported yet");

                return Types::Void;
            }

            auto CheckExpression(const std::unique_ptr<Ast::LiteralBoolExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return Types::Bool;
            }

            auto CheckExpression(const std::unique_ptr<Ast::LiteralNilExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return Types::Anyptr;
            }

            auto CheckExpression(const std::unique_ptr<Ast::IdentExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                const auto it = locals.find(expr->Name);
                if (it == locals.end()) {
                    diagnostics_.ReportError(
                        DiagnosticStage::Sema, expr->Location,
                        std::format("unknown identifier '{}'", expr->Name));

                    return Types::Void;
                }

                return it->second.Type;
            }

            auto CheckExpression(const std::unique_ptr<Ast::UnaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckUnary(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<Ast::BinaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckBinary(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<Ast::TernaryExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                CheckExpression(expr->Condition, locals, result, ResolvedType{.Kind = TypeKind::Bool});

                const auto type_a = CheckExpression(expr->ThenExpr, locals, result);
                const auto type_b = CheckExpression(expr->ElseExpr, locals, result);

                if (type_a != type_b) {
                    diagnostics_.ReportError(
                        DiagnosticStage::Sema, expr->Location,
                        "ternary branches have different types");
                }

                return type_a;
            }

            auto CheckExpression(const std::unique_ptr<Ast::AssignExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                return CheckAssign(expr, locals, result);
            }

            auto CheckExpression(const std::unique_ptr<Ast::CallExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) const -> ResolvedType {
                diagnostics_.ReportError(DiagnosticStage::Sema, expr->Location, "call expressions are not supported yet");

                return Types::Void;
            }

            auto CheckExpression(const std::unique_ptr<Ast::IncrDecrExpr> &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected) -> ResolvedType {
                const auto operand_type = CheckExpression(expr->Operand, locals, result);

                if (!operand_type.IsInteger()) {
                    diagnostics_.ReportError(
                        DiagnosticStage::Sema, expr->Location,
                        "operand of increment/decrement expression must be an integer");
                }

                return operand_type;
            }

            auto CheckExpression(const Ast::Expr &expr, LocalMap &locals, SemaResult &result, std::optional<ResolvedType> expected = std::nullopt) -> ResolvedType {
                const auto resolved_type = std::visit(
                    [&](const auto &v) {
                        return CheckExpression(v, locals, result, expected);
                    },
                    expr);

                result.expr_types[GetExprKey(expr)] = resolved_type;

                return resolved_type;
            }

            auto CheckUnary(const std::unique_ptr<Ast::UnaryExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                switch (expr->Op) {
                case Ast::UnaryOp::Negate:
                    {
                        const auto operand_type = CheckExpression(expr->Operand, locals, result);
                        if (!operand_type.IsInteger()) {
                            diagnostics_.ReportError(DiagnosticStage::Sema, expr->Location, "unary '-' requires an integer operand");
                        }

                        return operand_type;
                    }

                case Ast::UnaryOp::LogicalNot:
                    {
                        CheckExpression(expr->Operand, locals, result, Types::Bool);

                        return Types::Bool;
                    }

                case Ast::UnaryOp::BitwiseNot:
                    {
                        const auto operand_type = CheckExpression(expr->Operand, locals, result);
                        if (!operand_type.IsInteger()) {
                            diagnostics_.ReportError(DiagnosticStage::Sema, expr->Location, "unary '~' requires an integer operand");
                        }

                        return operand_type;
                    }

                case Ast::UnaryOp::AddressOf:
                    {
                        const auto operand_type = CheckExpression(expr->Operand, locals, result);
                        const auto pointer_type = ResolvedType{
                            .Kind = TypeKind::Pointer,
                            .PointeeIndex = static_cast<int>(result.PointerPointees.size()),
                        };

                        result.PointerPointees.push_back(operand_type);

                        return pointer_type;
                    }

                case Ast::UnaryOp::Deref:
                    {
                        const auto operand_type = CheckExpression(expr->Operand, locals, result);
                        if (operand_type.Kind != TypeKind::Pointer) {
                            diagnostics_.ReportError(
                                DiagnosticStage::Sema, expr->Location,
                                "cannot dereference a non-pointer value");

                            return Types::Void;
                        }

                        return result.PointerPointees[operand_type.PointeeIndex];
                    }
                }

                diagnostics_.ReportError(
                    DiagnosticStage::Sema, expr->Location,
                    "invalid unary operator");

                return Types::Void;
            }

            auto CheckBinary(const std::unique_ptr<Ast::BinaryExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                const auto lhs_type = CheckExpression(expr->Lhs, locals, result);
                const auto rhs_type = CheckExpression(expr->Rhs, locals, result);

                switch (expr->Op) {
                case Ast::BinaryOp::Add:
                case Ast::BinaryOp::Sub:
                case Ast::BinaryOp::Mul:
                case Ast::BinaryOp::Div:
                case Ast::BinaryOp::Mod:
                case Ast::BinaryOp::BitwiseAnd:
                case Ast::BinaryOp::BitwiseOr:
                case Ast::BinaryOp::BitwiseXor:
                case Ast::BinaryOp::ShiftLeft:
                case Ast::BinaryOp::ShiftRight:
                    if (lhs_type != rhs_type) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            "operand type mismatch in binary expression");
                    }
                    return lhs_type;

                case Ast::BinaryOp::Equal:
                case Ast::BinaryOp::NotEqual:
                case Ast::BinaryOp::Less:
                case Ast::BinaryOp::Greater:
                case Ast::BinaryOp::LessEqual:
                case Ast::BinaryOp::GreaterEqual:
                    if (lhs_type != rhs_type) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            "operand type mismatch in comparison");
                    }
                    return Types::Bool;

                case Ast::BinaryOp::LogicalAnd:
                case Ast::BinaryOp::LogicalOr:
                    if (lhs_type.Kind != TypeKind::Bool || rhs_type.Kind != TypeKind::Bool) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            "&&/|| require bool operands");
                    }
                    return Types::Bool;
                }

                return Types::Void;
            }

            auto CheckAssign(const std::unique_ptr<Ast::AssignExpr> &expr, LocalMap &locals, SemaResult &result) -> ResolvedType {
                ResolvedType target_type;

                if (const auto *ident = std::get_if<Ast::IdentExpr>(&expr->Target)) {
                    const auto it = locals.find(ident->Name);
                    if (it == locals.end()) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            std::format("unknown identifier '{}'t", ident->Name));

                        return Types::Void;
                    }

                    if (!it->second.IsMutable) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            std::format("cannot assign to '{}': not declared mut", ident->Name));
                    }

                    target_type = it->second.Type;
                } else {
                    diagnostics_.ReportError(
                        DiagnosticStage::Sema, expr->Location,
                        "invalid assignment target");

                    return Types::Void;
                }

                const auto value_type = CheckExpression(expr->Value, locals, result);
                if (expr->Op == Ast::AssignOp::Assign) {
                    if (value_type != target_type) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            "type mismatch in assignment");
                    }
                } else {
                    if (value_type != target_type) {
                        diagnostics_.ReportError(
                            DiagnosticStage::Sema, expr->Location,
                            "type mismatch in compound assignment");
                    }
                }

                return target_type;
            }
        };
    }

    auto Check(const std::vector<Ast::Decl> &decls, DiagnosticEngine &diagnostics) -> SemaResult {
        return TypeChecker(diagnostics).Check(decls);
    }
}
