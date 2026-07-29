#include "sema.hpp"

#include <algorithm>
#include <format>

namespace sema {
    auto resolve_import(const ast::Program &program, const std::string &importer, const std::string &imported) -> std::string {
        if (const auto importer_it = program.module_imports.find(importer); importer_it != program.module_imports.end()) {
            if (const auto module_it = importer_it->second.find(imported); module_it != importer_it->second.end()) {
                return module_it->second;
            }
        }
        return {};
    }

    auto declare_symbol(SymbolTable &symbol_table, std::string name, Symbol symbol, const SourceLocation &loc, DiagnosticEngine &diag) -> bool {
        if (symbol_table.contains(name)) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("redefinition of '{}'", name));
            return false;
        }
        symbol_table[name] = std::move(symbol);
        return true;
    }

    // Ensures 'module_path' has been declared (build_symbol_table_for_module has run for
    // it) — memoized + cycle-guarded. A module-scope 'when' condition referencing another
    // module's const (e.g. 'opts.target_os') needs that module's symbol table (and, via
    // resolve_global_symbol, that specific const's VALUE) to exist before the condition
    // can be folded, regardless of Program::modules' unordered iteration order. Declared
    // here (external linkage, before the anonymous namespace) so both this file's
    // anonymous-namespace helpers and sema.cpp's check_program can call it.
    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag);

    // Whether 'type' (a generic_param's declared type) is the builtin 'type' keyword itself
    // — i.e. this is a TYPE parameter ('T: type') rather than a value parameter.
    auto is_generic_type_param(const ast::Type &type) -> bool {
        const auto *builtin = std::get_if<ast::BuiltinType>(&type);
        return builtin != nullptr && builtin->kind == ast::BuiltinTypeKind::Type;
    }

    // Whether 'type' is a legal declared type for a VALUE generic parameter in v1: bool, an
    // integer kind, or usize. Deliberately narrower than '$option'/'$env's full coercible-type
    // set (which also allows []u8 and enum) — see spec.md §22, "Declaring Generic Types" for
    // the v1 scope decision (keeps the monomorphization cache key / name-mangling / RTTI
    // value encoding pure-integer comparison).
    auto is_legal_generic_value_param_type(const ast::Type &type) -> bool {
        const auto *builtin = std::get_if<ast::BuiltinType>(&type);
        if (!builtin) return false;
        switch (builtin->kind) {
        case ast::BuiltinTypeKind::Bool:
        case ast::BuiltinTypeKind::U8:
        case ast::BuiltinTypeKind::U16:
        case ast::BuiltinTypeKind::U32:
        case ast::BuiltinTypeKind::U64:
        case ast::BuiltinTypeKind::I8:
        case ast::BuiltinTypeKind::I16:
        case ast::BuiltinTypeKind::I32:
        case ast::BuiltinTypeKind::I64:
        case ast::BuiltinTypeKind::Usize:
            return true;
        default:
            return false;
        }
    }

    // Reports a sema error for each of 'generic_params' whose declared type is neither
    // 'type' nor a legal builtin scalar — shared by declare_type/parse_function_decl's
    // caller (declare_one_decl for fn) and the impl-decl paths below, since all four decl
    // kinds that can carry generic_params (type/fn/impl/trait-impl) validate identically.
    void validate_generic_param_types(const std::vector<ast::GenericParam> &generic_params, DiagnosticEngine &diag) {
        for (const auto &param : generic_params) {
            if (!is_generic_type_param(param.type) && !is_legal_generic_value_param_type(param.type)) {
                diag.report_error(DiagnosticStage::Sema, param.location, std::format(
                    "generic parameter '{}' declared type must be 'type' or a builtin scalar type "
                    "(bool, an integer kind, or usize)", param.name));
            }
        }
    }

    namespace {
        void declare_type(const ast::TypeDecl &decl, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            if (!decl.generic_params.empty()) {
                validate_generic_param_types(decl.generic_params, diag);
                // No slot is allocated yet — there is no "the" type for a generic declaration
                // until a concrete instantiation is actually requested (see
                // instantiate_generic_type, type_resolver.cpp). TypeSymbol::resolved stays
                // nullopt; a bare reference to this name with no generic_args is a separate
                // sema error, reported where the NamedType is resolved.
                declare_symbol(module.symbols, decl.name,
                    TypeSymbol{.decl = &decl, .resolved = std::nullopt, .is_pub = decl.is_pub, .location = decl.location},
                    decl.location, diag);
                return;
            }

            std::optional<ResolvedType> resolved = std::nullopt;

            int struct_slot = -1;
            int enum_slot = -1;
            int union_slot = -1;
            int trait_slot = -1;
            int bitset_slot = -1;
            if (std::holds_alternative<std::unique_ptr<ast::StructType>>(decl.type)) {
                struct_slot = static_cast<int>(sema_program.structs.size());
                resolved = ResolvedType{
                    .kind = TypeKind::Struct,
                    .struct_index = struct_slot,
                };
            } else if (std::holds_alternative<std::unique_ptr<ast::EnumType>>(decl.type)) {
                enum_slot = static_cast<int>(sema_program.enums.size());
                resolved = ResolvedType{
                    .kind = TypeKind::Enum,
                    .enum_index = enum_slot,
                };
            } else if (std::holds_alternative<std::unique_ptr<ast::UnionType>>(decl.type)) {
                union_slot = static_cast<int>(sema_program.unions.size());
                resolved = ResolvedType{
                    .kind = TypeKind::Union,
                    .union_index = union_slot,
                };
            } else if (std::holds_alternative<std::unique_ptr<ast::TraitType>>(decl.type)) {
                trait_slot = static_cast<int>(sema_program.traits.size());
                resolved = ResolvedType{
                    .kind = TypeKind::Trait,
                    .trait_index = trait_slot,
                };
            } else if (std::holds_alternative<std::unique_ptr<ast::BitsetType>>(decl.type)) {
                bitset_slot = static_cast<int>(sema_program.bitsets.size());
                resolved = ResolvedType{
                    .kind = TypeKind::Bitset,
                    .bitset_index = bitset_slot,
                };
            }

            if (!declare_symbol(module.symbols, decl.name, TypeSymbol{.decl = &decl, .resolved = resolved, .is_pub = decl.is_pub, .location = decl.location}, decl.location, diag)) {
                return;
            }

            if (struct_slot >= 0) {
                sema_program.structs.push_back(StructInfo{.module_path = module_path, .is_packed = std::get<std::unique_ptr<ast::StructType>>(decl.type)->is_packed});
            }
            if (enum_slot >= 0) {
                sema_program.enums.push_back(EnumInfo{.module_path = module_path});
            }
            if (union_slot >= 0) {
                const auto &union_decl = std::get<std::unique_ptr<ast::UnionType>>(decl.type);
                sema_program.unions.push_back(UnionInfo{.module_path = module_path, .is_tagged = union_decl->is_tagged});
            }
            if (trait_slot >= 0) {
                // 'name' is set here (declare time), not by layout_trait, so it's already
                // available via program.trait_at(idx)->name for an ancestor trait that's still
                // mid-layout (on ResolveState::trait_composition_stack) when a composition-cycle
                // error needs to name it — layout_trait never touches this field.
                sema_program.traits.push_back(TraitInfo{.module_path = module_path, .name = decl.name});
            }
            if (bitset_slot >= 0) {
                sema_program.bitsets.push_back(BitsetInfo{.module_path = module_path});
            }
        }

        void declare_global(const ast::VarDecl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            if (decl.init && std::holds_alternative<ast::ImportExpr>(*decl.init)) {
                const auto &import_expr = std::get<ast::ImportExpr>(*decl.init);

                if (const auto resolved_path = resolve_import(program, module_path, import_expr.module_name); resolved_path.empty()) {
                    // No diagnostic here: module_resolver.cpp already reported this
                    // failure with a real location when it walked the import graph.
                    // Still register the symbol, pointing at a sentinel, empty
                    // module, so downstream `name.X` references get a clean,
                    // per-use "unknown type/member 'X'" diagnostic instead of
                    // cascading "unknown identifier" everywhere the import is used.
                    const std::string sentinel_path = "<unresolved:" + import_expr.module_name + ">";
                    declare_symbol(
                        module.symbols, decl.name,
                        ImportSymbol{
                            .expr = &import_expr,
                            .module_path = sentinel_path,
                            .is_pub = decl.is_pub,
                        },
                        decl.location, diag);
                    sema_program.modules[sentinel_path]; // default-constructs an empty ProgramModule if absent
                } else {
                    declare_symbol(
                        module.symbols, decl.name,
                        ImportSymbol{
                            .expr = &import_expr,
                            .module_path = resolved_path,
                            .is_pub = decl.is_pub,
                        },
                        decl.location, diag);
                }
                return;
            }

            // Not a bare `const NAME := import(...)` module alias, but the initializer may
            // still be a chained '.field' access on an inline import (e.g.
            // `const target_arch := import("...").target_arch`) - resolve and cache the
            // import's module path now (while the ast::Program::module_imports map built by
            // module_resolver.cpp is available) so the check phase's
            // try_resolve_namespace_chain can look it up by node address later. `target_arch`
            // itself is a plain value here, so it still falls through to the ordinary
            // GlobalSymbol registration below.
            if (decl.init) {
                if (const auto *leaf_import = ast::find_leaf_import(*decl.init)) {
                    const auto resolved_path = resolve_import(program, module_path, leaf_import->module_name);
                    if (resolved_path.empty()) {
                        // Same sentinel-module rationale as the direct-import case above.
                        const std::string sentinel_path = "<unresolved:" + leaf_import->module_name + ">";
                        module.inline_import_paths[leaf_import] = sentinel_path;
                        sema_program.modules[sentinel_path];
                    } else {
                        module.inline_import_paths[leaf_import] = resolved_path;
                    }
                }
            }

            declare_symbol(
                module.symbols, decl.name,
                GlobalSymbol{
                    .decl = &decl,
                    .type = ResolvedType{.kind = TypeKind::Invalid},
                    .is_mut = decl.is_mut,
                    .is_pub = decl.is_pub,
                    .is_resolved = false,
                },
                decl.location, diag);
        }

        // Builds the bare-import-vs-bare-import collision diagnostic: names both source
        // modules (as the user actually wrote them, not their resolved absolute paths) and
        // the conflicting symbol, and suggests disambiguating with bound imports using each
        // module's leaf path segment as the suggested local alias (column-padded so the two
        // 'const NAME := import(...)' lines line up).
        void report_bare_import_collision(DiagnosticEngine &diag, const SourceLocation &loc, const std::string &name,
                                           const std::string &new_source, const std::string &prior_source) {
            const auto leaf = [](const std::string &path) {
                const auto slash = path.find_last_of('/');
                return slash == std::string::npos ? path : path.substr(slash + 1);
            };
            const auto prior_short = leaf(prior_source);
            const auto new_short = leaf(new_source);
            const auto width = std::max(prior_short.size(), new_short.size());
            const auto pad = [&](const std::string &s) { return s + std::string(width - s.size(), ' '); };

            diag.report_error(DiagnosticStage::Sema, loc, std::format(
                "bare import of '{}' introduces symbol '{}' which\n"
                "       conflicts with '{}' already imported from '{}'.\n"
                "       Use bound imports to disambiguate:\n"
                "         const {} := import(\"{}\")\n"
                "         const {} := import(\"{}\")\n"
                "         {}.{}(...)  {}.{}(...)",
                new_source, name, name, prior_source,
                pad(prior_short), prior_source, pad(new_short), new_source,
                prior_short, name, new_short, name));
        }

        // 'import("path")' as a standalone module-scope declaration (BareImportDecl):
        // registers a PRIVATE local alias — sharing the target's 'decl' AST pointer (and,
        // for a TypeSymbol, its global struct/enum/union/bitset/trait index) rather than a
        // duplicate — for every symbol declared 'pub' in the target module, named
        // identically to its unqualified name there. 'impl' blocks are never aliased
        // (method resolution on a bare-imported type already works through the type's own
        // defining module — see find_method/find_type_module_and_name in sema.cpp).
        // ImportSymbol entries (a target module's OWN 'pub const mod := import(...)'
        // namespace bindings) are also excluded — a namespace binding isn't one of the
        // value/fn/type/macro/ext-fn kinds this feature imports.
        void declare_bare_import(const ast::BareImportDecl &decl, const ast::Program &program, const std::string &module_path,
                                  ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            const auto target_path = resolve_import(program, module_path, decl.path);
            if (target_path.empty()) {
                return; // module_resolver.cpp already reported the unresolved-path error
            }

            // Force the target's own symbol table (and transitively anything IT depends on
            // — its own bare imports / module-scope 'when' conditions) to exist before
            // reading its 'pub' symbols, regardless of Program::modules' unordered
            // iteration order. Also detects/reports mutual-dependency cycles via the
            // shared cycle guard (see ensure_module_declared's generalized message below).
            ensure_module_declared(program, target_path, sema_program, diag);

            const auto target_it = sema_program.modules.find(target_path);
            if (target_it == sema_program.modules.end()) return; // unresolved-sentinel module etc.

            for (const auto &[name, target_sym] : target_it->second.symbols) {
                const bool is_pub = std::visit([](const auto &s) { return s.is_pub; }, target_sym);
                if (!is_pub) continue;
                if (std::holds_alternative<ImportSymbol>(target_sym)) continue;

                if (const auto prior = module.bare_import_origins.find(name); prior != module.bare_import_origins.end()) {
                    report_bare_import_collision(diag, decl.location, name, decl.path, prior->second.source_path);
                    continue;
                }

                if (!declare_symbol(module.symbols, name, target_sym, decl.location, diag)) {
                    continue; // generic "redefinition of 'name'" — collides with a local decl
                }
                std::visit([](auto &s) { s.is_pub = false; }, module.symbols.at(name));
                module.bare_import_origins.emplace(name, BareImportOrigin{.module_path = target_path, .symbol_name = name, .source_path = decl.path});
            }
        }

        // Location of any Decl variant alternative — mirrors sema.hpp's get_expr_location
        // for ast::Expr, needed here since only VarDecl/TypeDecl/etc's *own* diagnostics
        // carry a location normally; the module-scope-'when' allow-list check below needs
        // one for an arbitrary (possibly disallowed) decl kind.
        auto decl_location(const ast::Decl &decl) -> SourceLocation {
            return std::visit([](const auto &v) -> SourceLocation {
                using V = std::decay_t<decltype(v)>;
                if constexpr (requires { v->location; }) return v->location;
                else return v.location;
            }, decl);
        }

        // Only '#link', 'const' with a direct '$option(...)'/'$env(...)' initializer,
        // 'type', and 'ext fn' declarations are permitted inside a module-scope 'when'
        // block (spec). A nested 'when' is always structurally allowed here — its OWN
        // contents are checked recursively against this same allow-list wherever they're
        // processed.
        auto decl_allowed_in_module_scope_when(const ast::Decl &decl) -> bool {
            if (std::holds_alternative<ast::LinkDecl>(decl)) return true;
            if (std::holds_alternative<ast::DiagnosticDecl>(decl)) return true;
            if (std::holds_alternative<ast::TypeDecl>(decl)) return true;
            if (std::holds_alternative<ast::ExtFunctionDecl>(decl)) return true;
            if (std::holds_alternative<std::unique_ptr<ast::WhenDecl>>(decl)) return true;
            if (const auto *vd = std::get_if<ast::VarDecl>(&decl)) {
                return !vd->is_mut && vd->init &&
                       (std::holds_alternative<std::unique_ptr<ast::OptionExpr>>(*vd->init) ||
                        std::holds_alternative<std::unique_ptr<ast::EnvExpr>>(*vd->init));
            }
            return false;
        }

        void declare_one_decl(const ast::Decl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag);
        void declare_when_decl(const ast::WhenDecl &when_decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag);
        void declare_link_decl(const ast::LinkDecl &link_decl, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, bool collect);
        void declare_diagnostic_decl(const ast::DiagnosticDecl &decl, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, bool live);
        void check_decl_unreachable(const ast::Decl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag);
        void check_when_decl_unreachable(const ast::WhenDecl &when_decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag);

        // Declares (is_live=true, persists into the real symbol table / collects '#link'
        // directives) or merely type-checks-and-discards (is_live=false — the module-scope
        // 'when' spec requirement that BOTH branches are always type-checked, even though
        // only the live one has any lasting effect) a list of decls found inside a
        // module-scope 'when' block, after validating each against the allow-list.
        void declare_decl_list(const std::vector<ast::Decl> &decls, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag, const bool is_live) {
            for (const auto &decl : decls) {
                if (!decl_allowed_in_module_scope_when(decl)) {
                    diag.report_error(DiagnosticStage::Sema, decl_location(decl),
                        "only '#link', '#error', '#warn', 'const' with '$option'/'$env', 'type', "
                        "and 'ext fn' declarations are permitted inside a module-scope 'when' block.");
                    continue;
                }
                if (is_live) {
                    declare_one_decl(decl, program, module_path, module, sema_program, diag);
                } else {
                    check_decl_unreachable(decl, program, module_path, module, sema_program, diag);
                }
            }
        }

        // Type-checks (but never declares/collects) a single decl reached inside a dead
        // module-scope 'when' branch — 'both branches are always type-checked' without
        // giving the branch any real symbol-table presence.
        void check_decl_unreachable(const ast::Decl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            std::visit(
                [&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;
                    LocalScope empty;

                    if constexpr (std::is_same_v<V, ast::ExtFunctionDecl>) {
                        for (auto &p : v.params) resolve_type(p.type, module_path, sema_program, diag, &program);
                        if (v.return_type) resolve_type(*v.return_type, module_path, sema_program, diag, &program);
                    } else if constexpr (std::is_same_v<V, ast::VarDecl>) {
                        std::optional<ResolvedType> declared_ty;
                        if (v.type) declared_ty = resolve_type(*v.type, module_path, sema_program, diag, &program);
                        if (v.init) {
                            check_expr(*v.init, empty, module_path, sema_program, diag, declared_ty, 0, -1, nullptr);
                        }
                    } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                        declare_link_decl(v, module_path, sema_program, diag, /*collect=*/false);
                    } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                        declare_diagnostic_decl(v, module_path, sema_program, diag, /*live=*/false);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenDecl>>) {
                        check_when_decl_unreachable(*v, program, module_path, module, sema_program, diag);
                    }
                    // ast::TypeDecl: type layout resolution is lazy/on-demand throughout this
                    // compiler (only forced by an actual reference elsewhere) — an unreferenced
                    // dead-branch 'type' decl is left exactly as lazy as an unreferenced live one.
                },
                decl);
        }

        // Recursively type-checks (never declares/collects) an entire 'when' block already
        // known to be unreachable (an ancestor 'when' condition selected the OTHER branch) —
        // both of ITS branches are scratch-checked unconditionally, since neither will ever
        // be declared for real regardless of how this nested condition itself would fold.
        void check_when_decl_unreachable(const ast::WhenDecl &when_decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            LocalScope empty;
            check_expr(when_decl.condition, empty, module_path, sema_program, diag, ResolvedType{.kind = TypeKind::Bool}, 0, -1, nullptr);
            if (!is_constant_expr(when_decl.condition, module_path, sema_program)) {
                diag.report_error(DiagnosticStage::Sema, when_decl.location,
                    "'when' condition must be a compile-time constant expression. "
                    "Use 'if' for runtime conditions.");
            }

            declare_decl_list(when_decl.then_decls, program, module_path, module, sema_program, diag, /*is_live=*/false);
            if (when_decl.else_branch) {
                std::visit(
                    [&]<typename EV>(const EV &else_v) {
                        using EVT = std::decay_t<EV>;
                        if constexpr (std::is_same_v<EVT, std::vector<ast::Decl>>) {
                            declare_decl_list(else_v, program, module_path, module, sema_program, diag, /*is_live=*/false);
                        } else {
                            check_when_decl_unreachable(*else_v, program, module_path, module, sema_program, diag);
                        }
                    },
                    *when_decl.else_branch);
            }
        }

        // Walks a 'when' condition expression to find cross-module member accesses (e.g.
        // 'opts.target_os') and ensures the referenced module is declared before the
        // condition is type-checked/folded — see ensure_module_declared above.
        void ensure_condition_modules_declared(const ast::Expr &expr, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            std::visit(
                [&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        ensure_condition_modules_declared(v->object, program, module_path, module, sema_program, diag);
                        if (const auto *ident = std::get_if<ast::IdentExpr>(&v->object)) {
                            if (const auto sym_it = module.symbols.find(ident->name); sym_it != module.symbols.end()) {
                                if (const auto *imp = std::get_if<ImportSymbol>(&sym_it->second)) {
                                    ensure_module_declared(program, imp->module_path, sema_program, diag);
                                }
                            }
                        } else if (const auto *inline_import = std::get_if<ast::ImportExpr>(&v->object)) {
                            // 'import("...").field' used directly as the object (no bound name)
                            // — same shape resolve_member_object_import_path (value_resolver.cpp)
                            // and try_resolve_namespace_chain (sema_check.cpp) already resolve via
                            // inline_import_paths; declare_global caches this entry before a
                            // module-scope 'when' is ever reached, since consts are always
                            // declared before it in source order.
                            if (const auto path_it = module.inline_import_paths.find(inline_import); path_it != module.inline_import_paths.end()) {
                                ensure_module_declared(program, path_it->second, sema_program, diag);
                            }
                        }
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                        ensure_condition_modules_declared(v->lhs, program, module_path, module, sema_program, diag);
                        ensure_condition_modules_declared(v->rhs, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        ensure_condition_modules_declared(v->operand, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                        ensure_condition_modules_declared(v->value, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                        ensure_condition_modules_declared(v->condition, program, module_path, module, sema_program, diag);
                        ensure_condition_modules_declared(v->then_expr, program, module_path, module, sema_program, diag);
                        ensure_condition_modules_declared(v->else_expr, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        // A bare identifier naming a SAME-module const whose own initializer
                        // itself crosses into another module — e.g. 'target_os' where
                        // 'const target_os := import("build/options").target_os' — needs that
                        // module force-declared too, exactly like a direct 'mod.field' reference
                        // above, since resolving 'target_os' itself will reentrantly need it.
                        // Recurses into the referenced const's own init expression to catch this
                        // transitively (a chain of several same-module consts deep). Previously
                        // a no-op here ("same-module IdentExpr: nothing to do"), which happened
                        // to be masked by Program::modules' unordered iteration order usually
                        // reaching the referenced module some other way first — not guaranteed.
                        if (const auto sym_it = module.symbols.find(v.name); sym_it != module.symbols.end()) {
                            if (const auto *g = std::get_if<GlobalSymbol>(&sym_it->second); g && g->decl->init) {
                                ensure_condition_modules_declared(*g->decl->init, program, module_path, module, sema_program, diag);
                            }
                        }
                    }
                    // literals, DotIdentExpr, etc: nothing to do.
                },
                expr);
        }

        // Type-checks '#link's 'data' argument as a compile-time-constant '[]u8' expression
        // and, if 'collect' is true (a LIVE position), appends the resolved directive to
        // Program::link_directives. A dead-branch '#link' is still fully type-checked here
        // (per spec) — 'collect=false' just skips the final push_back.
        void declare_link_decl(const ast::LinkDecl &link_decl, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, const bool collect) {
            LocalScope empty;
            const auto u8_slice = intern_slice(sema_program, ResolvedType{.kind = TypeKind::U8});
            const auto data_ty = check_expr(link_decl.data, empty, module_path, sema_program, diag, u8_slice, 0);
            if (!is_assignable(data_ty, u8_slice)) {
                diag.report_error(DiagnosticStage::Sema, link_decl.location,
                    "'#link' data argument must be a compile-time constant '[]u8' expression");
                return;
            }
            if (!is_constant_expr(link_decl.data, module_path, sema_program)) {
                diag.report_error(DiagnosticStage::Sema, link_decl.location,
                    "'#link' data argument must be a compile-time constant expression");
                return;
            }
            if (!collect) return;

            const auto folded = evaluate_const_value(link_decl.data, module_path, sema_program, diag);
            const auto *str = folded ? std::get_if<std::string>(&*folded) : nullptr;
            if (!str) {
                diag.report_error(DiagnosticStage::Sema, link_decl.location,
                    "internal error: could not resolve '#link' data to a constant string");
                return;
            }

            const auto category = link_decl.category == ast::LinkCategory::Lib ? LinkCategory::Lib
                                 : link_decl.category == ast::LinkCategory::System ? LinkCategory::System
                                 : LinkCategory::Flag;

            sema_program.link_directives.push_back(LinkDirective{
                .category = category,
                .data = *str,
                .source_module = module_path,
                .location = link_decl.location,
            });
        }

        // Type-checks '#error'/'#warn's 'message' argument as a compile-time-constant '[]u8'
        // expression and, if 'live' is true, emits the corresponding sema diagnostic. A
        // dead-branch '#error'/'#warn' (an unselected 'when' arm) is still fully type-checked
        // here (per the module-scope 'when' rule every other directive follows) — 'live=false'
        // just skips the actual diag.report_error/diag.warn call, mirroring declare_link_decl's
        // 'collect' parameter above.
        void declare_diagnostic_decl(const ast::DiagnosticDecl &decl, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, const bool live) {
            const auto directive = decl.kind == ast::DiagnosticDirectiveKind::Error ? "#error" : "#warn";

            LocalScope empty;
            const auto u8_slice = intern_slice(sema_program, ResolvedType{.kind = TypeKind::U8});
            const auto message_ty = check_expr(decl.message, empty, module_path, sema_program, diag, u8_slice, 0);
            if (!is_assignable(message_ty, u8_slice)) {
                diag.report_error(DiagnosticStage::Sema, decl.location,
                    std::format("'{}' message argument must be a compile-time constant '[]u8' expression", directive));
                return;
            }
            if (!is_constant_expr(decl.message, module_path, sema_program)) {
                diag.report_error(DiagnosticStage::Sema, decl.location,
                    std::format("'{}' message argument must be a compile-time constant expression", directive));
                return;
            }
            if (!live) return;

            const auto folded = evaluate_const_value(decl.message, module_path, sema_program, diag);
            const auto *str = folded ? std::get_if<std::string>(&*folded) : nullptr;
            if (!str) {
                diag.report_error(DiagnosticStage::Sema, decl.location,
                    std::format("internal error: could not resolve '{}' message to a constant string", directive));
                return;
            }

            if (decl.kind == ast::DiagnosticDirectiveKind::Error) {
                diag.report_error(DiagnosticStage::Sema, decl.location, *str);
            } else {
                diag.warn(DiagnosticStage::Sema, decl.location, *str);
            }
        }

        // Folds a module-scope 'when' declaration's condition and either declares its live
        // branch for real (persisting symbols / collecting '#link') or scratch-checks it
        // (dead branch — still fully type-checked, per spec, but never declared/collected).
        void declare_when_decl(const ast::WhenDecl &when_decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            ensure_condition_modules_declared(when_decl.condition, program, module_path, module, sema_program, diag);

            LocalScope empty;
            check_expr(when_decl.condition, empty, module_path, sema_program, diag, ResolvedType{.kind = TypeKind::Bool}, 0, -1, nullptr);

            bool selected = false;
            if (!is_constant_expr(when_decl.condition, module_path, sema_program)) {
                diag.report_error(DiagnosticStage::Sema, when_decl.location,
                    "'when' condition must be a compile-time constant expression. "
                    "Use 'if' for runtime conditions.");
            } else if (const auto folded = evaluate_const_value(when_decl.condition, module_path, sema_program, diag)) {
                if (const auto *iv = std::get_if<int64_t>(&*folded)) {
                    selected = (*iv != 0);
                }
            }

            module.when_decl_selected[&when_decl] = selected;

            declare_decl_list(when_decl.then_decls, program, module_path, module, sema_program, diag, /*is_live=*/selected);

            if (when_decl.else_branch) {
                std::visit(
                    [&]<typename EV>(const EV &else_v) {
                        using EVT = std::decay_t<EV>;
                        if constexpr (std::is_same_v<EVT, std::vector<ast::Decl>>) {
                            declare_decl_list(else_v, program, module_path, module, sema_program, diag, /*is_live=*/!selected);
                        } else if (!selected) {
                            declare_when_decl(*else_v, program, module_path, module, sema_program, diag);
                        } else {
                            // Outer condition selected 'then_decls' already, so this entire
                            // 'else when' chain is dead; still type-checked, never declared.
                            check_when_decl_unreachable(*else_v, program, module_path, module, sema_program, diag);
                        }
                    },
                    *when_decl.else_branch);
            }
        }

        void declare_one_decl(const ast::Decl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            std::visit(
                [&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::FunctionDecl>) {
                        // The missing fourth caller. validate_generic_param_types documents
                        // itself as shared by all four decl kinds that carry generic_params,
                        // but the bare generic free function never called it -- so an illegal
                        // parameter type was accepted at the declaration and only surfaced at
                        // the call site as a confusing "generic argument 1 for 'f' must be a
                        // compile-time constant expression of type 'Thing'".
                        validate_generic_param_types(v.generic_params, diag);
                        declare_symbol(module.symbols, v.name, FunctionSymbol{.decl = &v, .is_pub = v.is_pub}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::ExtFunctionDecl>) {
                        declare_symbol(module.symbols, v.name, ExtFunctionSymbol{.decl = &v, .is_pub = v.is_pub}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::VarDecl>) {
                        declare_global(v, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::MacroDecl>) {
                        declare_symbol(module.symbols, v.name, MacroSymbol{.decl = &v, .is_pub = v.is_pub, .is_resolved = false}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::TypeDecl>) {
                        declare_type(v, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::BareImportDecl>) {
                        declare_bare_import(v, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::ImplDecl>) {
                        // Pre-register each method as unresolved in the module's method table.
                        // The target type name is the leaf of the named type chain.
                        const std::string &type_name = v.target.name;

                        validate_generic_param_types(v.generic_params, diag);
                        // Arity check: an 'impl' block's own generic_params must match the
                        // target type's own declared arity exactly (matched by count, not by
                        // parameter kind or name — see spec.md §22, "Generic Impl Blocks").
                        // Best-effort: only checked when 'type_name' is already declared in
                        // THIS module by this point (the common case) — a forward reference to
                        // a same-module type declared later in the file, or a cross-module
                        // target, isn't caught here (declare_one_decl runs in source order with
                        // no lookahead); instantiate_generic_type/instantiate_generic_method
                        // still reject a genuinely wrong arity later, just with a less precise
                        // diagnostic location.
                        if (const auto sym_it = module.symbols.find(type_name); sym_it != module.symbols.end()) {
                            if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                                const size_t target_arity = ts->decl ? ts->decl->generic_params.size() : 0;
                                if (v.generic_params.size() != target_arity) {
                                    diag.report_error(DiagnosticStage::Sema, v.location, std::format(
                                        "'impl {}' has {} generic parameter(s), but '{}' is declared with {} — impl "
                                        "generic parameter lists must match the target type's own arity exactly",
                                        type_name, v.generic_params.size(), type_name, target_arity));
                                }
                            }
                        }

                        for (auto &fn : v.functions) {
                            if (find_attribute(fn.attributes, "init")) {
                                diag.report_error(DiagnosticStage::Sema, fn.location,
                                    "'@init' is not allowed on impl methods; declare a module-scope function instead");
                            }
                            // Reject a redefinition instead of overwriting it, mirroring
                            // register_trait_impls_for_program's "duplicate method" check and
                            // declare_symbol's "redefinition of 'x'" for every other symbol
                            // kind. This map is keyed by (type, method name) across the whole
                            // module, so it catches all three collision shapes: twice in one
                            // impl block, across two 'impl TYPE {}' blocks, and across the
                            // separate .mir files merged into one module directory.
                            //
                            // The first definition wins, matching redefinition handling
                            // elsewhere. Silently keeping the last meant the earlier body was
                            // never type-checked or emitted, and calls resolved to whichever
                            // definition happened to be declared last in file-merge order.
                            auto &type_methods = module.methods[type_name];
                            if (type_methods.contains(fn.name)) {
                                diag.report_error(DiagnosticStage::Sema, fn.location,
                                    std::format("duplicate method '{}' for type '{}'", fn.name, type_name));
                                continue;
                            }
                            type_methods[fn.name] = MethodInfo{
                                .decl = &fn,
                                .impl_module = module_path,
                                .type_name = type_name,
                                .impl_generic_params = v.generic_params.empty() ? nullptr : &v.generic_params,
                                .is_mut_self = fn.is_mut_self,
                                .is_pub = fn.is_pub,
                                .is_resolved = false,
                            };
                        }
                    } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                        declare_link_decl(v, module_path, sema_program, diag, /*collect=*/true);
                    } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                        declare_diagnostic_decl(v, module_path, sema_program, diag, /*live=*/true);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenDecl>>) {
                        declare_when_decl(*v, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) {
                        // The parser accepts 'asm {...}' here purely so this diagnostic can name
                        // the exact construct, mirroring LinkDecl/DiagnosticDecl's mirror-image
                        // rejection as a Stmt inside a function body (sema_check.cpp).
                        diag.report_error(DiagnosticStage::Sema, v->location,
                            "asm blocks are only legal inside function bodies");
                    }
                    // ast::TraitImplDecl: handled separately by register_trait_impls_for_program.
                },
                decl);
        }
    }

    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diag) {
        for (auto &decl : decls) {
            declare_one_decl(decl, program, module_path, module, sema_program, diag);
        }
    }

    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag) {
        if (sema_program.modules_declared.contains(module_path)) return;
        if (sema_program.resolve_state.when_module_declaring.contains(module_path)) {
            diag.report_error(DiagnosticStage::Sema, {},
                std::format("circular dependency between modules' declarations involving '{}' "
                            "(caused by a module-scope 'when' condition or a bare import forced "
                            "to declare a module that, transitively, depends back on this one)", module_path));
            return;
        }
        const auto ast_mod_it = program.modules.find(module_path);
        if (ast_mod_it == program.modules.end()) return; // unresolved import sentinel, etc.

        sema_program.resolve_state.when_module_declaring.insert(module_path);
        auto &module = sema_program.modules[module_path];
        build_symbol_table_for_module(program, module_path, module, sema_program, ast_mod_it->second, diag);

        // Also force full type-symbol LAYOUT (not just declaration) for this module right
        // away — mirrors resolve_signatures_for_module's type-forcing loop (normally run
        // program-wide in check_program's step 3, well after every module's step 1). A
        // module reached early via this reentrant path (a 'when' condition referencing one
        // of its consts, e.g. 'opts.target_os') needs its enum/struct/union types FULLY
        // laid out immediately: resolve_type_impl's NamedType case only ever calls
        // resolve_final_SHALLOW (returns the pre-allocated handle without forcing layout),
        // so without this, a cross-module 'when' condition would see a referenced enum's
        // 'fields' still empty (layout_done=false) when '$option' tries to coerce a
        // '--opt' string against it. Harmless to repeat later — layout_done guards make
        // the eventual real step 3 pass a no-op for a module already resolved here.
        for (auto &[name, sym] : module.symbols) {
            if (const auto *ts = std::get_if<TypeSymbol>(&sym)) {
                // A generic type declaration has no ResolvedType of its own to force layout
                // for — 'List' alone is never a valid type, and there is no "the" instantiation
                // to eagerly resolve (see declare_type/instantiate_generic_type).
                if (ts->decl && !ts->decl->generic_params.empty()) continue;
                resolve_type_symbol(module_path, name, sema_program, diag, ts->decl->location, &program);
            }
        }

        sema_program.resolve_state.when_module_declaring.erase(module_path);
        sema_program.modules_declared.insert(module_path);
    }

    namespace {
        // Resolves a (possibly dotted) NamedType reference used in trait-impl position to
        // the (module, local_name) pair it names, walking import hops the same way
        // type_resolver.cpp's walk_namespace_chain does for full type resolution. Only
        // symbol-table presence is needed here (declare_type has already run for every
        // module by the time register_trait_impls_for_program runs), not full layout
        // resolution — this intentionally duplicates the chain-walk rather than reaching
        // into type_resolver.cpp's anonymous namespace, which isn't externally callable.
        auto walk_named_type_chain(const std::string &start_module, const ast::NamedType &named,
                                    Program &sema_program, DiagnosticEngine &diag) -> std::optional<std::pair<std::string, std::string>> {
            std::string current_module = start_module;
            const ast::NamedType *current = &named;

            while (current->member != nullptr) {
                const auto mod_it = sema_program.modules.find(current_module);
                if (mod_it == sema_program.modules.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("internal error: module '{}' not found", current_module));
                    return std::nullopt;
                }
                const auto sym_it = mod_it->second.symbols.find(current->name);
                if (sym_it == mod_it->second.symbols.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("unknown identifier '{}'", current->name));
                    return std::nullopt;
                }
                const auto *imp = std::get_if<ImportSymbol>(&sym_it->second);
                if (!imp) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("'{}' is not a namespace", current->name));
                    return std::nullopt;
                }
                current_module = imp->module_path;
                current = current->member.get();
            }

            return std::make_pair(current_module, current->name);
        }

        struct ResolvedTypeRef {
            std::string module_path;
            std::string name;
            const TypeSymbol *symbol = nullptr;
        };

        auto resolve_type_ref(const std::string &start_module, const ast::NamedType &named,
                               Program &sema_program, DiagnosticEngine &diag) -> std::optional<ResolvedTypeRef> {
            const auto chain = walk_named_type_chain(start_module, named, sema_program, diag);
            if (!chain) return std::nullopt;
            const auto &[mod_path, name] = *chain;

            const auto mod_it = sema_program.modules.find(mod_path);
            if (mod_it == sema_program.modules.end()) {
                diag.report_error(DiagnosticStage::Sema, named.location, std::format("unknown identifier '{}'", name));
                return std::nullopt;
            }
            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) {
                diag.report_error(DiagnosticStage::Sema, named.location, std::format("unknown identifier '{}'", name));
                return std::nullopt;
            }
            const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
            if (!ts) {
                diag.report_error(DiagnosticStage::Sema, named.location, std::format("'{}' is not a type", name));
                return std::nullopt;
            }
            return ResolvedTypeRef{.module_path = mod_path, .name = name, .symbol = ts};
        }
    }

    void register_trait_impls_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag) {
        for (auto &[module_path, decls] : ast_program.modules) {
            for (auto &decl : decls) {
                const auto *timpl = std::get_if<ast::TraitImplDecl>(&decl);
                if (!timpl) continue;

                const auto trait_ref = resolve_type_ref(module_path, timpl->trait_name, sema_program, diag);
                const auto type_ref = resolve_type_ref(module_path, timpl->type_name, sema_program, diag);
                if (!trait_ref || !type_ref) continue;

                if (!trait_ref->symbol->resolved || trait_ref->symbol->resolved->kind != TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, timpl->trait_name.location, std::format("'{}' is not a trait", trait_ref->name));
                    continue;
                }

                // A generic type's TypeSymbol has no 'resolved' ResolvedType of its own (see
                // declare_type) — 'List' alone is never itself a valid type, but it IS a
                // legal 'impl TRAIT for List[T: type] { ... }' target, parametrizing the
                // TYPE side only (traits themselves are not made generic).
                const bool type_is_generic_decl = type_ref->symbol->decl && !type_ref->symbol->decl->generic_params.empty();
                if (!type_is_generic_decl && (!type_ref->symbol->resolved || type_ref->symbol->resolved->kind == TypeKind::Trait)) {
                    diag.report_error(DiagnosticStage::Sema, timpl->type_name.location, std::format("'{}' is not a struct, enum, or union type", type_ref->name));
                    continue;
                }

                validate_generic_param_types(timpl->generic_params, diag);
                const size_t target_arity = type_is_generic_decl ? type_ref->symbol->decl->generic_params.size() : 0;
                if (timpl->generic_params.size() != target_arity) {
                    diag.report_error(DiagnosticStage::Sema, timpl->location, std::format(
                        "'impl {} for {}' has {} generic parameter(s), but '{}' is declared with {} — impl "
                        "generic parameter lists must match the target type's own arity exactly",
                        trait_ref->name, type_ref->name, timpl->generic_params.size(), type_ref->name, target_arity));
                    continue;
                }

                const int trait_index = trait_ref->symbol->resolved->trait_index;

                // Coherence: the impl must live in the trait's module or the type's module.
                if (module_path != trait_ref->module_path && module_path != type_ref->module_path) {
                    diag.report_error(DiagnosticStage::Sema, timpl->location,
                        std::format("orphan impl: 'impl {} for {}' must be declared in the module that defines '{}' or the module that defines '{}'",
                            trait_ref->name, type_ref->name, trait_ref->name, type_ref->name));
                    continue;
                }

                // Duplicate-impl check.
                auto dedup_key = std::make_tuple(trait_ref->module_path, trait_ref->name, type_ref->module_path, type_ref->name);
                if (sema_program.trait_impl_registry.contains(dedup_key)) {
                    diag.report_error(DiagnosticStage::Sema, timpl->location,
                        std::format("duplicate impl of trait '{}' for type '{}'", trait_ref->name, type_ref->name));
                    continue;
                }
                sema_program.trait_impl_registry[dedup_key] = timpl->location;

                TraitImplInfo impl_info{
                    .trait_module = trait_ref->module_path,
                    .trait_name = trait_ref->name,
                    .trait_index = trait_index,
                    .type_module = type_ref->module_path,
                    .type_name = type_ref->name,
                    .impl_module = module_path,
                    .location = timpl->location,
                };

                bool ok = true;
                for (auto &fn : timpl->functions) {
                    // (a) collision within this trait impl block
                    if (impl_info.methods.contains(fn.name)) {
                        diag.report_error(DiagnosticStage::Sema, fn.location,
                            std::format("duplicate method '{}' in 'impl {} for {}'", fn.name, trait_ref->name, type_ref->name));
                        ok = false;
                        continue;
                    }

                    // (b) collision against the type's bare impl methods (live in the type's own module)
                    bool collided = false;
                    if (const auto type_mod_it = sema_program.modules.find(type_ref->module_path); type_mod_it != sema_program.modules.end()) {
                        if (const auto bare_it = type_mod_it->second.methods.find(type_ref->name); bare_it != type_mod_it->second.methods.end()) {
                            if (bare_it->second.contains(fn.name)) {
                                diag.report_error(DiagnosticStage::Sema, fn.location,
                                    std::format("method '{}' conflicts with an existing method in 'impl {} {{ }}'", fn.name, type_ref->name));
                                collided = true;
                            }
                        }
                    }

                    // (c) collision against other trait impls already registered for this type
                    if (!collided) {
                        if (const auto existing_it = sema_program.trait_impls_by_type.find({type_ref->module_path, type_ref->name});
                            existing_it != sema_program.trait_impls_by_type.end()) {
                            for (auto &other_impl : existing_it->second) {
                                if (other_impl.methods.contains(fn.name)) {
                                    diag.report_error(DiagnosticStage::Sema, fn.location,
                                        std::format("method '{}' conflicts with 'impl {} for {}'", fn.name, other_impl.trait_name, type_ref->name));
                                    collided = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (collided) {
                        ok = false;
                        continue;
                    }

                    if (find_attribute(fn.attributes, "init")) {
                        diag.report_error(DiagnosticStage::Sema, fn.location,
                            "'@init' is not allowed on impl methods; declare a module-scope function instead");
                    }

                    impl_info.methods[fn.name] = MethodInfo{
                        .decl = &fn,
                        .impl_module = module_path,
                        .type_name = type_ref->name,
                        .is_mut_self = fn.is_mut_self,
                        .is_pub = fn.is_pub,
                        .is_resolved = false,
                        .trait_name = trait_ref->name,
                        .trait_module = trait_ref->module_path,
                    };
                }

                if (!ok) continue; // a colliding impl is dropped rather than partially registered

                sema_program.trait_impls_by_type[{type_ref->module_path, type_ref->name}].push_back(std::move(impl_info));
            }
        }
    }
}
