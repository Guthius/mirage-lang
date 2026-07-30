#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"
#include "module_resolver.hpp"
#include "resolved_type.hpp"
#include "symbol_table.hpp"

#include <format>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace sema {
    // A '--opt key=value' or environment-variable value coerced (per $option's/$env's
    // target-type coercion rules) or folded from a default expression: an integer/bool/
    // enum-underlying value, or a []u8 string. Used for '$option'/'$env' themselves and for
    // constant-folding 'when' conditions and '#link' data expressions that reference an
    // '$option'/'$env'-backed const. Defined here (ahead of VariantCoercion/AsmStmtInfo
    // etc. further below, which used to be its original home) so the generics structures
    // immediately following — which also need it — can be declared next to the
    // ResolvedType-adjacent structs above rather than split across the file.
    using ConstFoldValue = std::variant<int64_t, std::string>;

    // Substitution environment active while resolving one specific generic instantiation's
    // signature/layout/body: one entry per the generic declaration's own params, binding
    // each param name to either a concrete ResolvedType (a 'T: type' parameter) or a
    // concrete compile-time constant of the parameter's declared scalar type (an
    // 'N: usize'-style value parameter). Generalizes eval_integer_const_expr's existing
    // 'macro_args' name->AST-node substitution map (type_resolver.cpp, used for iota/
    // array-length const-folding) from "const-int folding only" to "any type/expr
    // resolution reachable while walking a generic decl's own AST." Small (== the decl's
    // own arity) — linear lookup by name is intentional, not a missed optimization.
    struct GenericBinding {
        std::string param_name;
        bool is_type = true;
        ResolvedType type_value{};       // valid when is_type
        ConstFoldValue const_value{};    // valid when !is_type
        ResolvedType const_value_type{}; // the param's declared scalar type, e.g. USize
    };
    using GenericBindingEnv = std::vector<GenericBinding>;

    // The resolved, cacheable form of one generic argument — no param name (unlike
    // GenericBinding above), since this is used as a monomorphization-cache key and for
    // RTTI/name-mangling, not for substitution during a single resolution pass.
    struct GenericArgValue {
        bool is_type = true;
        ResolvedType type_arg{};
        ConstFoldValue value_arg{};
        ResolvedType value_arg_scalar_type{};

        auto operator==(const GenericArgValue &) const -> bool = default;
    };

    // Tags a monomorphized struct/enum/union/bitset slot with where it came from: the
    // UNSPECIALIZED declaration's own (module, name) plus the concrete args this
    // instantiation was created with, in declared parameter order. Never set on a
    // non-generic slot (std::optional<GenericInstanceInfo> on each *Info struct below).
    struct GenericInstanceInfo {
        std::string decl_module;
        std::string decl_name;
        std::vector<GenericArgValue> args;
    };

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
        // Set only when this slot is a monomorphized instantiation of a generic 'type
        // Name[...] = struct {...}' declaration — see instantiate_generic_type.
        std::optional<GenericInstanceInfo> generic_instance;
        // Guards check_generic_struct_field_defaults_for_program's worklist. Only meaningful
        // when generic_instance is set: a non-generic struct's field defaults are checked by
        // check_struct_field_defaults_for_module instead, which walks declarations, not slots.
        bool field_defaults_checked = false;
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
        std::string module_path; // module where this enum is declared — see find_type_module_and_name
        ResolvedType underlying_type;
        std::vector<EnumFieldInfo> fields;
        bool layout_done = false;
        // See StructInfo::generic_instance above.
        std::optional<GenericInstanceInfo> generic_instance;
    };

    // 'bitset(EnumType, StorageType)' — a distinct nominal type over an integer,
    // representing a set of 'member_enum_type' variants stored as bits.
    // Bit index for variant v is 'v.value + 1' (see layout_bitset); this is
    // validated to fit within 'storage_bits' at declaration time.
    struct BitsetInfo {
        std::string module_path; // module where this bitset is declared — see find_type_module_and_name
        ResolvedType member_enum_type;
        ResolvedType storage_type;
        uint32_t storage_bits = 0;
        bool layout_done = false;
        // See StructInfo::generic_instance above.
        std::optional<GenericInstanceInfo> generic_instance;
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

        // Set on both the outer Ok/Failed wrapper synthesized for an 'error(...)'
        // type and (when there are 2+ error member types) the inner member-dispatch
        // union nested in its Failed variant. Never set on a user-declared union.
        // 'error_member_types' holds the error(...) type's own member list (the
        // enum/union Ts named between the parens) in the order they were declared
        // in the ORIGINAL error(...) spelling that first synthesized this union
        // (used for the inner union's own variant naming and for diagnostics that
        // don't have the originating ast::ErrorType node in scope). Not meaningful
        // (empty) unless is_error_union is true.
        bool is_error_union = false;
        std::vector<ResolvedType> error_member_types;

        // '?error(...)' — an error a caller may leave unhandled (see intern_error_union).
        // Optionality is modelled as TYPE IDENTITY, not as a per-return-slot signature flag:
        // '?error(E)' and 'error(E)' intern to two distinct unions with two distinct
        // union_indexes. That is what makes a trait/impl '?' mismatch fall out of the
        // existing return_types comparison, and what lets reflection report it
        // (Type_Info.Error_Type.is_optional). Set on the OUTER Ok/Failed wrapper only —
        // the inner member-dispatch union is an implementation detail with no callers.
        // Deliberately NOT part of error_union_is_subset or the try/return_err rules:
        // dropping or adding the marker is semantically inert on a value, because it only
        // ever describes an obligation at the CALL SITE.
        bool is_optional = false;

        // See StructInfo::generic_instance above. Never set alongside is_error_union —
        // error(...) unions are synthesized, not user-declared generic types.
        std::optional<GenericInstanceInfo> generic_instance;
    };

    // The tag field of a tagged union is always u32.
    inline const ResolvedType TAG_TYPE{.kind = TypeKind::U32};

    // Signature stored for a function-pointer type: fn(ParamTypes) -> ReturnTypes
    struct FunctionTypeInfo {
        std::vector<ResolvedType> param_types;
        std::vector<std::string> param_names; // parallel to param_types; "" = unnamed.
                                               // Cosmetic only (LSP hover) — deliberately
                                               // excluded from intern_function_type's equality
                                               // check, so differing names for the same
                                               // structural signature don't cause duplicate
                                               // interning; first-parsed name set wins.
        std::vector<ResolvedType> return_types; // empty=void, 1=single, 2+=multi
        bool is_variadic = false;
    };

    struct MethodInfo {
        const ast::ImplDecl::Function *decl = nullptr;
        std::string impl_module;               // module where this impl is defined
        std::string type_name;                 // type name this method belongs to
        ResolvedType self_type;                // resolved type of self (the struct/enum type)
        // Non-null only when the enclosing 'impl' block carries its own generic_params
        // (an ImplDecl::Function never has its own — see ast.hpp). Points into the AST
        // (same lifetime as 'decl'). Needed because a method body's 'T'/'N' references bind
        // to the IMPL block's own chosen param names (which may differ from the target
        // type's own, e.g. 'impl List[U: type] { ... }'), not the type's — see
        // instantiate_generic_method.
        const std::vector<ast::GenericParam> *impl_generic_params = nullptr;
        std::vector<ResolvedType> param_types; // non-self params
        std::vector<ResolvedType> return_types;
        bool is_mut_self = false;
        bool is_pub = false;
        bool is_resolved = false;
        bool is_variadic = false;             // true if the last param is native '...T'
        ResolvedType variadic_element_type{}; // T; only meaningful if is_variadic

        // Count of leading non-defaulted params (== param_types.size() for a
        // trait-impl method, which is never allowed to declare its own defaults —
        // see resolve_trait_impl_signatures_for_program).
        size_t required_params = 0;
        std::vector<bool> param_default_is_const; // parallel to param_types

        // Set (to the trait's local name) only when this MethodInfo came from an
        // 'impl TRAIT for TYPE' block, rather than a bare 'impl TYPE' block. Codegen
        // uses this to route function lookups to the trait_functions_ table instead
        // of functions_, since trait-impl methods are mangled/keyed distinctly to
        // avoid colliding with a same-named inherent method (see Generator::run()).
        std::optional<std::string> trait_name;
        std::string trait_module; // only meaningful if trait_name has a value

        // Set alongside trait_name: the trait's own TraitInfo/TraitMethodInfo this
        // method backs, so a call resolved via this MethodInfo (not via a dyn Trait
        // handle) can still source its default parameter values from the trait's
        // signature, never from the impl (which is never allowed to declare its own).
        int trait_index = -1;
        int trait_method_index = -1;
    };

    // Identifies one generic function/method instantiation for cache lookup: the
    // unspecialized declaration's own (module, name) plus its concrete args, in declared
    // parameter order. Mirrors GenericInstanceInfo above but as a lookup key rather than a
    // tag stored on the result.
    struct GenericInstanceKey {
        std::string module_path;
        std::string decl_name;
        std::vector<GenericArgValue> args;

        auto operator==(const GenericInstanceKey &) const -> bool = default;
    };

    // One concrete instantiation of a generic function OR method — both share this one
    // shape (a method additionally sets 'self_type' and 'impl_decl'; a free function
    // instantiation leaves those unset and uses 'decl' instead). Signature (param/return
    // types, mangled name) is resolved eagerly when the instance is first created;
    // 'body_checked' guards against re-checking/re-emitting the body if the same concrete
    // instantiation is reached again from a different call site.
    struct GenericFunctionInstance {
        const ast::FunctionDecl *decl = nullptr;            // set for a free-function instance
        const ast::ImplDecl::Function *impl_decl = nullptr;  // set for a method instance
        std::string module_path; // module where the generic decl itself lives
        std::vector<GenericArgValue> args;
        std::optional<ResolvedType> self_type; // set only for a method instance
        // Set only for a method instance (mirrors MethodInfo::impl_generic_params) — 'decl's
        // own generic_params is used directly for a free-function instance instead, since
        // FunctionDecl carries its own list.
        const std::vector<ast::GenericParam> *generic_params_for_method = nullptr;
        std::vector<ResolvedType> param_types; // non-self params
        std::vector<ResolvedType> return_types;
        std::string mangled_name; // e.g. "make_fixed__16", "List__i32::reserve" — see codegen
        bool body_checked = false;
        bool is_variadic = false;
        ResolvedType variadic_element_type{};
        size_t required_params = 0;
        std::vector<bool> param_default_is_const; // parallel to param_types
        // This instantiation's OWN per-node records live in Program::generic_fn_instance_exprs,
        // keyed by this instance's index — not inline here, only because ExprSideTables names
        // coercion structs declared further down this header. Reach them via
        // Program::exprs_for_fn_instance().
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
        const ast::TraitType::Method *decl = nullptr; // needed to reach param default_value AST nodes
        size_t required_params = 0;                    // count of leading non-defaulted params
        std::vector<bool> param_default_is_const;        // parallel to params
    };

    // One resolved entry in a trait's composition relationship (direct or transitive).
    struct ComposedTraitRef {
        std::string module_path;
        std::string name;
        int trait_index = -1;
    };

    struct TraitInfo {
        std::string module_path;            // module where 'type Name = trait {...}' is declared
        std::string name;                   // the trait's own unqualified name — needed for
                                             // composition diagnostics (cycle chains, collision
                                             // messages, redundancy warning); nothing else on
                                             // TraitInfo/ast::TraitType carries it.
        std::vector<TraitMethodInfo> methods; // FLATTENED: own methods (decl order) followed by
                                             // each directly-composed trait's own (already-
                                             // flattened) methods, composition-list order,
                                             // skipping names already present — this order IS
                                             // the vtable layout and must never be re-derived.
        bool layout_done = false;

        std::vector<ComposedTraitRef> composed_traits;  // resolved, deduped DIRECT composition
                                                         // list, source order.
        std::vector<ComposedTraitRef> component_traits; // deduped TRANSITIVE closure of every
                                                         // trait this one composes (excludes
                                                         // self). This vector's order IS the
                                                         // synthesized sub-vtable ordinal
                                                         // assignment — codegen and every
                                                         // coercion-site sema check read this
                                                         // same vector, never re-derive an order.
        bool composition_done = false;                  // memoization guard, parallel to layout_done
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
        // The trait actually impl'd for the pointee (== trait_index for a direct exact-match
        // impl; the composing trait's index when this coercion was resolved indirectly through
        // its composition — see the pointer-to-component search in check_expr's expected-type
        // tail). Codegen uses (provider_trait_index, trait_index, pointee type) to find either
        // the ordinary vtable (provider == trait_index) or the right synthesized sub-vtable.
        int provider_trait_index = -1;
    };

    // Records an implicit HANDLE-TO-HANDLE composed-trait narrowing coercion decided by
    // check_expr's expected-type tail (sibling to TraitCoercion above, which is
    // pointer-to-handle). 'slot_index' is the pre-computed GEP index into the SOURCE handle's
    // vtable — methods.size() of the source trait, plus the target's ordinal within the source
    // trait's own component_traits — codegen reads it directly, never re-derives it.
    struct TraitHandleCoercion {
        int from_trait_index = -1;
        int to_trait_index = -1;
        int slot_index = -1;
    };

    // Records an implicit value-to-'any' coercion decided by check_expr's expected-type
    // tail (sibling to TraitCoercion above). 'source_type' is the coerced value's own
    // natural type — codegen uses it to look up the type's id (Program::type_ids) and to
    // decide how to materialize the 'data' word (take the value's address, or use it
    // directly if it's already an 'anyptr' — see emit_any_coercion).
    struct AnyCoercion {
        ResolvedType source_type;
    };

    // Records where a '.method()' call was resolved against a trait's method list via
    // an actual dyn-handle receiver (TypeKind::Trait), rather than a concrete MethodInfo.
    // 'method_order_index' is the index into TraitInfo::methods — the vtable slot.
    struct TraitDispatchInfo {
        int trait_index = -1;
        int method_order_index = -1;
    };

    // Records that a match/switch operand expression is an error(...) value being matched
    // transparently on its inner representation — see check_expr's MatchExpr/SwitchStmt
    // handling in sema_check.cpp. 'wrapper_type' is the operand's own (Ok/Failed) type;
    // 'effective_type' is what sema actually dispatched the arms against (the single error
    // member type, or the synthesized inner dispatch union for 2+ members). Codegen uses
    // this to GEP through the Failed payload to 'effective_type''s bytes before emitting the
    // ordinary (unmodified) tagged-union/enum match/switch code.
    struct ErrorMatchUnwrap {
        ResolvedType wrapper_type;
        ResolvedType effective_type;
    };

    // Records that a call's trailing '?error(...)' return slot was left unbound by the
    // caller — see ExprSideTables::call_dropped_optional_error. 'slot_index' is that slot's
    // index in the callee's return list, so codegen knows the call's true arity (== index +
    // 1) without re-deriving the callee's signature: 0 means the error WAS the only return
    // value (statement position), 1 means one real value survives the drop.
    struct DroppedOptionalError {
        ResolvedType error_type;
        size_t slot_index = 0;
    };

    // Sema's resolved view of one 'asm { ... }' instruction, computed by check_asm_stmt
    // (sema_check.cpp) and consumed by codegen's emit_asm_stmt — parallel to
    // ast::AsmInstruction::operands; populated only for ast::AsmVariableOperand entries
    // (register/immediate operands carry std::nullopt here, their type being implicit in the
    // AST node itself).
    struct AsmInstructionInfo {
        std::vector<std::optional<ResolvedType>> operand_types;
    };

    // Sema's resolved view of a whole 'asm { ... }' block: per-instruction operand types (for
    // codegen's input/output classification) plus the clobber set computed from the Tier-1/
    // Tier-2 operand-direction analysis and each mnemonic's implicit clobbers.
    struct AsmStmtInfo {
        std::vector<AsmInstructionInfo> instructions; // parallel to ast::AsmStmt::instructions
        std::set<std::string> clobbered_families;     // 64-bit root register names, e.g. "rax"
        bool clobbers_memory = false;
    };

    // The true origin of a bare-import alias: 'module_path'/'symbol_name' (both resolved,
    // canonical) are used by every redirect/skip site that needs to re-resolve/emit the
    // real underlying decl in its correct context; 'source_path' is the raw string as
    // written in the 'import("...")' call that introduced this alias, kept only so
    // diagnostics (the bare-vs-bare collision message) can show the module path the way
    // the user actually wrote it, not a resolved absolute filesystem path.
    struct BareImportOrigin {
        std::string module_path;
        std::string symbol_name;
        std::string source_path;
    };

    // Everything sema records ABOUT AN EXPRESSION/STATEMENT NODE, keyed by that node's
    // address (get_expr_key, or the raw node pointer for the three decl-shaped maps).
    //
    // Split out of ProgramModule because a module is the wrong owner for these: a generic
    // function's body AST is SHARED by every monomorphization of it, so keying on the node
    // alone means instantiation #2 overwrites instantiation #1's entries and codegen then
    // emits #2's types into #1's body. Each GenericFunctionInstance therefore owns a set of
    // these, and each generic type instantiation gets one via
    // Program::generic_type_instance_exprs; ProgramModule keeps one for ordinary
    // (non-generic) code. Program::active_expr_tables selects between them — see
    // expr_tables() below, whose read/write asymmetry is load-bearing.
    //
    // A map belongs here only if its value can legitimately DIFFER between two
    // instantiations of the same AST node. Maps written at declare time (inline_import_paths,
    // when_decl_selected) or whose value is instantiation-independent (expr_option_values —
    // a '$option'/'$env' build-time constant) deliberately stay on ProgramModule.
    struct ExprSideTables {
        std::unordered_map<const void *, ResolvedType> expr_types;
        std::unordered_map<const void *, VariantCoercion> expr_variant_coercions;
        std::unordered_map<const void *, TraitCoercion> expr_trait_coercions;
        std::unordered_map<const void *, TraitHandleCoercion> expr_trait_handle_coercions;
        std::unordered_map<const void *, AnyCoercion> expr_any_coercions;
        std::unordered_map<const void *, TraitDispatchInfo> expr_trait_dispatch;
        // A call-site CallExpr node -> the index into Program::generic_fn_instances it
        // resolved to (explicit or inferred instantiation of a generic function, or a
        // generic method call) — populated by check_expr's CallExpr handling, consumed by
        // codegen to find the pre-resolved, pre-mangled callee instead of re-deriving it.
        // The clearest case for this struct existing: a generic function calling another
        // generic function resolves to a DIFFERENT instance index per enclosing instance,
        // on the very same call node.
        std::unordered_map<const void *, size_t> expr_generic_fn_instance;
        // 'type_info_of(type_of(T))' EXPRESSION -> T's compile-time-constant type id, present
        // only for this exact syntactic shape (see check_expr's TypeInfoOfExpr case) — the fast
        // path that resolves directly to a specific '__type_info_<id>' global at codegen time,
        // rather than the generic runtime binary-search table lookup.
        std::unordered_map<const void *, uint64_t> expr_type_info_const_id;
        // 'type_of(...)' EXPRESSION -> its operand's resolved type. Needed because a type-name
        // operand (e.g. 'type_of(Point)') never goes through check_expr itself (see check_expr's
        // TypeOfExpr case, mirroring SizeOfExpr) and so has no expr_types entry of its own -
        // TypeInfoOfExpr's 'type_info_of(type_of(T))' fast-path detection reads this instead of
        // trying (and failing) to look up the inner operand's expr_types entry directly.
        std::unordered_map<const void *, ResolvedType> expr_type_of_operand_type;
        std::unordered_map<const void *, ErrorMatchUnwrap> expr_error_match_unwrap;
        // A CallExpr whose callee's trailing '?error(...)' slot the caller did NOT bind ->
        // that slot's type. Codegen turns each entry into the check the caller didn't write:
        // if the returned error is Failed, panic with its variant name and this call's
        // source location (see emit_unhandled_error_check). Absent means "nothing to do" —
        // either the callee's error isn't optional, or the caller bound it (including
        // binding it to '_', which is the deliberate silent opt-out).
        //
        // Keyed by the CallExpr itself, NOT get_expr_key: the group-declaration path only
        // ever sees the unwrapped node, and this map is written from both paths. Same
        // convention as expr_generic_fn_instance's writes in check_group_call_returns.
        std::unordered_map<const ast::CallExpr *, DroppedOptionalError> call_dropped_optional_error;
        // 'when' EXPRESSION -> which branch (true=then_expr, false=else_expr) a
        // compile-time-constant condition folded to; absent if the condition is a runtime
        // value (codegen emits a runtime branch/PHI in that case, like an ordinary ternary).
        // Instance-varying: 'when N > 4' picks a different branch per 'N'.
        std::unordered_map<const void *, bool> expr_when_selected;
        // 'when' STATEMENT -> which branch is live (always present; a when statement's
        // condition is always required to be a compile-time constant).
        std::unordered_map<const ast::WhenStmt *, bool> when_stmt_selected;
        // 'asm { ... }' node -> sema's resolved operand types + computed clobber set. Populated
        // by check_asm_stmt (sema_check.cpp), consumed by codegen's emit_asm_stmt.
        std::unordered_map<const ast::AsmStmt *, AsmStmtInfo> asm_stmt_info;
        // 'asm -> reg [: type] { ... }' EXPRESSION node -> the same AsmStmtInfo shape as above,
        // in a separate map (an AsmExpr never shares a key with any AsmStmt). Populated by
        // check_asm_expr, consumed by codegen's emit_asm_expr.
        std::unordered_map<const ast::AsmExpr *, AsmStmtInfo> asm_expr_info;
    };

    struct ProgramModule {
        SymbolTable symbols;
        // type_name -> method_name -> MethodInfo
        std::unordered_map<std::string, std::unordered_map<std::string, MethodInfo>> methods;
        // alias_name (as it appears, unqualified, in THIS module's own 'symbols' table)
        // -> its true origin — populated only for names introduced by a bare import
        // (sema_declare.cpp's declare_bare_import). An alias entry shares its underlying
        // AST decl (and, for types, its global struct/enum/union/bitset/trait index) with
        // the origin, so every per-module bulk pass below (signature/value resolution,
        // body/attribute/field-default checking, codegen emission) must consult this map
        // before processing a symbol generically — either to skip it (the origin's own
        // natural pass already does the real work) or to redirect resolution to the
        // origin's path/name — rather than accidentally re-resolving/re-checking/
        // re-emitting the SAME decl under this module's (wrong) context.
        std::unordered_map<std::string, BareImportOrigin> bare_import_origins;
        // Per-node records for this module's ORDINARY (non-generic) code. A generic
        // instantiation's nodes land in its own instance-owned set instead — never here.
        ExprSideTables exprs;
        // '$option(...)'/'$env(...)' expression -> its resolved compile-time value, cached
        // by check_expr's OptionExpr/EnvExpr cases so later constant-folding (when
        // conditions, '#link' data) never re-runs coercion/diagnostics for the same node.
        // Stays module-level: a build-time config value does not vary by instantiation.
        std::unordered_map<const void *, ConstFoldValue> expr_option_values;
        // module-scope 'when' DECLARATION -> which branch is live.
        std::unordered_map<const ast::WhenDecl *, bool> when_decl_selected;
        // ImportExpr node (found nested under a '.field' MemberExpr chain by
        // ast::find_leaf_import, e.g. `import("...").target_arch`) -> its resolved module
        // path, cached at declare time (declare_global) since the check phase's
        // try_resolve_namespace_chain only has access to sema::Program, not the
        // ast::Program::module_imports map the resolution needs.
        std::unordered_map<const ast::ImportExpr *, std::string> inline_import_paths;
        bool ok = false;
    };

    struct ResolveState {
        std::set<std::pair<std::string, std::string>> alias_resolving;
        std::set<std::pair<std::string, std::string>> struct_resolving;
        std::set<std::pair<std::string, std::string>> union_resolving;
        std::set<std::pair<std::string, std::string>> bitset_resolving;
        std::set<std::pair<std::string, std::string>> trait_resolving;
        std::set<std::pair<std::string, std::string>> value_resolving;
        std::set<std::pair<std::string, std::string>> fn_signature_resolving;
        // Cycle guard for the reentrant module-scope-'when' symbol declaration helper
        // (ensure_module_declared, sema_declare.cpp) — a 'when' condition that references
        // another module's const can force that module's symbol table to be built
        // on-demand, out of Program::modules' unordered iteration order; this detects a
        // circular dependency between modules' 'when' conditions.
        std::set<std::string> when_module_declaring;
        // Cycle guard for instantiate_generic_type, keyed by (decl, concrete args) — a
        // recursive generic type (e.g. 'type Node[T: type] = struct { next: *Node[T] }')
        // is fine (pointer indirection breaks the cycle, same as any non-generic recursive
        // struct — the pointee is resolved lazily, no layout forced), but a genuinely
        // infinite instantiation chain (no indirection anywhere) must still be caught.
        // Linear-scan (small, rare), not a set, since GenericInstanceKey has no operator<.
        std::vector<GenericInstanceKey> generic_type_resolving;
        // Ordered ancestor stack of trait_index currently mid-flattening (layout_trait's
        // composition-resolution step, type_resolver.cpp) — pushed/popped around that step
        // only. Distinct from trait_resolving above: that set exists purely to short-circuit
        // a trait referencing ITSELF in a METHOD SIGNATURE (not a true cycle, since a trait
        // handle is a fixed-size fat pointer); this stack instead detects a genuine trait
        // COMPOSITION cycle ('trait(A)' -> ... -> 'trait(A)'), which must be a hard error
        // naming the full chain.
        std::vector<int> trait_composition_stack;
    };

    // RAII helpers for the cycle guards above.
    //
    // Every guard used to be a hand-written insert(key) ... erase(key) (or push_back/pop_back)
    // pair spanning a large recursive body. Any exception thrown in between -- an .at() miss,
    // a bad std::get, an uncaught std::stoll -- left the key permanently marked "resolving",
    // so every later attempt to resolve that type reported a spurious "circular dependency".
    // For the vector stacks the leak is worse: active_generic_env_stack holds a pointer to a
    // stack-local env, and a missed pop leaves it dangling for the next .back() read.
    //
    // These make the unwind path correct by construction. lsp/server.cpp runs sema on a
    // worker thread with no try/catch anywhere above it, so a poisoned guard would otherwise
    // persist for the lifetime of the editor session.
    template <typename Set, typename Key>
    class ScopedResolveMark {
      public:
        ScopedResolveMark(Set &set, Key key) : set_(set), key_(std::move(key)) { set_.insert(key_); }
        ~ScopedResolveMark() { set_.erase(key_); }

        ScopedResolveMark(const ScopedResolveMark &) = delete;
        auto operator=(const ScopedResolveMark &) -> ScopedResolveMark & = delete;

      private:
        Set &set_;
        Key key_;
    };

    template <typename Set, typename Key>
    ScopedResolveMark(Set &, Key) -> ScopedResolveMark<Set, Key>;

    template <typename Stack>
    class ScopedResolvePush {
      public:
        template <typename Value>
        ScopedResolvePush(Stack &stack, Value &&value) : stack_(stack) {
            stack_.push_back(std::forward<Value>(value));
        }
        ~ScopedResolvePush() { stack_.pop_back(); }

        ScopedResolvePush(const ScopedResolvePush &) = delete;
        auto operator=(const ScopedResolvePush &) -> ScopedResolvePush & = delete;

      private:
        Stack &stack_;
    };

    template <typename Stack, typename Value>
    ScopedResolvePush(Stack &, Value &&) -> ScopedResolvePush<Stack>;

    // Compiler-driver-supplied configuration read by '$option' during sema. Threaded
    // through check_program the same way codegen::Options is threaded through
    // codegen::generate (see codegen.hpp) — a single defaulted struct parameter, copied
    // once into Program::options at check_program's entry, then read via the
    // already-universally-threaded Program& rather than adding a new parameter to every
    // check_expr/check_stmt call site.
    struct Options {
        std::unordered_map<std::string, std::string> opt_values;
        // Type-check every generic declaration's body once at its point of definition, with
        // its parameters bound to Opaque (see check_generic_templates_for_program). On by
        // default; '--no-eager-generic-check' turns it off so a false positive in that pass
        // can never hard-block a build.
        bool eager_generic_check = true;
    };

    enum class LinkCategory : uint8_t { Lib, System, Flag };

    struct LinkDirective {
        LinkCategory category;
        std::string data;
        std::string source_module; // for diagnostics
        SourceLocation location;
    };

    struct Program {
        std::unordered_map<std::string, ProgramModule> modules;
        std::vector<StructInfo> structs;          // global; struct_index is unique across all modules
        std::vector<EnumInfo> enums;
        std::vector<UnionInfo> unions;            // global; union_index is unique across all modules
        std::vector<BitsetInfo> bitsets;          // global; bitset_index is unique across all modules
        std::vector<TraitInfo> traits;            // global; trait_index is unique across all modules
        std::vector<FunctionTypeInfo> fn_signatures; // global; fn_index is unique across all modules
        std::vector<ResolvedType> pointer_pointees; // global; pointee_index is unique across all modules
        std::vector<ArrayInfo> arrays;             // global; array_index is unique across all modules
        std::vector<SliceInfo> slices;             // global; slice_index is unique across all modules
        // Interning cache for synthesized 'error(...)' union types: (sorted member-type
        // list, is_optional) — the canonical identity, order-independent in the member
        // list (see intern_error_union) -> the union_index of the synthesized Ok/Failed
        // wrapper. Two 'error(...)' spellings naming the same set of member types (in any
        // order) with the same '?' marking intern to the same union. The '?' IS part of the
        // key: 'error(E)' and '?error(E)' are distinct types (see UnionInfo::is_optional).
        struct ErrorUnionKey {
            std::vector<ResolvedType> members;
            bool is_optional = false;

            auto operator==(const ErrorUnionKey &) const -> bool = default;
        };
        std::vector<std::pair<ErrorUnionKey, int>> error_unions;
        // 'type_of's identity table: every distinct ResolvedType that has ever been the
        // operand of a 'type_of'/'any' coercion gets a stable id here, assigned lazily in
        // encounter order (see intern_type_id). Ids 1-15 are pre-seeded for the builtin
        // scalar kinds by seed_builtin_type_ids (called once at the top of check_program);
        // 'next_type_id' starts at 16 for everything else. Codegen NEVER calls
        // intern_type_id itself — every id a program can ever reference is assigned
        // synchronously during sema's whole-program check_expr pass, so codegen only ever
        // reads this map (see codegen.cpp's type_of_operand).
        std::unordered_map<ResolvedType, uint64_t> type_ids;
        uint64_t next_type_id = 16;
        // Every type id for which 'type_info_of(type_of(T))' (the compile-time-constant
        // fast path — see check_expr's TypeInfoOfExpr case) appears anywhere in the
        // program, plus every source type of an 'any' coercion (so the generic runtime
        // binary-search table has real entries to find) — the set of types codegen's
        // declare_type_info_globals() must emit a 'Type_Info' global for.
        std::set<uint64_t> types_needing_info;
        std::map<uint64_t, ResolvedType> types_needing_info_types;
        ResolveState resolve_state;
        bool ok = false;

        // '--opt key=value' values supplied by the driver, read by '$option'.
        Options options;
        // Linker directives collected from '#link' declarations in live 'when' branches
        // (or unconditional module scope) across every module. Driver-specific consumption
        // is out of scope for the compiler itself — this is collection only.
        std::vector<LinkDirective> link_directives;
        // Module paths whose symbol table (build_symbol_table_for_module) has already run —
        // see ensure_module_declared (sema_declare.cpp) for why this must be reentrant
        // rather than a single flat loop over Program::modules.
        std::set<std::string> modules_declared;

        // (module_path, function_name) pairs, in the exact order the synthesized '_init'
        // (codegen.cpp) must call them: modules topologically sorted by actual cross-module
        // symbol references made from '@init' function bodies (see
        // validate_init_dependencies_for_program, sema_attributes.cpp), each module's own
        // '@init' functions kept in source declaration order. Empty if no '@init' function
        // exists anywhere in the program, or if a circular dependency was detected (in which
        // case 'ok' is false regardless).
        std::vector<std::pair<std::string, std::string>> init_call_order;

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

        // Generic type instantiation cache: (unspecialized decl's module+name, concrete
        // args) -> the monomorphized ResolvedType, linear-scan-and-reuse exactly like
        // intern_pointer/intern_slice's existing structural-interning idiom (type_resolver.cpp)
        // extended to a keyed lookup instead of plain equality. Populated by
        // instantiate_generic_type; consulted before ever allocating a new struct/enum/
        // union/bitset slot for a given (decl, args) pair.
        std::vector<std::pair<GenericInstanceKey, ResolvedType>> generic_type_instance_lookup;
        // NOTE: there is deliberately no 'generic_type_instances_needed' liveness set here,
        // unlike generic_fn_instances_needed below. One existed and was written on every type
        // instantiation, but nothing ever read it: codegen's declare_structs()/declare_unions()
        // iterate all of program.structs/unions/enums/bitsets unconditionally, so a
        // monomorphized type slot is emitted because it exists, not because sema marked it
        // reached. Its doc comment claimed the opposite, which was the actual harm.
        //
        // Over-inclusive codegen is correct, just not minimal — a generic type instantiated
        // only in an unselected 'when' branch is never created in the first place, since sema
        // never resolves that branch. If dead-type elimination is wanted later, reintroduce
        // the set *and* the codegen loop that consults it in the same change.

        // Generic function/method instantiation cache + liveness, mirroring the type-side
        // tables immediately above. 'generic_fn_instance_lookup' maps a GenericInstanceKey to
        // an index into 'generic_fn_instances' (linear scan, same interning idiom). Each
        // instance is heap-allocated (unique_ptr, not stored by value) so a REFERENCE
        // returned by instantiate_generic_function/instantiate_generic_method stays valid
        // even if a later, nested/recursive instantiation triggers another push_back on this
        // vector — only the vector of pointers would reallocate, never the pointee objects.
        std::vector<std::pair<GenericInstanceKey, size_t>> generic_fn_instance_lookup;
        std::vector<std::unique_ptr<GenericFunctionInstance>> generic_fn_instances;
        std::set<size_t> generic_fn_instances_needed;

        // Ambient "currently active generic substitution" stack, pushed/popped around
        // checking one generic function/method instance's body (see
        // check_generic_function_instance_body, sema_check.cpp). Exists because check_expr/
        // check_stmt's own internals (a local var decl's type annotation, size_of's operand,
        // a cast target, ...) all eventually call type_resolver.cpp's free 'resolve_type'
        // function, which constructs its own fresh Resolver with no way to receive an
        // explicit generic_env parameter without threading one through every check_expr/
        // check_stmt call site in the compiler. 'resolve_type'/'resolve_declared_type'
        // consult the top of this stack as a fallback instead — the same role
        // Resolver::generic_env plays when generic_env IS available directly (struct/enum/
        // union/bitset field resolution, a generic instance's own signature resolution via
        // resolve_type_with_generic_env). A stack (not a single pointer) because checking one
        // instance's body can recursively trigger another instantiation before returning.
        std::vector<const GenericBindingEnv *> active_generic_env_stack;

        // Per-instantiation expression records (see ExprSideTables). Heap-allocated and
        // lazily created so a pointer handed out here survives any later insertion, and so
        // instances that never check a body cost nothing.
        //   - fn:   keyed by index into 'generic_fn_instances'
        //   - type: keyed by the GLOBAL struct/enum/union/bitset slot index. Keyed by slot
        //           rather than stored on *Info because 'structs' is a plain vector that
        //           reallocates mid-check (check_generic_struct_field_defaults_for_program
        //           already re-fetches by index for exactly this reason).
        std::unordered_map<size_t, std::unique_ptr<ExprSideTables>> generic_fn_instance_exprs;
        std::unordered_map<int, std::unique_ptr<ExprSideTables>> generic_type_instance_exprs;

        auto exprs_for_fn_instance(size_t idx) -> ExprSideTables & {
            auto &slot = generic_fn_instance_exprs[idx];
            if (!slot) slot = std::make_unique<ExprSideTables>();
            return *slot;
        }
        auto exprs_for_type_instance(int slot_index) -> ExprSideTables & {
            auto &slot = generic_type_instance_exprs[slot_index];
            if (!slot) slot = std::make_unique<ExprSideTables>();
            return *slot;
        }
        // Read-only lookups for codegen, which holds a 'const Program &' and must not create.
        // Null means "nothing was ever checked for this instantiation".
        auto find_fn_instance_exprs(size_t idx) const -> const ExprSideTables * {
            const auto it = generic_fn_instance_exprs.find(idx);
            return it == generic_fn_instance_exprs.end() ? nullptr : it->second.get();
        }
        auto find_type_instance_exprs(int slot_index) const -> const ExprSideTables * {
            const auto it = generic_type_instance_exprs.find(slot_index);
            return it == generic_type_instance_exprs.end() ? nullptr : it->second.get();
        }

        // Selects which ExprSideTables the node currently being checked/emitted belongs to.
        // Pushed by ScopedGenericScope (sema) and ScopedEmitExprs (codegen); empty means
        // "ordinary module-level code".
        std::vector<ExprSideTables *> active_expr_tables;

        // Non-zero while the eager pass is checking a generic DECLARATION's body with its
        // parameters bound to Opaque. Suppresses side effects that would otherwise leak
        // template-only artifacts into the real program — specifically, marking a concrete
        // instantiation as needing emission. 'fn f[T]() { g[i32]() }' must not cause 'g[i32]'
        // to be emitted just because 'f' was checked; only a real call to 'f' does that.
        // A counter, not a flag, because checking one template can reach another.
        int template_check_depth = 0;

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
        [[nodiscard]] auto bitset_at(int index) const -> const BitsetInfo * {
            return index >= 0 && static_cast<size_t>(index) < bitsets.size() ? &bitsets[index] : nullptr;
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

    // Typestate for an error(...)-typed local: whether it's currently known to be in the
    // Ok/Failed tag state. NotApplicable for every non-error-typed binding. Block-scoped —
    // LocalScope's existing copy-on-block-entry semantics (see check_stmt's BlockStmt/
    // ForInStmt/switch-arm handling) give this the reset-on-scope-exit behavior for free;
    // IfStmt/WhileStmt additionally copy explicitly (a prerequisite fix — they used to pass
    // 'locals' straight through by reference) so branch-local narrowing can't leak sideways.
    enum class ErrorState : uint8_t { NotApplicable, Unknown, Failed, Ok };

    // Why an error local's state is Unknown. Purely for diagnostics: three quite different
    // situations funnel into the same "cannot match on an error value of unknown state"
    // message, and without this the advice it gives ("check it first") is the thing the user
    // has often already done -- with a condition whose SHAPE is what defeated the narrowing.
    enum class ErrorUnknownReason : uint8_t {
        // Never checked, or the reason was not recorded.
        Unchecked,
        // Branched on, but by a condition the narrowing rules draw no conclusion from:
        // a '||', or the error variable behind a member access, deref, call or comparison.
        UncheckedConditionShape,
        // '&err' was taken; the callee may have overwritten it.
        AddressTaken,
        // Two branches disagreed about the state and the join could not keep either.
        BranchesDisagreed,
    };

    struct LocalBinding {
        ResolvedType type;
        bool is_mut = false;
        ErrorState err_state = ErrorState::NotApplicable;
        ErrorUnknownReason err_unknown_reason = ErrorUnknownReason::Unchecked;
    };

    using LocalScope = std::unordered_map<std::string, LocalBinding>;

    inline auto get_expr_key(const ast::Expr &expr) -> const void * {
        return std::visit([](const auto &v) -> const void * { return &v; }, expr);
    }

    // Which ExprSideTables a node's record belongs in. WRITES go to the innermost active
    // scope: while a generic instantiation is being checked or emitted, every node it walks
    // is that instantiation's, so the module's tables must not be touched at all (writing
    // there is the monomorphization-collision bug this split exists to fix).
    inline auto expr_tables_for_write(Program &program, const std::string &module_path) -> ExprSideTables & {
        if (!program.active_expr_tables.empty()) return *program.active_expr_tables.back();
        // operator[], not at(): two pre-existing write sites (the 'when' expression and
        // statement selectors) already used the module-creating form, and a throw here would
        // be a hard crash where a default-constructed module is harmless.
        return program.modules[module_path].exprs;
    }

    // READS are asymmetric with writes: they check the active scope first, then FALL BACK to
    // the module. The fallback is required — checking an instance body legitimately reads
    // nodes that were recorded module-level long before any instance existed. The clearest
    // case is evaluate_const_value recursing into a global const's initializer, whose entries
    // resolve_values_for_module wrote into the module's tables; a top-of-stack-only read
    // would miss them and the fold would fail with a spurious "not a compile-time constant".
    //
    // The fallback cannot mask a stale-instance read, because writes never reach the module's
    // tables while a scope is active — the two sets are disjoint by construction. Keep that
    // invariant if you add a write site.
    // Returns a pointer to the record, or nullptr if neither scope has one.
    template <typename Map, typename Key>
    auto find_expr_record(const Program &program, const std::string &module_path,
                           Map ExprSideTables::*member, const Key &key) -> const typename Map::mapped_type * {
        if (!program.active_expr_tables.empty()) {
            const auto &active = (*program.active_expr_tables.back()).*member;
            if (const auto it = active.find(key); it != active.end()) return &it->second;
        }
        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) return nullptr;
        const auto &fallback = mod_it->second.exprs.*member;
        if (const auto it = fallback.find(key); it != fallback.end()) return &it->second;
        return nullptr;
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

    auto check_program(const ast::Program &program, DiagnosticEngine &diag, const Options &options = {}) -> Program;
    // 'ast_program' is only needed by callers that may run before every module is
    // guaranteed to have been declared (the reentrant module-scope 'when' declare
    // pass) — passing it lets a cross-module named type declare its target module
    // on demand instead of failing with "module not found" purely because of
    // Program::modules' unordered iteration order. Every other (post-declare-phase)
    // caller safely omits it.
    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag, const ast::Program *ast_program = nullptr) -> ResolvedType;
    // Like resolve_type, but resolves inside 'env' — used for a generic function/method
    // instance's own param/return types. See instantiate_generic_function/
    // instantiate_generic_method (sema_check.cpp). 'enclosing_generic_params', when
    // non-null, is the DECLARATION's own generic_params list (not the instance's concrete
    // args) — enables implicit self-instantiation for a bare reference appearing in the
    // declaration's own signature/body (e.g. 'fn make_list[T: type](...) -> List' meaning
    // '-> List[T]').
    auto resolve_type_with_generic_env(const ast::Type &type, const std::string &module_path, Program &program,
                                        DiagnosticEngine &diag, const GenericBindingEnv &env,
                                        const std::vector<ast::GenericParam> *enclosing_generic_params = nullptr) -> ResolvedType;
    auto resolve_declared_type(const std::optional<ast::Type> &type, const std::optional<ast::Expr> &init,
                                const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                const SourceLocation &decl_loc) -> std::optional<ResolvedType>;
    auto resolve_import_bin_type(const std::string &module_path, const std::string &path, const SourceLocation &loc,
                                  Program &program, DiagnosticEngine &diag) -> ResolvedType;
    // The trait a generic parameter is bound by ('[T: Hashable]'), or -1 for an unconstrained
    // '[T: type]'. Defined in type_resolver.cpp.
    // Does '(type_module, type_name)' implement the trait at 'trait_index'? Returns the index
    // of the trait that PROVIDES it — the trait itself for a direct 'impl TRAIT for TYPE', or
    // a composing trait that reaches it transitively — or -1 for no. Note the type is named by
    // its UNSPECIALIZED (module, name): find_type_module_and_name maps 'List[i32]' back to
    // 'List', matching the coherence rule that one impl governs every instantiation.
    //
    // 'ambiguous_via', when non-null, collects the names of every composing trait that reaches
    // the wanted one; more than one entry means the caller should report an ambiguity. Shared
    // by the implicit value-to-trait-handle coercion and by generic bound checking, which must
    // agree on what "implements" means. Defined in type_resolver.cpp.
    auto find_trait_provider(const Program &program, const std::string &type_module, const std::string &type_name,
                              int trait_index, std::vector<std::string> *ambiguous_via = nullptr) -> int;

    // 'report' asks it to also diagnose a named bound that does not resolve to a trait; only
    // the eager pass sets it, since that pass visits each declaration exactly once.
    auto generic_param_bound_trait_index(const ast::GenericParam &param, const std::string &module_path,
                                          Program &program, DiagnosticEngine &diag, bool report = false) -> int;

    // Whether 'ty' is, or structurally contains, an unbound generic parameter (TypeKind::Opaque
    // — see resolved_type.hpp). Recursive because the parameter is usually wrapped: '*T', '[]T',
    // or a struct with a 'T' field. Defined in type_resolver.cpp.
    auto contains_opaque(const ResolvedType &ty, const Program &program) -> bool;

    // Whether an instantiation's argument list still mentions an unbound generic parameter —
    // i.e. this is a generic being instantiated from inside another generic's DECLARATION
    // ('fn f[T]() { g[T]() }'), not from real code.
    //
    // Such an instantiation is a fiction: there is no concrete 'g[T]' to monomorphize, and
    // registering one would put a type with no layout in front of codegen. Callers therefore
    // resolve its SIGNATURE (so the call's arity and arguments are still checked) but keep it
    // out of every registry, and never check its body — the callee's own eager pass does that
    // once, at its declaration. instantiate_generic_type has nothing worth building at all and
    // simply yields Opaque. Defined in type_resolver.cpp.
    auto args_contain_opaque(const std::vector<GenericArgValue> &args, const Program &program) -> bool;

    // Whether a struct/enum/union/bitset slot is a TEMPLATE placeholder — an instantiation
    // whose arguments are still unbound, built only so the eager pass can check a generic
    // type's impl methods against a receiver whose partly-concrete fields are visible
    // ('capacity: usize' stays known even when 'K' is not). Such a slot has no layout worth
    // emitting and must be skipped by every codegen declare loop. Defined in type_resolver.cpp.
    auto is_opaque_template_instance(const std::optional<GenericInstanceInfo> &generic_instance,
                                      const Program &program) -> bool;

    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool;
    // Like is_assignable, but also allows the additional expected-type-position-only
    // coercions available at an ordinary call/assignment site (array<->slice/pointer,
    // bitset->storage). Used for call-argument checking and default-parameter-value
    // type-checking so a defaulted argument coerces exactly like an explicit one would.
    auto assignable_in_module(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool;
    // Minimal human-readable rendering of a resolved type, used for trait conformance
    // and default-parameter-value diagnostics.
    auto describe_type(const ResolvedType &t, const Program &program) -> std::string;
    // Renders a method signature ("(self, T1) -> R") for trait-conformance and
    // trait-composition-collision diagnostics.
    auto describe_signature(bool is_mut_self, const std::vector<ResolvedType> &params,
                             const std::vector<ResolvedType> &returns, const Program &program) -> std::string;
    // True if two trait methods' signatures (self/mut-self, param types, return types —
    // defaults excluded) differ, i.e. they cannot be merged as the same flattened method
    // during trait-composition flattening (layout_trait, type_resolver.cpp).
    auto trait_methods_conflict(const TraitMethodInfo &a, const TraitMethodInfo &b) -> bool;
    // Byte size/alignment of an already-resolved type - exactly what 'size_of(T)'/
    // 'align_of(T)' evaluate to at compile time. codegen.cpp's Generator::size_of()/
    // align_of() and type_resolver.cpp's own Resolver::size_of()/align_of() each compute
    // this identically but privately (codegen.cpp isn't linked into mirage-lsp, and
    // Resolver is a file-local type_resolver.cpp class) - these free functions are the
    // one copy exposed for callers with only a Program& in hand, namely the LSP's hover
    // support for size_of()/align_of()/len().
    auto resolved_type_size(const ResolvedType &t, const Program &program) -> uint32_t;
    auto resolved_type_align(const ResolvedType &t, const Program &program) -> uint32_t;

    // Folds a 'size_of(X)'/'align_of(X)' operand (a type name or a value expression). Needs a
    // mutable Program because resolving the operand can force a type's layout.
    auto eval_size_of_operand(const ast::SizeOfExpr &expr, const std::string &module_path, Program &program,
                              DiagnosticEngine &diag) -> uint64_t;
    auto eval_align_of_operand(const ast::AlignOfExpr &expr, const std::string &module_path, Program &program,
                               DiagnosticEngine &diag) -> uint64_t;
    auto error_union_is_subset(const ResolvedType &callee, const ResolvedType &caller, const Program &program) -> bool;
    auto function_params_compatible(const std::vector<ResolvedType> &actual, const std::vector<ResolvedType> &expected) -> bool;
    auto intern_pointer(Program &program, const ResolvedType &pointee) -> ResolvedType;
    auto intern_slice(Program &program, const ResolvedType &element) -> ResolvedType;
    auto intern_function_type(Program &program, FunctionTypeInfo sig) -> ResolvedType;
    // The function-pointer type a named function DECAYS to when it appears in value position
    // ('const g := f', a ':='-inferred default parameter value, a call argument with no
    // declared parameter type to expect). Each interns a 'fn(params) -> returns' from an
    // already-resolved signature; none of them may be called on a variadic function (the
    // caller must reject that first — see spec.md's function-pointer rules) nor on a generic
    // TEMPLATE, whose 'params' are deliberately empty (see is_generic_function).
    // Whole-program pass checking every monomorphized generic struct's field default
    // initializers — see its definition in sema_check.cpp for why the declaration-walking
    // check_struct_field_defaults_for_module cannot cover them.
    void check_generic_struct_field_defaults_for_program(Program &program, DiagnosticEngine &diag);
    auto function_pointer_type_of(const FunctionSymbol &sym, Program &program) -> ResolvedType;
    auto ext_function_pointer_type_of(const ExtFunctionSymbol &sym, Program &program) -> ResolvedType;
    auto generic_instance_function_pointer_type(const GenericFunctionInstance &instance, Program &program) -> ResolvedType;
    // Assigns (or looks up) 'type''s stable identity for a given ResolvedType — the
    // compile-time value 'type_of' resolves to. Lazy, encounter-order assignment starting
    // at Program::next_type_id (16); ids 1-15 are pre-seeded by seed_builtin_type_ids for
    // the builtin scalar kinds. See Program::type_ids' doc comment for the sema-decides/
    // codegen-only-reads contract.
    auto intern_type_id(Program &program, const ResolvedType &t) -> uint64_t;
    // Pre-registers compiler-internal type ids 1-15 for the builtin scalar TypeKinds, called
    // once at the top of check_program before any 'type_of' expression can be visited.
    void seed_builtin_type_ids(Program &program);
    // See resolve_type's 'ast_program' doc above — same on-demand-declare purpose.
    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const ast::Program *ast_program = nullptr) -> ResolvedType;
    auto resolve_global_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    // Lazily/reentrantly resolves a free function's full signature (param types —
    // inferring ':=' param types from their default expression — return types, and
    // default-parameter validation: ordering, ext-fn exclusion doesn't apply here,
    // type-checking, 'try'-rejection, constant-flagging). Safe to call multiple
    // times; a no-op once FunctionSymbol::is_resolved is true. Required because a
    // default expression may call another not-yet-resolved function (mirrors
    // resolve_global_symbol's reentrant design for the same reason consts can
    // reference other consts).
    auto ensure_function_signature_resolved(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag) -> FunctionSymbol &;
    auto resolve_macro_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> MacroSymbol &;

    // Whether 'type' (a generic_param's declared type) is the builtin 'type' keyword itself
    // — i.e. this is a TYPE parameter ('T: type') rather than a value parameter. Defined in
    // sema_declare.cpp; shared by declare_type, type_resolver.cpp's generic_args resolution,
    // and sema_check.cpp's inference/instantiation logic.
    auto is_generic_type_param(const ast::Type &type) -> bool;
    // Whether 'type' is a legal declared type for a VALUE generic parameter in v1: bool, an
    // integer kind, or usize — see spec.md §22, "Declaring Generic Types" for the v1 scope
    // decision. Defined in sema_declare.cpp.
    auto is_legal_generic_value_param_type(const ast::Type &type) -> bool;
    // Reports a sema error for each of 'generic_params' whose declared type is neither
    // 'type' nor a legal builtin scalar. Shared by every decl kind that can carry
    // generic_params (type/fn/impl/trait-impl). Defined in sema_declare.cpp.
    void validate_generic_param_types(const std::vector<ast::GenericParam> &generic_params, DiagnosticEngine &diag);

    // A bare identifier or dotted chain ('T', 'mod.Foo') used as a generic_arg always parses
    // as an ast::Expr (IdentExpr/MemberExpr), never ast::Type - starts_type_only (ast.cpp) has
    // exactly one token of lookahead and a plain identifier is ambiguous between "value
    // expression" and "type name passed through unchanged" (e.g. forwarding an enclosing
    // generic function's own type param: 'fn wrap[T: type]() -> List[T]'). Reinterprets such
    // an Expr as the ast::Type it would have parsed to had the parser known better; returns
    // nullopt for any other expression shape (the caller then reports its own "must be a
    // type" diagnostic, unchanged). Shared by type_resolver.cpp's resolve_generic_named_type
    // and sema_check.cpp's resolve_explicit_generic_args. Defined in type_resolver.cpp.
    auto reinterpret_expr_as_type_name(const ast::Expr &expr) -> std::optional<ast::Type>;

    // Returns (or lazily creates, on first request) the concrete monomorphized ResolvedType
    // for 'decl_name[args]' — a generic 'type'/struct/enum/union/bitset declaration
    // instantiated with 'args', in declared parameter order. Linear-scans
    // Program::generic_type_instance_lookup for a cache hit first (intern_pointer's idiom,
    // keyed rather than plain-equality); on miss, allocates a fresh struct/enum/union/bitset
    // slot exactly like a non-generic declare_type, resolves the declaration's RHS with a
    // GenericBindingEnv built from (decl's generic_params, args) active, and records the new
    // slot in Program::generic_type_instance_lookup. Cycle-guarded by (decl_name, args) —
    // see type_resolver.cpp. Reports a sema error and returns TypeKind::Invalid if 'args'
    // doesn't match 'decl_name's declared arity/param kinds.
    auto instantiate_generic_type(Program &program, DiagnosticEngine &diag, const std::string &module_path,
                                   const std::string &decl_name, std::vector<GenericArgValue> args,
                                   const SourceLocation &use_loc) -> ResolvedType;
    // Same cache-or-create shape as instantiate_generic_type, for a generic free function.
    // Resolves the instance's signature eagerly (so its param/return types are immediately
    // usable for call-checking); returns its stable INDEX into
    // Program::generic_fn_instances — not a reference/pointer, since each instance is
    // individually heap-allocated (unique_ptr) specifically so an index (or a pointer
    // obtained via 'program.generic_fn_instances[idx].get()' immediately before use) stays
    // valid even if a later, nested/recursive instantiation grows the vector.
    auto instantiate_generic_function(Program &program, DiagnosticEngine &diag, const std::string &module_path,
                                       const std::string &decl_name, std::vector<GenericArgValue> args,
                                       const SourceLocation &use_loc) -> size_t;
    // Same as instantiate_generic_function, but for a method of a generic type's 'impl'
    // block — 'receiver_instantiation' is the already-resolved concrete receiver type (e.g.
    // 'List[i32]'), which supplies the args (read off its ResolvedType's *Info.generic_instance).
    // Returns nullopt if 'method_name' doesn't name a method on the receiver's unspecialized
    // generic type at all (an ordinary "no such method" sema error, reported by the caller).
    auto instantiate_generic_method(Program &program, DiagnosticEngine &diag, const ResolvedType &receiver_instantiation,
                                     const std::string &method_name, const SourceLocation &use_loc) -> std::optional<size_t>;
    // Builds the substitution env binding each of 'params' (in order) to its concrete
    // argument. Shared by instantiate_generic_function/instantiate_generic_method
    // (sema_check.cpp) and codegen.cpp's generic-instance body emission, which both need to
    // push the exact same env shape onto Program::active_generic_env_stack.
    auto build_generic_binding_env(const std::vector<ast::GenericParam> &params, const std::vector<GenericArgValue> &args) -> GenericBindingEnv;
    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, std::optional<ResolvedType> expected, int loop_depth, int defer_loop_base = -1, const ResolvedType *fn_error_type = nullptr) -> ResolvedType;
    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth, int defer_loop_base = -1) -> void;
    // 'extra_const_names' is treated exactly like a name already known to fold to a
    // compile-time constant (mirrors is_constant_expr_impl's internal 'treated_as_const' set,
    // used for macro-parameter substitution) — used to recognize a generic value-parameter
    // reference (e.g. 'N' inside 'fn make_fixed[N: usize]()') as constant while resolving
    // inside that declaration's own body/signature. Empty for every ordinary (non-generic)
    // caller.
    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program,
                           const std::unordered_set<std::string> &extra_const_names = {}) -> bool;

    // Returns the attribute named 'name' in 'attrs' (see ast::Attribute), or nullptr if none
    // matches. Shared by sema_attributes.cpp's own per-attribute validation, sema_declare.cpp
    // (rejecting '@init' on impl methods, where it's registered), and sema_check.cpp (the
    // '@always_inline' address-taken warning) — declared here rather than confined to
    // sema_attributes.cpp's anonymous namespace since all three need it.
    auto find_attribute(const std::vector<ast::Attribute> &attrs, std::string_view name) -> const ast::Attribute *;

    // Validates default parameter values, shared by free functions, inherent
    // methods, and trait method declarations (a template since those three each
    // have their own independent AST Param struct — see ast.hpp): forbids mixing a
    // native variadic parameter with any default in the same list, enforces the
    // "once a default appears, all later params need one too" ordering rule,
    // type-checks explicitly-typed ('= expr') defaults against their declared
    // type, and records each defaulted param's compile-time-constant-ness for
    // codegen. ':=' inferred-type defaults are already fully checked (and their
    // type already derived) by the caller before this runs — via check_expr with
    // expected=nullopt in an empty (module-scope) LocalScope — so they're only
    // visited here for the ordering/variadic/constant-flag bookkeeping.
    // Every default expression is checked with fn_error_type = nullptr (no caller
    // in scope, since defaults are validated once at declaration time, not per
    // call site) — this is what naturally makes 'try' inside a default expression
    // a sema error via the ordinary TryExpr check, with no special casing needed
    // here or anywhere else in this feature.
    //
    // 'outer_scope' is the scope explicitly-typed defaults are checked in. Ordinary
    // declarations pass nothing (module scope — a default may not reference another
    // parameter of the same function). A GENERIC declaration's instantiation passes a scope
    // carrying its VALUE generic-params as locals, since spec.md §22 makes the enclosing
    // declaration's own generic parameters the single exception to that rule; its TYPE
    // generic-params come from the ambient ScopedGenericScope instead.
    template <typename ParamT>
    void check_param_defaults(const std::vector<ParamT> &decl_params, const std::vector<ResolvedType> &resolved_params,
                               size_t &required_params, std::vector<bool> &param_default_is_const,
                               const std::string &module_path, Program &program, DiagnosticEngine &diag,
                               const LocalScope *outer_scope = nullptr) {
        required_params = decl_params.size();
        param_default_is_const.assign(decl_params.size(), false);
        if (decl_params.empty()) return;

        bool has_variadic = false;
        bool has_default = false;
        for (const auto &p : decl_params) {
            if constexpr (requires { p.is_variadic; }) {
                if (p.is_variadic) has_variadic = true;
            }
            if (p.default_value) has_default = true;
        }
        if (has_variadic && has_default) {
            diag.report_error(DiagnosticStage::Sema, decl_params.front().location,
                "a parameter list cannot mix a native variadic parameter with default parameter values");
            return;
        }

        bool seen_default = false;
        std::string last_defaulted_name;
        for (size_t i = 0; i < decl_params.size(); ++i) {
            const auto &p = decl_params[i];
            if (p.default_value) {
                if (!seen_default) {
                    required_params = i;
                    seen_default = true;
                }
                if (p.type) {
                    LocalScope scope = outer_scope ? *outer_scope : LocalScope{};
                    const auto default_ty = check_expr(*p.default_value, scope, module_path, program, diag, resolved_params[i], 0);
                    if (!assignable_in_module(default_ty, resolved_params[i], module_path, program)) {
                        diag.report_error(DiagnosticStage::Sema, p.location, std::format(
                            "default value for parameter '{}' has type '{}' but parameter is declared as '{}'",
                            p.name, describe_type(default_ty, program), describe_type(resolved_params[i], program)));
                    }
                }
                param_default_is_const[i] = is_constant_expr(*p.default_value, module_path, program);
                last_defaulted_name = p.name;
            } else if (seen_default) {
                diag.report_error(DiagnosticStage::Sema, p.location, std::format(
                    "parameter '{}' has no default value but follows parameter '{}' which does. "
                    "All parameters after the first defaulted parameter must also have defaults.",
                    p.name, last_defaulted_name));
            }
        }
    }

    // Evaluate a compile-time integer or bool constant expression. Returns nullopt if the expression
    // cannot be statically evaluated (e.g. non-constant or unsupported form). Used by match/switch
    // for duplicate arm detection (sema) and case-value emission (codegen).
    auto evaluate_integer_constant(const ast::Expr &expr, const std::string &module_path, const Program &program) -> std::optional<int64_t>;

    // A more general compile-time evaluator than evaluate_integer_constant above: also
    // folds string literals, cross-module qualified const access ('mod.NAME'), '$option'
    // (via the cached expr_option_values side table), 'when' expressions, and enum-literal
    // (.Variant) equality comparisons (resolved via the sibling operand's expr_types entry,
    // since a bare '.Variant' has no fixed value without that context). Used to fold 'when'
    // statement/declaration conditions and '#link' data expressions. Takes Program by
    // non-const reference (unlike evaluate_integer_constant) because it may need to force
    // resolution of a referenced global's value (resolve_global_symbol) on demand.
    auto evaluate_const_value(const ast::Expr &expr, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> std::optional<ConstFoldValue>;

    auto find_enum_field_by_name(const EnumInfo &info, std::string_view name) -> const EnumFieldInfo *;
    auto find_enum_field_by_value(const EnumInfo &info, int64_t value) -> const EnumFieldInfo *;

    // Full '$option' resolution: target-type priority (expected > default's type > []u8),
    // '--opt' string coercion or default-value folding, and the required/invalid-value
    // diagnostics. 'expr_key' is get_expr_key(...) of the ENCLOSING ast::Expr (for caching
    // into ProgramModule::expr_option_values) — see check_expr's OptionExpr case.
    auto resolve_option_expr(const void *expr_key, const ast::OptionExpr &opt, std::optional<ResolvedType> expected,
                              const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType;

    // Same as resolve_option_expr above, but the value source is an environment variable
    // (std::getenv(opt.key)) instead of '--opt'. Shares the same target-type resolution,
    // expr_option_values cache, and required/invalid-value diagnostic shape.
    auto resolve_env_expr(const void *expr_key, const ast::EnvExpr &opt, std::optional<ResolvedType> expected,
                           const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType;

    // Returns the module path and type name for a given resolved struct/enum type,
    // searching all modules. Returns {"", ""} if not found.
    auto find_type_module_and_name(const ResolvedType &ty, const Program &program) -> std::pair<std::string, std::string>;

    // Look up a method on a type. Searches the type's defining module's method table.
    auto find_method(const ResolvedType &ty, const std::string &method_name, const Program &program) -> const MethodInfo *;
}
