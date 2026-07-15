#include "sema.hpp"

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

    namespace {
        void declare_type(const ast::TypeDecl &decl, const std::string &module_path, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            std::optional<ResolvedType> resolved = std::nullopt;

            int struct_slot = -1;
            int enum_slot = -1;
            int union_slot = -1;
            int trait_slot = -1;
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
            }

            if (!declare_symbol(module.symbols, decl.name, TypeSymbol{.decl = &decl, .resolved = resolved, .is_pub = decl.is_pub, .location = decl.location}, decl.location, diag)) {
                return;
            }

            if (struct_slot >= 0) {
                sema_program.structs.push_back(StructInfo{.module_path = module_path, .is_packed = std::get<std::unique_ptr<ast::StructType>>(decl.type)->is_packed});
            }
            if (enum_slot >= 0) {
                sema_program.enums.push_back(EnumInfo{});
            }
            if (union_slot >= 0) {
                const auto &union_decl = std::get<std::unique_ptr<ast::UnionType>>(decl.type);
                sema_program.unions.push_back(UnionInfo{.module_path = module_path, .is_tagged = union_decl->is_tagged});
            }
            if (trait_slot >= 0) {
                sema_program.traits.push_back(TraitInfo{.module_path = module_path});
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
    }

    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diag) {
        for (auto &decl : decls) {
            std::visit(
                [&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::FunctionDecl>) {
                        declare_symbol(module.symbols, v.name, FunctionSymbol{.decl = &v, .is_pub = v.is_pub}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::ExtFunctionDecl>) {
                        declare_symbol(module.symbols, v.name, ExtFunctionSymbol{.decl = &v, .is_pub = v.is_pub}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::VarDecl>) {
                        declare_global(v, program, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::MacroDecl>) {
                        declare_symbol(module.symbols, v.name, MacroSymbol{.decl = &v, .is_pub = v.is_pub, .is_resolved = false}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::TypeDecl>) {
                        declare_type(v, module_path, module, sema_program, diag);
                    } else if constexpr (std::is_same_v<V, ast::ImplDecl>) {
                        // Pre-register each method as unresolved in the module's method table.
                        // The target type name is the leaf of the named type chain.
                        const std::string &type_name = v.target.name;
                        for (auto &fn : v.functions) {
                            module.methods[type_name][fn.name] = MethodInfo{
                                .decl = &fn,
                                .impl_module = module_path,
                                .type_name = type_name,
                                .is_mut_self = fn.is_mut_self,
                                .is_pub = fn.is_pub,
                                .is_resolved = false,
                            };
                        }
                    }
                },
                decl);
        }
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
                if (!type_ref->symbol->resolved || type_ref->symbol->resolved->kind == TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, timpl->type_name.location, std::format("'{}' is not a struct, enum, or union type", type_ref->name));
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
