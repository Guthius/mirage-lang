#pragma once

#include <Compiler/SourceLocation.hpp>

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace Ast {
    struct IntLiteralExpr;
    struct FloatLiteralExpr;
    struct StringLiteralExpr;
    struct BoolLiteralExpr;
    struct IdentExpr;
    struct UnaryExpr;
    struct BinaryExpr;
    struct TernaryExpr;
    struct AssignExpr;
    struct CallExpr;

    using Expr = std::variant<
        std::unique_ptr<IntLiteralExpr>,
        std::unique_ptr<FloatLiteralExpr>,
        std::unique_ptr<StringLiteralExpr>,
        std::unique_ptr<BoolLiteralExpr>,
        std::unique_ptr<IdentExpr>,
        std::unique_ptr<UnaryExpr>,
        std::unique_ptr<BinaryExpr>,
        std::unique_ptr<TernaryExpr>,
        std::unique_ptr<AssignExpr>,
        std::unique_ptr<CallExpr>>;

    struct IntLiteralExpr {
        uint64_t Value;
        SourceLocation Location;
    };

    struct FloatLiteralExpr {
        double Value;
        SourceLocation Location;
    };

    struct StringLiteralExpr {
        std::string Value;
        SourceLocation Location;
    };

    struct BoolLiteralExpr {
        bool Value;
        SourceLocation Location;
    };

    struct IdentExpr {
        std::string Name;
        SourceLocation Location;
    };

    enum class UnaryOp : uint8_t {
        Negate,
        LogicalNot,
        BitwiseNot,
        AddressOf,
        Deref,
    };

    struct UnaryExpr {
        UnaryOp Op;
        Expr Operand;
        SourceLocation Location;
    };

    enum class BinaryOp : uint8_t {
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        BitwiseAnd,
        BitwiseOr,
        BitwiseXor,
        ShiftLeft,
        ShiftRight,
        Equal,
        NotEqual,
        Less,
        Greater,
        LessEqual,
        GreaterEqual,
        LogicalAnd,
        LogicalOr,
    };

    struct BinaryExpr {
        BinaryOp Op;
        Expr Lhs;
        Expr Rhs;
        SourceLocation Location;
    };

    struct TernaryExpr {
        Expr Condition;
        Expr Then;
        Expr Else;
        SourceLocation Location;
    };

    enum class AssignOp : uint8_t {
        Assign,
        AddAssign,
        SubAssign,
        MulAssign,
        DivAssign,
        AndAssign,
        OrAssign,
        XorAssign,
        ShlAssign,
        ShrAssign,
    };

    struct AssignExpr {
        AssignOp Op;
        Expr Target;
        Expr Value;
        SourceLocation Location;
    };

    struct CallExpr {
        Expr Callee;
        std::vector<Expr> Args;
        SourceLocation Location;
    };
}
