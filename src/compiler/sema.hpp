#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"
#include "module_resolver.hpp"
#include "resolved_type.hpp"
#include "symbol_table.hpp"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace sema {
    struct StructField {
        std::string name;
        ResolvedType type;
        uint32_t offset = 0;
        const ast::Expr *init_expr = nullptr; // null if no field-level default initializer
        SourceLocation location;
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
        SourceLocation location;
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
        bool is_variadic = false;             // true if the last param is native '...T'
        ResolvedType variadic_element_type{}; // T; only meaningful if is_variadic

        // Set (to the trait's local name) only when this MethodInfo came from an
        // 'impl TRAIT for TYPE' block, rather than a bare 'impl TYPE' block. Codegen
        // uses this to route function lookups to the trait_functions_ table instead
        // of functions_, since trait-impl methods are mangled/keyed distinctly to
        // avoid colliding with a same-named inherent method (see Generator::run()).
        std::optional<std::string> trait_name;
        std::string trait_module; // only meaningful if trait_name has a value
    };

    // A single trait method's resolved signature (no body — trait methods are
    // signature-only). Declaration order matches the source 'trait { ... }' body
    // exactly; this order IS the vtable layout and must never be re-sorted or
    // re-derived by codegen.
    struct TraitMethodInfo {
        std::string name;
        bool is_mut_self = false;
        std::vector<ResolvedType> params; // excludes self
        std::vector<ResolvedType> return_types;
        SourceLocation location;
    };

    struct TraitInfo {
        std::string module_path;            // module where 'type Name = trait {...}' is declared
        std::vector<TraitMethodInfo> methods; // declaration order — the vtable layout
        bool layout_done = false;
    };

    // One successfully-declared 'impl TRAIT for TYPE' block.
    struct TraitImplInfo {
        std::string trait_module, trait_name;
        int trait_index = -1;
        std::string type_module, type_name;
        std::string impl_module; // module where the 'impl ... for ...' block itself lives
        std::unordered_map<std::string, MethodInfo> methods; // method_name -> MethodInfo
        SourceLocation location;
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

    // Records an implicit pointer-to-trait-handle coercion decided by check_expr's
    // expected-type tail (mirrors VariantCoercion above). The pointer expression's
    // own natural type is left untouched in expr_types; codegen consults this side
    // table to know which trait handle to materialize, and looks up the (trait,
    // pointee-type) vtable global via Program::trait_impls_by_type, never re-deriving
    // trait membership itself.
    struct TraitCoercion {
        int trait_index = -1;
    };

    // Records where a '.method()' call was resolved against a trait's method list via
    // an actual dyn-handle receiver (TypeKind::Trait), rather than a concrete MethodInfo.
    // 'method_order_index' is the index into TraitInfo::methods — the vtable slot.
    struct TraitDispatchInfo {
        int trait_index = -1;
        int method_order_index = -1;
    };

    struct ProgramModule {
        SymbolTable symbols;
        // type_name -> method_name -> MethodInfo
        std::unordered_map<std::string, std::unordered_map<std::string, MethodInfo>> methods;
        std::unordered_map<const void *, ResolvedType> expr_types;
        std::unordered_map<const void *, VariantCoercion> expr_variant_coercions;
        std::unordered_map<const void *, TraitCoercion> expr_trait_coercions;
        std::unordered_map<const void *, TraitDispatchInfo> expr_trait_dispatch;
        bool ok = false;
    };

    struct ResolveState {
        std::set<std::pair<std::string, std::string>> alias_resolving;
        std::set<std::pair<std::string, std::string>> struct_resolving;
        std::set<std::pair<std::string, std::string>> union_resolving;
        std::set<std::pair<std::string, std::string>> trait_resolving;
        std::set<std::pair<std::string, std::string>> value_resolving;
    };

    struct Program {
        std::unordered_map<std::string, ProgramModule> modules;
        std::vector<StructInfo> structs;          // global; struct_index is unique across all modules
        std::vector<EnumInfo> enums;
        std::vector<UnionInfo> unions;            // global; union_index is unique across all modules
        std::vector<TraitInfo> traits;            // global; trait_index is unique across all modules
        std::vector<FunctionTypeInfo> fn_signatures; // global; fn_index is unique across all modules
        std::vector<ResolvedType> pointer_pointees; // global; pointee_index is unique across all modules
        std::vector<ArrayInfo> arrays;             // global; array_index is unique across all modules
        std::vector<SliceInfo> slices;             // global; slice_index is unique across all modules
        ResolveState resolve_state;
        bool ok = false;

        // Trait-impl registries. Kept separate from ProgramModule::methods (which has
        // no trait dimension and would silently collide with same-named inherent
        // methods) and Program-level (not per-module) because coherence/duplicate-impl
        // checks must see across every module in the program, not just one.
        //
        // Keyed by (type_module, type_name) rather than a bare type-name string, since
        // two unrelated modules may each declare a type with the same local name — a
        // string-only key would incorrectly conflate them.
        std::map<std::pair<std::string, std::string>, std::vector<TraitImplInfo>> trait_impls_by_type;
        // Dedup key (trait_module, trait_name, type_module, type_name) -> first-seen
        // location, used purely to detect and report duplicate impls.
        std::map<std::tuple<std::string, std::string, std::string, std::string>, SourceLocation> trait_impl_registry;

        // Bounds-checked table lookups, returning nullptr for an out-of-range
        // index instead of the undefined behavior of raw operator[]. Under
        // normal compilation every *_index on a ResolvedType is guaranteed
        // valid by construction (declare_type() in sema_declare.cpp always
        // allocates the slot synchronously before the index is ever handed
        // out), but the LSP intentionally runs check_program() on ASTs with
        // unresolved imports and lex/parse errors that the CLI would never
        // let reach sema - callers touching a *_index that came from such a
        // program should use these instead of indexing the vectors directly.
        [[nodiscard]] auto struct_at(int index) const -> const StructInfo * {
            return index >= 0 && static_cast<size_t>(index) < structs.size() ? &structs[index] : nullptr;
        }
        [[nodiscard]] auto enum_at(int index) const -> const EnumInfo * {
            return index >= 0 && static_cast<size_t>(index) < enums.size() ? &enums[index] : nullptr;
        }
        [[nodiscard]] auto union_at(int index) const -> const UnionInfo * {
            return index >= 0 && static_cast<size_t>(index) < unions.size() ? &unions[index] : nullptr;
        }
        [[nodiscard]] auto trait_at(int index) const -> const TraitInfo * {
            return index >= 0 && static_cast<size_t>(index) < traits.size() ? &traits[index] : nullptr;
        }
        [[nodiscard]] auto fn_signature_at(int index) const -> const FunctionTypeInfo * {
            return index >= 0 && static_cast<size_t>(index) < fn_signatures.size() ? &fn_signatures[index] : nullptr;
        }
        [[nodiscard]] auto pointee_at(int index) const -> const ResolvedType * {
            return index >= 0 && static_cast<size_t>(index) < pointer_pointees.size() ? &pointer_pointees[index] : nullptr;
        }
        [[nodiscard]] auto array_at(int index) const -> const ArrayInfo * {
            return index >= 0 && static_cast<size_t>(index) < arrays.size() ? &arrays[index] : nullptr;
        }
        [[nodiscard]] auto slice_at(int index) const -> const SliceInfo * {
            return index >= 0 && static_cast<size_t>(index) < slices.size() ? &slices[index] : nullptr;
        }
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
    auto resolve_declared_type(const std::optional<ast::Type> &type, const std::optional<ast::Expr> &init,
                                const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                const SourceLocation &decl_loc) -> std::optional<ResolvedType>;
    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool;
    auto intern_pointer(Program &program, const ResolvedType &pointee) -> ResolvedType;
    auto intern_slice(Program &program, const ResolvedType &element) -> ResolvedType;
    auto intern_function_type(Program &program, FunctionTypeInfo sig) -> ResolvedType;
    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_global_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_macro_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> MacroSymbol &;
    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, std::optional<ResolvedType> expected, int loop_depth, int defer_loop_base = -1, bool fn_returns_error = false) -> ResolvedType;
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
