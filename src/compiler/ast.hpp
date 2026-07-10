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
    struct FunctionType;

    struct NamedType {
        std::string name;
        std::unique_ptr<NamedType> member;
        SourceLocation location;
    };

    struct StructType;
    struct ArrayType;
    struct SliceType;
    struct EnumType;
    struct UnionType;

    using Type = std::variant<
        std::monostate,
        BuiltinType,
        std::unique_ptr<PointerType>,
        NamedType,
        std::unique_ptr<StructType>,
        std::unique_ptr<ArrayType>,
        std::unique_ptr<SliceType>,
        std::unique_ptr<EnumType>,
        std::unique_ptr<UnionType>,
        std::unique_ptr<FunctionType>>;

    struct PointerType {
        Type pointee;
        SourceLocation location;
    };

    struct SliceType {
        Type base_type;
        SourceLocation location;
    };

    // fn(ParamType, ...) -> RetType          — single return
    // fn(ParamType, ...) -> (Ret1, Ret2)     — multi-return (parens required)
    // fn(ParamType, ...)                     — no return
    // fn(ParamType, ..., ...)                — variadic (last param is ...)
    struct FunctionType {
        std::vector<Type> param_types;
        std::vector<Type> return_types; // empty = void, 1 = single, 2+ = multi
        bool is_variadic = false;
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

    struct LiteralCharExpr {
        uint8_t value;
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
    struct LenExpr;
    struct CastExpr;
    struct IndexExpr;
    struct SliceExpr;
    struct MemberExpr;
    struct MatchExpr;

    struct IotaExpr {
        SourceLocation location;
    };

    struct DotIdentExpr {
        std::string name;
        SourceLocation location;
    };

    struct StructExpr;
    struct ArrayExpr;
    struct EmptyExpr {
        SourceLocation location;
    };

    struct TaggedVariantExpr;
    struct TryExpr;
    struct RangeExpr;

    // Call-argument spread: 'expr...' forwards an existing slice as a variadic argument.
    // Only legal as the sole, final argument of a call to a native '...T' variadic function;
    // all other legality is enforced in sema (see check_expr's default rejection for this node).
    struct SpreadExpr;

    struct DefaultExpr {
        SourceLocation location;
    };

    struct UndefinedExpr {
        SourceLocation location;
    };

    using BracedInitializerExpr = std::variant<StructExpr, ArrayExpr, EmptyExpr>;

    using Expr = std::variant<
        LiteralIntegerExpr,
        LiteralFloatExpr,
        LiteralStringExpr,
        LiteralCharExpr,
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
        std::unique_ptr<LenExpr>,
        std::unique_ptr<CastExpr>,
        std::unique_ptr<IndexExpr>,
        std::unique_ptr<SliceExpr>,
        std::unique_ptr<MemberExpr>,
        IotaExpr,
        DotIdentExpr,
        std::unique_ptr<MatchExpr>,
        std::unique_ptr<BracedInitializerExpr>,
        std::unique_ptr<TaggedVariantExpr>,
        DefaultExpr,
        UndefinedExpr,
        std::unique_ptr<TryExpr>,
        std::unique_ptr<RangeExpr>,
        std::unique_ptr<SpreadExpr>>;

    struct StructType {
        struct Field {
            std::string name;
            Type type;
            std::optional<Expr> init;
            SourceLocation location;
        };

        bool is_packed;
        std::vector<Field> fields;
        SourceLocation location;
    };

    struct ArrayType {
        Type base_type;
        std::optional<Expr> size; // nullopt = '[?]T', size inferred from the initializer
        SourceLocation location;
    };

    struct UnionType {
        struct Member {
            std::string name;
            Type type; // std::monostate for payload-free tagged variants
            SourceLocation location;
        };

        bool is_tagged = false;
        std::vector<Member> members;
        SourceLocation location;
    };

    struct EnumType {
        struct Field {
            std::string name;
            std::optional<Expr> init;
            SourceLocation location;
        };

        std::optional<Type> underlying_type;
        std::vector<Field> fields;
        SourceLocation location;
    };

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

    struct LenExpr {
        Expr operand;
        SourceLocation location;
    };

    struct CastExpr {
        Expr value;
        Type as_type;
        std::optional<Expr> len_expr;
        SourceLocation location;
    };

    struct IndexExpr {
        Expr operand;
        Expr index;
        SourceLocation location;
    };

    struct SliceExpr {
        Expr operand;
        Expr start;
        Expr end;
        SourceLocation location;
    };

    struct MemberExpr {
        Expr object;
        std::string member;
        SourceLocation location;
    };

    struct MatchExpr {
        // Arm pattern variants
        struct VariantPattern {
            std::string name;
            std::optional<std::string> capture_name; // nullopt if no capture
            bool capture_by_ref = false;             // true for (&v) capture
        };
        struct LiteralPattern {
            std::unique_ptr<Expr> expr; // compile-time constant; type-checked against operand type
        };
        struct DefaultPattern {}; // the '_' wildcard arm

        using ArmPattern = std::variant<VariantPattern, LiteralPattern, DefaultPattern>;

        struct Arm {
            ArmPattern pattern;
            Expr value;
            SourceLocation location;
        };

        Expr operand;
        std::vector<Arm> arms;
        SourceLocation location;
    };

    struct StructExpr {
        struct Field {
            std::string name;
            Expr expr;
            SourceLocation location;
        };

        std::vector<Field> fields;
        SourceLocation location;
    };

    struct ArrayExpr {
        std::vector<Expr> values;
        bool has_fill = false; // true if the last value ends with '...' to fill remaining elements
        SourceLocation location;
    };

    // Tagged union variant construction: `TypeName.variant{field = val}` (qualified)
    // or `.variant{field = val}` (contextual). Payload-free variants use DotIdentExpr.
    struct TaggedVariantExpr {
        std::optional<NamedType> type_path;  // nullopt for contextual form
        std::string variant_name;
        std::optional<StructExpr> payload;   // nullopt only for qualified payload-free
        SourceLocation location;
    };

    struct TryExpr {
        Expr call; // must resolve to a CallExpr (validated in sema)
        SourceLocation location;
    };

    struct RangeExpr {
        std::optional<Expr> lower; // nullopt means implicit 0
        Expr upper;                // exclusive upper bound
        SourceLocation location;
    };

    struct SpreadExpr {
        Expr operand;
        SourceLocation location;
    };

    struct BlockStmt;
    struct IfStmt;
    struct WhileStmt;
    struct SwitchStmt;

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

    struct VarDeclGroupStmt {
        bool is_mut;
        std::vector<std::string> names;
        Expr init;
        SourceLocation location;
    };

    struct ContinueStmt {
        SourceLocation location;
    };

    struct BreakStmt {
        SourceLocation location;
    };

    struct ReturnStmt {
        std::vector<Expr> return_values;
        SourceLocation location;
    };

    struct DeferStmt;
    struct ForInStmt;

    using Stmt = std::variant<
        std::unique_ptr<BlockStmt>,
        std::unique_ptr<IfStmt>,
        std::unique_ptr<WhileStmt>,
        std::unique_ptr<ForInStmt>,
        std::unique_ptr<SwitchStmt>,
        ExprStmt,
        VarDeclStmt,
        VarDeclGroupStmt,
        ContinueStmt,
        BreakStmt,
        ReturnStmt,
        std::unique_ptr<DeferStmt>>;

    struct DeferStmt {
        Stmt body;
        SourceLocation location;
    };

    struct ForInStmt {
        std::string index_name;       // "_" if omitted
        std::string element_name;
        bool element_by_ref = false;  // true for &val syntax
        Expr iterable;
        Stmt body;
        SourceLocation location;
    };

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

    // switch EXPR { PATTERN: STMT, PATTERN: STMT, ... }
    // Statement-level switch: no result type, no exhaustiveness requirement.
    // Same arm patterns as MatchExpr; arm bodies are statements.
    struct SwitchStmt {
        struct Arm {
            MatchExpr::ArmPattern pattern;
            Stmt body;
            SourceLocation location;
        };

        Expr operand;
        std::vector<Arm> arms;
        SourceLocation location;
    };

    struct FunctionDecl {
        struct Param {
            bool is_mut;
            std::string name;
            Type type;
            bool is_variadic = false; // 'name: ...T' — native variadic; T is the element type, dissolves to []T in sema
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
        bool is_variadic = false;
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

    struct ImplDecl {
        struct Function {
            struct Param {
                std::string name;
                Type type;
                bool is_mut;
                bool is_variadic = false;
                SourceLocation location;
            };

            bool is_pub;
            bool is_mut_self;
            std::string name;
            std::vector<Param> params; // non-self params
            std::vector<Type> return_types;
            Stmt body;
            SourceLocation location;
        };

        NamedType target;
        std::vector<Function> functions;
        SourceLocation location;
    };

    using Decl = std::variant<FunctionDecl, ExtFunctionDecl, VarDecl, MacroDecl, TypeDecl, ImplDecl>;

    auto parse(std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl>;

    auto parse_type(Parser &parser) -> Type;
    auto parse_decl(Parser &parser, bool top_level) -> std::optional<Decl>;
    auto parse_stmt(Parser &parser) -> Stmt;
    auto parse_expr(Parser &parser, bool allow_import = false) -> Expr;
}
