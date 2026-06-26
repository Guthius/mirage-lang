#pragma once

#include <Compiler/SourceLocation.hpp>

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace Ast {
    struct LiteralIntegerExpr {
        uint64_t Value;
        SourceLocation Location;
    };

    struct LiteralFloatExpr {
        double Value;
        SourceLocation Location;
    };

    struct LiteralStringExpr {
        std::string Value;
        SourceLocation Location;
    };

    struct LiteralBoolExpr {
        bool Value;
        SourceLocation Location;
    };

    struct LiteralNilExpr {
        SourceLocation Location;
    };

    struct IdentExpr {
        std::string Name;
        SourceLocation Location;
    };

    struct UnaryExpr;
    struct BinaryExpr;
    struct TernaryExpr;
    struct AssignExpr;
    struct CallExpr;
    struct IncrDecrExpr;

    struct ImportExpr {
        std::string ModuleName;
        SourceLocation Location;
    };

    using Expr = std::variant<
        LiteralIntegerExpr,
        LiteralFloatExpr,
        LiteralStringExpr,
        LiteralBoolExpr,
        LiteralNilExpr,
        IdentExpr,
        std::unique_ptr<UnaryExpr>,
        std::unique_ptr<BinaryExpr>,
        std::unique_ptr<TernaryExpr>,
        std::unique_ptr<AssignExpr>,
        std::unique_ptr<CallExpr>,
        std::unique_ptr<IncrDecrExpr>,
        ImportExpr>;

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
        Expr ThenExpr;
        Expr ElseExpr;
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

    struct IncrDecrExpr {
        Expr Operand;
        bool IsIncrement;
        bool IsPrefix;
        SourceLocation Location;
    };
}
