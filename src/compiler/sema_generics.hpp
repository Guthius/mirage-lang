#pragma once

// The generic-instantiation machinery's data structures, split out of sema.hpp:
// the substitution/argument value types, the monomorphization cache keys, the
// per-instance record for generic functions/methods, and the ambient scope
// stacks Program carries for "currently resolving inside instantiation X".
// Everything here is pure data + RAII — the instantiation logic itself lives in
// sema_check.cpp (functions) and type_resolver.cpp (types).

#include "ast.hpp"
#include "resolved_type.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sema {
    // A '--opt key=value' or environment-variable value coerced (per $option's/$env's
    // target-type coercion rules) or folded from a default expression: an integer/bool/
    // enum-underlying value, or a []u8 string. Used for '$option'/'$env' themselves and for
    // constant-folding 'when' conditions and '#link' data expressions that reference an
    // '$option'/'$env'-backed const. Defined here (rather than next to VariantCoercion/
    // AsmStmtInfo etc. in sema.hpp, its original home) because the generics structures
    // immediately following need it.
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
    // non-generic slot (std::optional<GenericInstanceInfo> on each *Info struct in
    // sema.hpp).
    struct GenericInstanceInfo {
        std::string decl_module;
        std::string decl_name;
        std::vector<GenericArgValue> args;
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

    // Hash over exactly the fields operator== compares, so GenericInstanceKey can key an
    // unordered_map. Deliberately NOT the instance's mangled name: mangling drops the
    // module path and sanitizes type spellings, so two distinct keys can mangle alike.
    struct GenericInstanceKeyHash {
        auto operator()(const GenericInstanceKey &k) const -> size_t {
            size_t h = std::hash<std::string>{}(k.module_path);
            const auto mix = [&h](const size_t v) { h ^= v + 0x9e3779b9U + (h << 6) + (h >> 2); };
            mix(std::hash<std::string>{}(k.decl_name));
            for (const auto &arg : k.args) {
                mix(std::hash<bool>{}(arg.is_type));
                mix(std::hash<ResolvedType>{}(arg.type_arg));
                mix(std::visit([]<typename V>(const V &v) { return std::hash<V>{}(v); }, arg.value_arg));
                mix(std::hash<ResolvedType>{}(arg.value_arg_scalar_type));
            }
            return h;
        }
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
        // coercion structs declared in sema.hpp. Reach them via
        // Program::exprs_for_fn_instance().
    };

    // Ambient scope stack whose ONLY mutation API is the RAII PushGuard. The two
    // Program members built on this (active_generic_env_stack, active_expr_tables)
    // hold pointers to STACK-LOCAL objects at their call sites, and
    // expr_tables_for_write's "writes never reach the module tables while a scope
    // is active" contract (see sema.hpp) depends on every push being popped on
    // every exit path — a missed pop dangles, an unmatched pop corrupts an outer
    // scope. Enforcing push/pop pairing by construction makes those invariants a
    // property of the API rather than of comment discipline at each site.
    template <typename T>
    class AmbientScopeStack {
      public:
        class PushGuard {
          public:
            PushGuard(AmbientScopeStack &stack, T *value) : stack_(stack.stack_) { stack_.push_back(value); }
            ~PushGuard() { stack_.pop_back(); }

            PushGuard(const PushGuard &) = delete;
            auto operator=(const PushGuard &) -> PushGuard & = delete;

          private:
            std::vector<T *> &stack_;
        };

        // The innermost active scope, or nullptr when none is active.
        [[nodiscard]] auto current() const -> T * { return stack_.empty() ? nullptr : stack_.back(); }
        [[nodiscard]] auto empty() const -> bool { return stack_.empty(); }

      private:
        std::vector<T *> stack_;
    };

    struct ExprSideTables; // defined in sema.hpp; the stack stores pointers only

    // The concrete stack types Program carries — named so call sites can spell a
    // guard without repeating the element type (e.g.
    // 'ActiveGenericEnvStack::PushGuard').
    using ActiveGenericEnvStack = AmbientScopeStack<const GenericBindingEnv>;
    using ActiveExprTableStack = AmbientScopeStack<ExprSideTables>;
}
