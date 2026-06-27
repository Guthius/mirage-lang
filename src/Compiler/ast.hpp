#pragma once

#include "ast_parser.hpp"
#include "diagnostic_engine.hpp"
#include "source_location.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ast {
    enum class BuiltinTypeKind : uint8_t {
        U8,
        U16,
        U32,
        U64,
        I8,
        I16,
        I32,
        I64,
        F32,
        F64,
        Usize,
        Bool,
        Byte,
        Error,
        Anyptr,
        Type,
    };

    struct BuiltinType {
        BuiltinTypeKind kind;
        SourceLocation location;
    };

    struct PointerType;

    struct NamedType {
        std::string name;
        std::unique_ptr<NamedType> member;
        SourceLocation location;
    };

    struct StructType;

    using Type = std::variant<std::monostate, BuiltinType, std::unique_ptr<PointerType>, NamedType, std::unique_ptr<StructType>>;

    struct PointerType {
        Type pointee;
        SourceLocation location;
    };

    struct StructType {
        struct Field {
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_packed;
        std::vector<Field> fields;
        SourceLocation location;
    };

    struct LiteralIntegerExpr {
        uint64_t value;
        SourceLocation location;
    };

    struct LiteralFloatExpr {
        double value;
        SourceLocation location;
    };

    struct LiteralStringExpr {
        std::string value;
        SourceLocation location;
    };

    struct LiteralBoolExpr {
        bool value;
        SourceLocation location;
    };

    struct LiteralNilExpr {
        SourceLocation location;
    };

    struct IdentExpr {
        std::string name;
        SourceLocation location;
    };

    struct UnaryExpr;
    struct BinaryExpr;
    struct TernaryExpr;
    struct AssignExpr;
    struct CallExpr;
    struct IncrDecrExpr;

    struct ImportExpr {
        std::string module_name;
        SourceLocation location;
    };

    struct SizeOfExpr;
    struct CastExpr;
    struct MemberExpr;
    struct DerefExpr;

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
        ImportExpr,
        std::unique_ptr<SizeOfExpr>,
        std::unique_ptr<CastExpr>,
        std::unique_ptr<MemberExpr>,
        std::unique_ptr<DerefExpr>>;

    enum class UnaryOp : uint8_t {
        Negate,
        LogicalNot,
        BitwiseNot,
        AddressOf,
        Deref,
    };

    struct UnaryExpr {
        UnaryOp op;
        Expr operand;
        SourceLocation location;
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
        BinaryOp op;
        Expr lhs;
        Expr rhs;
        SourceLocation location;
    };

    struct TernaryExpr {
        Expr condition;
        Expr then_expr;
        Expr else_expr;
        SourceLocation location;
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
        AssignOp op;
        Expr target;
        Expr value;
        SourceLocation location;
    };

    struct CallExpr {
        Expr callee;
        std::vector<Expr> args;
        SourceLocation location;
    };

    struct IncrDecrExpr {
        Expr operand;
        bool is_increment;
        bool is_prefix;
        SourceLocation location;
    };

    struct SizeOfExpr {
        Expr operand;
        SourceLocation location;
    };

    struct CastExpr {
        Expr value;
        Type as_type;
        SourceLocation location;
    };

    struct MemberExpr {
        Expr object;
        std::string member;
        SourceLocation location;
    };

    struct DerefExpr {
        Expr operand;
        SourceLocation location;
    };

    struct BlockStmt;
    struct IfStmt;
    struct WhileStmt;

    struct ExprStmt {
        Expr expr;
        SourceLocation location;
    };

    struct VarDeclStmt {
        bool is_mut;
        std::string name;
        std::optional<Type> type;
        std::optional<Expr> init;
        SourceLocation location;
    };

    struct ContinueStmt {
        SourceLocation location;
    };

    struct ReturnStmt {
        std::vector<Expr> return_values;
        SourceLocation location;
    };

    using Stmt = std::variant<
        std::unique_ptr<BlockStmt>,
        std::unique_ptr<IfStmt>,
        std::unique_ptr<WhileStmt>,
        ExprStmt,
        VarDeclStmt,
        ContinueStmt,
        ReturnStmt>;

    struct BlockStmt {
        std::vector<Stmt> stmts;
        SourceLocation location;
    };

    struct IfStmt {
        Expr condition;
        Stmt then_stmt;
        std::optional<Stmt> else_stmt;
        SourceLocation location;
    };

    struct WhileStmt {
        Expr condition;
        Stmt body;
        SourceLocation location;
    };

    struct FunctionDecl {
        struct Param {
            bool is_mut;
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_pub;
        std::string name;
        std::vector<Param> params;
        std::vector<Type> return_types;
        Stmt body;
        SourceLocation location;
    };

    struct ExtFunctionDecl {
        struct Param {
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_pub;
        std::string name;
        std::vector<Param> params;
        std::optional<Type> return_type;
        SourceLocation location;
    };

    struct VarDecl {
        bool is_pub;
        bool is_mut;
        std::string name;
        std::optional<Type> type;
        std::optional<Expr> init;
        SourceLocation location;
    };

    struct MacroDecl {
        struct Param {
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_pub;
        std::string name;
        std::vector<Param> params;
        Expr expr_template;
        SourceLocation location;
    };

    struct TypeDecl {
        bool is_pub;
        std::string name;
        Type type;
        SourceLocation location;
    };

    using Decl = std::variant<FunctionDecl, ExtFunctionDecl, VarDecl, MacroDecl, TypeDecl>;

    auto parse(std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl>;

    auto parse_type(Parser &parser) -> Type;
    auto parse_decl(Parser &parser, bool top_level) -> std::optional<Decl>;
    auto parse_stmt(Parser &parser) -> Stmt;
    auto parse_expr(Parser &parser, bool allow_import = false) -> Expr;
}
