#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"
#include "module_resolver.hpp"
#include "resolved_type.hpp"
#include "symbol_table.hpp"

#include <optional>
#include <set>
#include <unordered_map>

namespace sema {
    struct StructField {
        std::string name;
        ResolvedType type;
        uint32_t offset = 0;
        const ast::Expr *init_expr = nullptr; // null if no field-level default initializer
    };

    struct StructInfo {
        std::string module_path; // module where this struct is declared
        std::vector<StructField> fields;
        uint32_t size = 0;
        uint32_t align = 1;
        bool is_packed = false;
        bool layout_done = false;
    };

    struct ArrayInfo {
        ResolvedType element_type;
        uint64_t count = 0;
        uint32_t size = 0;
        uint32_t align = 1;
    };

    struct SliceInfo {
        ResolvedType element_type;
    };

    struct EnumFieldInfo {
        std::string name;
        int64_t value = 0;
    };

    struct EnumInfo {
        ResolvedType underlying_type;
        std::vector<EnumFieldInfo> fields;
        bool layout_done = false;
    };

    struct UnionMember {
        std::string name;
        ResolvedType type;
    };

    // One variant in a tagged union (union(enum))
    struct TaggedUnionVariant {
        std::string name;
        int32_t tag_value = 0;             // 0..N-1 in declaration order
        int32_t payload_struct_index = -1; // global struct index; -1 if payload-free
        ResolvedType payload_type;         // declared payload type: Struct{struct_index} for
                                            // struct-literal payloads, the bare type (e.g. Slice)
                                            // for wrapped scalar/slice/pointer/array payloads
    };

    struct UnionInfo {
        std::string module_path; // module where this union is declared
        std::vector<UnionMember> members;       // for untagged unions
        bool is_tagged = false;
        std::vector<TaggedUnionVariant> variants; // for tagged unions
        uint32_t payload_offset = 0;            // byte offset of payload after tag
        uint32_t size = 0;
        uint32_t align = 1;
        bool layout_done = false;
    };

    // The tag field of a tagged union is always u32.
    inline const ResolvedType TAG_TYPE{.kind = TypeKind::U32};

    // Signature stored for a function-pointer type: fn(ParamTypes) -> ReturnTypes
    struct FunctionTypeInfo {
        std::vector<ResolvedType> param_types;
        std::vector<ResolvedType> return_types; // empty=void, 1=single, 2+=multi
        bool is_variadic = false;
    };

    struct MethodInfo {
        const ast::ImplDecl::Function *decl = nullptr;
        std::string impl_module;               // module where this impl is defined
        std::string type_name;                 // type name this method belongs to
        ResolvedType self_type;                // resolved type of self (the struct/enum type)
        std::vector<ResolvedType> param_types; // non-self params
        std::vector<ResolvedType> return_types;
        bool is_mut_self = false;
        bool is_pub = false;
        bool is_resolved = false;
    };

    // Records an implicit tagged-union coercion decided by check_expr for a given expression node
    // (see check_expr's expected-type tail). The scalar expression's own natural type is left
    // untouched in expr_types (overwriting it would corrupt leaf codegen, e.g. integer/float literal
    // constant construction, which reads its LLVM type directly from expr_types); codegen instead
    // consults this side table and materializes the wrapped union value from the already-decided
    // variant, never re-scanning UnionInfo::variants itself.
    struct VariantCoercion {
        ResolvedType union_type;
        int32_t tag_value = 0;
        int32_t payload_struct_index = -1;
    };

    struct ProgramModule {
        SymbolTable symbols;
        // type_name -> method_name -> MethodInfo
        std::unordered_map<std::string, std::unordered_map<std::string, MethodInfo>> methods;
        std::unordered_map<const void *, ResolvedType> expr_types;
        std::unordered_map<const void *, VariantCoercion> expr_variant_coercions;
        bool ok = false;
    };

    struct ResolveState {
        std::set<std::pair<std::string, std::string>> alias_resolving;
        std::set<std::pair<std::string, std::string>> struct_resolving;
        std::set<std::pair<std::string, std::string>> union_resolving;
        std::set<std::pair<std::string, std::string>> value_resolving;
    };

    struct Program {
        std::unordered_map<std::string, ProgramModule> modules;
        std::vector<StructInfo> structs;          // global; struct_index is unique across all modules
        std::vector<EnumInfo> enums;
        std::vector<UnionInfo> unions;            // global; union_index is unique across all modules
        std::vector<FunctionTypeInfo> fn_signatures; // global; fn_index is unique across all modules
        std::vector<ResolvedType> pointer_pointees; // global; pointee_index is unique across all modules
        std::vector<ArrayInfo> arrays;             // global; array_index is unique across all modules
        std::vector<SliceInfo> slices;             // global; slice_index is unique across all modules
        ResolveState resolve_state;
        bool ok = false;
    };

    struct LocalBinding {
        ResolvedType type;
        bool is_mut = false;
    };

    using LocalScope = std::unordered_map<std::string, LocalBinding>;

    inline auto get_expr_key(const ast::Expr &expr) -> const void * {
        return std::visit([](const auto &v) -> const void * { return &v; }, expr);
    }

    // Expr alternatives are a mix of by-value nodes ('.location'), boxed 'unique_ptr<T>' nodes
    // ('->location'), and the boxed BracedInitializerExpr (a unique_ptr to a nested variant with
    // no location of its own); this uniformly extracts the location regardless of which.
    inline auto get_expr_location(const ast::Expr &expr) -> SourceLocation {
        return std::visit([](const auto &v) -> SourceLocation {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                return std::visit([](const auto &bv) -> SourceLocation { return bv.location; }, *v);
            } else if constexpr (requires { v->location; }) {
                return v->location;
            } else {
                return v.location;
            }
        }, expr);
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag) -> Program;
    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType;
    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool;
    auto intern_pointer(Program &program, const ResolvedType &pointee) -> ResolvedType;
    auto intern_slice(Program &program, const ResolvedType &element) -> ResolvedType;
    auto intern_function_type(Program &program, FunctionTypeInfo sig) -> ResolvedType;
    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_global_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_macro_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> MacroSymbol &;
    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, std::optional<ResolvedType> expected, int loop_depth, int defer_loop_base = -1) -> ResolvedType;
    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth, int defer_loop_base = -1) -> void;
    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program) -> bool;
    // Evaluate a compile-time integer or bool constant expression. Returns nullopt if the expression
    // cannot be statically evaluated (e.g. non-constant or unsupported form). Used by match/switch
    // for duplicate arm detection (sema) and case-value emission (codegen).
    auto evaluate_integer_constant(const ast::Expr &expr, const std::string &module_path, const Program &program) -> std::optional<int64_t>;

    // Returns the module path and type name for a given resolved struct/enum type,
    // searching all modules. Returns {"", ""} if not found.
    auto find_type_module_and_name(const ResolvedType &ty, const Program &program) -> std::pair<std::string, std::string>;

    // Look up a method on a type. Searches the type's defining module's method table.
    auto find_method(const ResolvedType &ty, const std::string &method_name, const Program &program) -> const MethodInfo *;
}
