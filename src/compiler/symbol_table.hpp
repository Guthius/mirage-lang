#pragma once

#include <optional>
#include <unordered_map>

#include "ast.hpp"
#include "resolved_type.hpp"

namespace sema {
    // Which ABI a function's signature is lowered with, and therefore how aggregates cross
    // its boundary. 'Mirage' passes structs/arrays/unions raw -- the compiler owns both
    // sides of such a call, so they are self-consistent regardless of any platform rule.
    // 'C' routes the signature (and every call site) through the platform C ABI instead:
    // System V eightbyte classification natively, clang's WebAssembly rules on wasm. Set by
    // '@callconv("c")' / '@cdecl'; see ext_function_type in codegen.cpp, which is the same
    // machinery 'ext fn' already goes through.
    enum class CallConv : uint8_t { Mirage, C };

    // '@export' / '@export("name")': the linker-visible name a declaration is emitted under,
    // replacing the module-path mangling every other symbol gets. Absent means "mangle
    // normally". An empty string is never stored -- a bare '@export' records the
    // declaration's own name.
    using ExportName = std::optional<std::string>;

    struct GlobalSymbol {
        const ast::VarDecl *decl = nullptr;
        ResolvedType type;
        bool is_mut = false;
        bool is_pub = false;
        bool is_resolved = false;
    };

    struct FunctionSymbol {
        const ast::FunctionDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        std::vector<ResolvedType> return_types;
        bool is_pub = false;
        bool is_variadic = false;             // true if the last param is native '...T'
        ResolvedType variadic_element_type{};  // T; only meaningful if is_variadic
        bool is_resolved = false;              // lazily/reentrantly resolved — see ensure_function_signature_resolved
        size_t required_params = 0;            // count of leading non-defaulted params; == params.size() if none are defaulted
        std::vector<bool> param_default_is_const; // parallel to params; meaningful only at i >= required_params

        // Declaration attributes that outlive the AST walk. Filled by
        // validate_function_attributes (sema_attributes.cpp) once per declaration, so that
        // check sites (call expressions, codegen) never re-scan the attribute list.
        bool no_discard = false;               // '@no_discard': the result may not be dropped
        ExportName export_name;                // '@export' / '@export("name")'
        CallConv call_conv = CallConv::Mirage; // '@callconv("c")' / '@cdecl'
        // '@test'. Read at two places: check_bodies_for_module, which skips a test's body
        // outside 'mirage test', and the call-site diagnostic in check_expr.
        bool is_test = false;
    };

    // True for a generic function TEMPLATE ('fn f[T: type](v: T)'). Such a symbol's
    // 'params'/'return_types' are deliberately never filled in: a template has no single
    // signature, only one per concrete instantiation (see GenericFunctionInstance). Anything
    // that reads those vectors — or calls ensure_function_signature_resolved — must check
    // this first and route to instantiate_generic_function instead.
    inline auto is_generic_function(const FunctionSymbol &sym) -> bool {
        return sym.decl != nullptr && !sym.decl->generic_params.empty();
    }

    struct ExtFunctionSymbol {
        const ast::ExtFunctionDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        std::optional<ResolvedType> return_type;
        bool is_pub = false;
        bool is_variadic = false;
        // Guards resolve_signatures_for_module's per-param/return-type resolution loop
        // (sema.cpp) against being run twice on the same symbol — needed once a bare
        // import can cause the SAME underlying decl to be visited via two different
        // symbol-table entries (the origin's own, and an alias's), unlike ordinary
        // (non-aliased) 'ext fn's, which this loop previously only ever visited once.
        bool is_resolved = false;

        // '@import("module")' / '@import("module", "name")' -- which wasm import this
        // declaration binds to. Empty means the defaults: module "env", name == the
        // declaration's own name. Inert on non-wasm targets, where an 'ext fn' is resolved
        // by the linker from its bare name and there is no import-module concept.
        std::string import_module;
        std::string import_name;
    };

    struct MacroSymbol {
        const ast::MacroDecl *decl = nullptr;
        std::vector<ResolvedType> params;
        ResolvedType result_type;
        bool has_declared_result_type = false;
        bool is_pub = false;
        bool is_resolved = false;
    };

    struct ImportSymbol {
        const ast::ImportExpr *expr = nullptr;
        std::string module_path;
        bool is_pub = false;
    };

    struct TypeSymbol {
        const ast::TypeDecl *decl = nullptr;
        std::optional<ResolvedType> resolved;
        bool is_pub = false;
        SourceLocation location;
        // True only for the temporary entries shadow_generic_type_params installs to make a
        // generic parameter name ('T') resolve to its bound type while one instantiation is
        // checked. Such an entry is an ALIAS, not a declaration, so anything that maps a
        // ResolvedType back to its declared NAME must skip it — otherwise
        // find_type_module_and_name can answer "T" instead of "Point" (the scan is over an
        // unordered_map, so it would do so only sometimes), and every lookup keyed on the type
        // name — trait impls, methods — then misses.
        bool is_generic_param_shadow = false;
    };

    using Symbol = std::variant<
        GlobalSymbol,
        FunctionSymbol,
        ExtFunctionSymbol,
        MacroSymbol,
        ImportSymbol,
        TypeSymbol>;

    using SymbolTable = std::unordered_map<std::string, Symbol>;
}
