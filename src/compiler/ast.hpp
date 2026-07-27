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
    struct TraitType;
    struct ErrorType;
    struct BitsetType;

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
        std::unique_ptr<FunctionType>,
        std::unique_ptr<TraitType>,
        std::unique_ptr<ErrorType>,
        std::unique_ptr<BitsetType>>;

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
        std::vector<std::string> param_names; // parallel to param_types; "" = unnamed.
                                               // Cosmetic only (e.g. LSP hover) — never used
                                               // for matching, identity, or interning.
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
    struct WhenExpr;
    struct AssignExpr;
    struct CallExpr;
    struct IncrDecrExpr;

    struct ImportExpr {
        std::string module_name;
        SourceLocation location;
    };

    struct ImportBinExpr {
        std::string path;
        SourceLocation location;
    };

    struct SizeOfExpr;
    struct OptionExpr;
    struct EnvExpr;
    struct TypeExpr;
    struct LenExpr;
    struct StackAllocExpr;
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

    // '{.A, .B}' — a bitset literal: a set of member names (no '= expr'), only
    // legal where the expected type is a bitset. Distinguished from StructExpr
    // ('{.field = expr, ...}') at parse time by whether '=' follows the first
    // '.IDENT'; see parse_braced_initializer.
    struct BitsetExpr {
        std::vector<std::string> members;
        SourceLocation location;
    };

    struct TaggedVariantExpr;
    struct TryExpr;
    struct RangeExpr;

    // Call-argument spread: 'expr...' forwards an existing slice as a variadic argument.
    // Only legal as the sole, final argument of a call to a native '...T' variadic function;
    // all other legality is enforced in sema (see check_expr's default rejection for this node).
    struct SpreadExpr;

    struct AsmExpr;

    struct DefaultExpr {
        SourceLocation location;
    };

    struct UndefinedExpr {
        SourceLocation location;
    };

    using BracedInitializerExpr = std::variant<StructExpr, ArrayExpr, EmptyExpr, BitsetExpr>;

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
        std::unique_ptr<WhenExpr>,
        std::unique_ptr<AssignExpr>,
        std::unique_ptr<CallExpr>,
        std::unique_ptr<IncrDecrExpr>,
        ImportExpr,
        ImportBinExpr,
        std::unique_ptr<SizeOfExpr>,
        std::unique_ptr<OptionExpr>,
        std::unique_ptr<EnvExpr>,
        std::unique_ptr<TypeExpr>,
        std::unique_ptr<LenExpr>,
        std::unique_ptr<StackAllocExpr>,
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
        std::unique_ptr<SpreadExpr>,
        std::unique_ptr<AsmExpr>>;

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

    // 'bitset(EnumName)' / 'bitset(EnumName, u16)' — a distinct nominal type
    // representing a set of 'member_type' enum variants stored as bits in an
    // integer. 'member_type' must resolve (in sema) to an enum with an
    // explicit integer backing type. 'storage_type', if omitted, defaults to
    // u32; when present it must resolve to u8/u16/u32/u64.
    struct BitsetType {
        NamedType member_type;
        std::optional<Type> storage_type;
        SourceLocation location;
    };

    // 'error(A | B | C)' — a fallible-function return type. Each member must
    // resolve (in sema) to an enum(i32) or union(enum) type; members are
    // written in source order here, but the compiler treats them as a SET
    // (order does not affect the type's identity — see sema's error-union
    // interning).
    struct ErrorType {
        std::vector<NamedType> members;
        SourceLocation location;
    };

    // 'trait { fn method(self) -> T ... }' — a signature-only dispatch surface.
    // Using a trait name in type position denotes a fat-pointer HANDLE, not the
    // trait definition itself; see sema::TraitInfo / TypeKind::Trait.
    struct TraitType {
        struct Param {
            std::string name;
            std::optional<Type> type;          // absent only for ':=' inferred-type form
            std::optional<Expr> default_value; // set whenever 'type' is absent
            SourceLocation location;
        };

        struct Method {
            std::string name;
            bool is_mut_self;
            std::vector<Param> params; // non-self params; no 'mut', no variadics (rejected by parser)
            std::vector<Type> return_types;
            SourceLocation location;      // 'fn' keyword
            SourceLocation self_location; // the 'self' token itself
        };

        std::vector<Method> methods;
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
        In, // bitset membership testing: 'expr in expr' — see sema/codegen for legal shapes
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

    // 'then_val when cond else else_val' — a compile-time-evaluated (when 'cond' is a
    // compile-time constant) or ternary-equivalent (when 'cond' is a runtime value)
    // suffix expression. Field order (condition, then, else) matches TernaryExpr/IfStmt
    // convention, despite 'then_expr' appearing FIRST in source.
    struct WhenExpr {
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
        ToggleAssign, // '~=' — bitset toggle-assign only; sema-rejected on any other target type
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

    // '#option(key)' / '#option(key, default)' — a compile-time expression that reads a
    // value supplied by the compiler driver via '--opt key=value'. Legal anywhere an
    // expression is legal (arithmetic, call arguments, 'mut' initializers, ...): its value
    // is always resolved once from '--opt'/the default and cached (ProgramModule::
    // expr_option_values), so it composes with is_constant_expr/evaluate_const_value like
    // any other compile-time-constant expression — see check_expr's OptionExpr case.
    struct OptionExpr {
        std::string key;
        std::optional<Expr> default_value;
        SourceLocation location;
    };

    // '#env(key)' / '#env(key, default)' — like '#option' above, but the value comes from
    // an environment variable ('key') instead of a '--opt key=value' driver flag. Shares
    // '#option''s resolution/caching/codegen machinery end-to-end (see resolve_env_expr,
    // ProgramModule::expr_option_values, codegen's emit_option_value) — only the value
    // source differs.
    struct EnvExpr {
        std::string key;
        std::optional<Expr> default_value;
        SourceLocation location;
    };

    // Wraps a Type in an Expr slot, for contexts that can't syntactically tell whether an
    // operand names a type or a value until a type-only-start token is seen (e.g. sizeof).
    struct TypeExpr {
        Type type;
        SourceLocation location;
    };

    struct LenExpr {
        Expr operand;
        SourceLocation location;
    };

    struct StackAllocExpr {
        Expr size;
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
    struct WhenStmt;

    // '#link(category, data)' — a linker directive. Legal at module scope (or inside a
    // module-scope 'when' block); parses successfully as a Stmt too (inside a function
    // body) purely so sema can reject it there with a precise diagnostic instead of the
    // parser silently failing to produce a node at all.
    enum class LinkCategory : uint8_t { Lib, System, Flag };

    struct LinkDecl {
        LinkCategory category;
        Expr data;
        SourceLocation location;
    };

    // '#error(message)' / '#warn(message)' — compile-time diagnostic directives. Emits a
    // sema error/warning with 'message' (a compile-time constant '[]u8' expression) when
    // reached while declaring a LIVE branch — the intended use is guarding it with a
    // module-scope 'when' block, e.g. 'when target_os == .Windows { #error("Windows is not
    // supported") } '. Legal at module scope (or inside a module-scope 'when' block); parses
    // successfully as a Stmt too (inside a function body) purely so sema can reject it there
    // with a precise diagnostic instead of a raw parse error — mirrors '#link' exactly.
    enum class DiagnosticDirectiveKind : uint8_t { Error, Warn };

    struct DiagnosticDecl {
        DiagnosticDirectiveKind kind;
        Expr message;
        SourceLocation location;
    };

    // 'import("path")' as a standalone module-scope declaration (as opposed to
    // 'const x := import("path")', which binds a namespace) — injects every 'pub'
    // symbol of the target module into this module as private, unqualified local
    // names. Never legal in statement position (a PARSE error there, handled directly
    // in parse_stmt — unlike LinkDecl/DiagnosticDecl/AsmStmt above, this is
    // deliberately NOT dual-registered into the Stmt variant) and never 'pub' itself.
    struct BareImportDecl {
        std::string path;
        SourceLocation location;
    };

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
        bool possible_asi_gotcha = false;
    };

    struct ReturnErrStmt {
        Expr error_value;
        SourceLocation location;
    };

    struct ReturnOkStmt {
        std::vector<Expr> return_values;
        SourceLocation location;
        bool possible_asi_gotcha = false;
    };

    struct DeferStmt;
    struct ForInStmt;

    // 'asm { ... }' — inline assembly, only legal inside a function body. Parses successfully
    // as a Decl too (at module scope) purely so sema can reject it there with a precise
    // diagnostic ("asm blocks are only legal inside function bodies"), mirroring how
    // LinkDecl/DiagnosticDecl parse as a Stmt purely for the mirror-image rejection above.
    // The raw asm body (an AsmBlock token's text) is lexed/parsed by the standalone
    // asm_lexer/asm_parser (see asm_lexer.hpp/asm_parser.hpp), never by the main Mirage
    // lexer/parser.
    struct AsmRegisterOperand {
        std::string name;      // normalized lowercase, e.g. "rax"
        uint32_t width_bits;   // 8, 16, 32, or 64
        SourceLocation location;
    };

    struct AsmImmediateOperand {
        int64_t value;
        SourceLocation location;
    };

    struct AsmVariableOperand {
        std::string name;   // Mirage variable name
        bool is_address;    // true if '&var', false if bare 'var'
        SourceLocation location;
    };

    using AsmOperand = std::variant<AsmRegisterOperand, AsmImmediateOperand, AsmVariableOperand>;

    struct AsmInstruction {
        std::string mnemonic; // normalized lowercase; not validated by the parser — sema decides
        std::vector<AsmOperand> operands;
        SourceLocation location;
    };

    struct AsmStmt {
        std::vector<AsmInstruction> instructions;
        SourceLocation location;
    };

    // 'asm -> reg { ... }' / 'asm -> reg: type { ... }' — the expression form of inline
    // assembly: legal anywhere an expression is legal (return, var-decl initializers, call
    // arguments, ...), unlike AsmStmt which is statement-only. 'result_register' names the
    // register whose value at block-exit becomes the expression's value; 'result_type' is
    // present only when the explicit ': type' suffix was written, otherwise the result type
    // is inferred from the surrounding expected-type context (see check_asm_expr). Shares
    // AsmInstruction/AsmOperand with AsmStmt and the same asm_lexer/asm_parser raw-body
    // pipeline — only the header ('-> reg [: type]') is parsed by the main parser.
    struct AsmExpr {
        std::vector<AsmInstruction> instructions;
        AsmRegisterOperand result_register;
        std::optional<Type> result_type;
        SourceLocation location;
    };

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
        ReturnErrStmt,
        ReturnOkStmt,
        std::unique_ptr<DeferStmt>,
        LinkDecl,
        DiagnosticDecl,
        std::unique_ptr<WhenStmt>,
        std::unique_ptr<AsmStmt>>;

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

    // 'when cond { ... } [else (when ... | { ... })]' — a compile-time conditional
    // statement. The condition must be a compile-time constant expression (sema-checked,
    // not parser-checked); both branches are always type-checked, only the selected
    // branch's statements are emitted by codegen. 'then_block' is forced to be a literal
    // block (mirrors WhileStmt's body), unlike IfStmt's 'then_stmt' which accepts any stmt.
    struct WhenStmt {
        Expr condition;
        BlockStmt then_block;
        std::optional<std::variant<BlockStmt, std::unique_ptr<WhenStmt>>> else_branch;
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

    // '@name' / '@name(arg1, arg2, ...)' — a declaration attribute. Currently legal only
    // immediately preceding a module-scope 'fn' (see FunctionDecl::attributes) or an
    // impl-block method (see ImplDecl::Function::attributes, parsed permissively there so
    // sema can reject method-illegal attributes like '@init' with a precise diagnostic —
    // mirrors '#link'/'#error'/'#warn's "parse anywhere, reject in sema" idiom). 'name' is
    // validated against the fixed known-attribute-name set by the PARSER (a pure lexical
    // fact, no semantic context needed — mirrors '#link's category-name validation);
    // argument count/type/constant-ness and inter-attribute legality are sema's job (see
    // sema_attributes.cpp). 'args' is empty for a bare '@name' or for a grouped-form member
    // (grouped attributes never take their own argument list — only the single '@name(args)'
    // form does).
    struct Attribute {
        std::string name;
        std::vector<Expr> args;
        SourceLocation location;
    };

    struct FunctionDecl {
        struct Param {
            bool is_mut;
            std::string name;
            std::optional<Type> type;          // absent only for ':=' inferred-type form
            std::optional<Expr> default_value; // set whenever 'type' is absent
            bool is_variadic = false; // 'name: ...T' — native variadic; T is the element type, dissolves to []T in sema
            SourceLocation location;
        };

        bool is_pub;
        std::vector<Attribute> attributes; // empty if none; see 'attribute_clause' in the parser
        std::string name;
        std::vector<Param> params;
        std::vector<Type> return_types;
        Stmt body;
        SourceLocation location;
    };

    struct ExtFunctionDecl {
        struct Param {
            std::string name;
            Type type; // always required — ext fn params never use ':=' inference
            std::optional<Expr> default_value; // parsed so sema can reject with a clear diagnostic; never valid
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
        std::optional<Type> result_type;
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
                std::optional<Type> type;          // absent only for ':=' inferred-type form
                std::optional<Expr> default_value; // set whenever 'type' is absent
                bool is_mut;
                bool is_variadic = false;
                SourceLocation location;
            };

            bool is_pub;
            std::vector<Attribute> attributes; // empty if none; every attribute name here is
                                                // sema-rejected for '@init' specifically (see
                                                // sema_attributes.cpp) — the other four are
                                                // validated identically to a free function's.
            bool is_mut_self;
            std::string name;
            std::vector<Param> params; // non-self params
            std::vector<Type> return_types;
            Stmt body;
            SourceLocation location;      // 'fn' keyword
            SourceLocation self_location; // the 'self' token itself
        };

        NamedType target;
        std::vector<Function> functions;
        SourceLocation location;
    };

    // 'impl TRAIT for TYPE { ... }' — a trait implementation. Distinct from the bare
    // 'impl TYPE { ... }' form (ImplDecl) above, which is left untouched.
    struct TraitImplDecl {
        NamedType trait_name;
        NamedType type_name;
        std::vector<ImplDecl::Function> functions;
        SourceLocation location;
    };

    // 'when cond { decl... } [else (when ... | { decl... })]' — a module-scope compile-time
    // conditional declaration block. Parsed permissively (any Decl kind inside), restricted
    // to '#link'/'const' with '#option'/'type'/'ext fn' by sema. Boxed via unique_ptr (not
    // stored by value like Decl's other alternatives) because it holds a 'vector<Decl>' —
    // Decl isn't a complete type until this very 'using Decl = ...' declaration finishes, so
    // WhenDecl can only be fully DEFINED afterward; the variant itself only needs the
    // forward-declared name via unique_ptr at this point.
    struct WhenDecl;

    using Decl = std::variant<FunctionDecl, ExtFunctionDecl, VarDecl, MacroDecl, TypeDecl, ImplDecl, TraitImplDecl,
                               LinkDecl, DiagnosticDecl, BareImportDecl, std::unique_ptr<WhenDecl>, std::unique_ptr<AsmStmt>>;

    struct WhenDecl {
        Expr condition;
        std::vector<Decl> then_decls;
        std::optional<std::variant<std::vector<Decl>, std::unique_ptr<WhenDecl>>> else_branch;
        SourceLocation location;
    };

    auto parse(std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl>;

    // Consumes any run of (explicit or ASI-inserted) ';' separators. Struct/union/enum bodies
    // and statement lists are newline-delimited with no comma requirement, so once ASI starts
    // emitting real Semicolon tokens between items, these list-parsing loops need to tolerate
    // (not require) them as stray separators.
    auto skip_semicolons(Parser &parser) -> void;

    auto parse_type(Parser &parser) -> Type;
    auto parse_decl(Parser &parser, bool top_level) -> std::optional<Decl>;
    auto parse_stmt(Parser &parser) -> Stmt;
    auto parse_expr(Parser &parser, bool allow_import = false) -> Expr;

    // Unwraps a chain of MemberExpr.object (e.g. `import(...).a.b`) down to its base
    // ImportExpr, or returns nullptr if the chain doesn't bottom out in one. Shared by
    // module_resolver.cpp (import-graph discovery) and sema_declare.cpp/sema_check.cpp
    // (resolving/caching the module path a chained '.field' access reads from).
    auto find_leaf_import(const Expr &expr) -> const ImportExpr *;
}
