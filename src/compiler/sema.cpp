#include "sema.hpp"

#include <algorithm>
#include <format>
#include <ranges>

namespace sema {
    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diagnostics);
    void register_trait_impls_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag);
    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag);
    void validate_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag);
    void validate_method_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag);
    void validate_trait_impl_attributes_for_program(Program &program, DiagnosticEngine &diag);
    void validate_init_dependencies_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag);

    // Minimal human-readable rendering of a resolved type, used for trait conformance
    // and default-parameter-value diagnostics (there is no general ResolvedType-to-string
    // formatter elsewhere in sema).
    auto describe_type(const ResolvedType &t, const Program &program) -> std::string {
        switch (t.kind) {
        case TypeKind::Invalid:  return "<invalid>";
        case TypeKind::Void:     return "void";
        case TypeKind::U8:       return "u8";
        case TypeKind::U16:      return "u16";
        case TypeKind::U32:      return "u32";
        case TypeKind::U64:      return "u64";
        case TypeKind::I8:       return "i8";
        case TypeKind::I16:      return "i16";
        case TypeKind::I32:      return "i32";
        case TypeKind::I64:      return "i64";
        case TypeKind::F32:      return "f32";
        case TypeKind::F64:      return "f64";
        case TypeKind::USize:    return "usize";
        case TypeKind::Bool:     return "bool";
        case TypeKind::Anyptr:   return "anyptr";
        case TypeKind::Pointer: {
            const auto *pointee = program.pointee_at(t.pointee_index);
            return "*" + (pointee ? describe_type(*pointee, program) : std::string("?"));
        }
        case TypeKind::Slice: {
            const auto *info = program.slice_at(t.slice_index);
            return "[]" + (info ? describe_type(info->element_type, program) : std::string("?"));
        }
        case TypeKind::Array: {
            const auto *info = program.array_at(t.array_index);
            return info ? std::format("[{}]{}", info->count, describe_type(info->element_type, program)) : "[]?";
        }
        case TypeKind::Union: {
            if (const auto *info = program.union_at(t.union_index); info && info->is_error_union) {
                std::string out = "error(";
                for (size_t i = 0; i < info->error_member_types.size(); ++i) {
                    if (i > 0) out += " | ";
                    out += describe_type(info->error_member_types[i], program);
                }
                return out + ")";
            }
            [[fallthrough]];
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Trait: {
            const auto [mod, name] = find_type_module_and_name(t, program);
            return name.empty() ? "<unknown type>" : name;
        }
        case TypeKind::Function: return "fn(...)";
        default: return "<type>";
        }
    }

    namespace {
        // Resolves 'ef's declared param/return types exactly once (idempotent via
        // 'is_resolved'). Extracted out of resolve_signatures_for_module's inline body so
        // a bare-import alias can force the TRUE ORIGIN'S ExtFunctionSymbol to resolve
        // using the origin's own module_path as context (see resolve_signatures_for_module
        // below), instead of resolving relative to whichever module's table happens to be
        // iterating it — a param/return type naming another type local to the origin
        // module must resolve there, not in the importer.
        void resolve_ext_function_symbol(const std::string &module_path, ExtFunctionSymbol &ef, Program &program, DiagnosticEngine &diag) {
            if (ef.is_resolved) return;

            for (auto &p : ef.decl->params) {
                const auto pt = resolve_type(p.type, module_path, program, diag);
                if (p.default_value) {
                    diag.report_error(DiagnosticStage::Sema, p.location,
                        "'ext fn' declarations may not have default parameter values. "
                        "Default arguments have no C ABI representation.");
                }
                if (pt.kind == TypeKind::Union) {
                    diag.report_error(DiagnosticStage::Sema, p.location, "union types are not yet supported in extern function signatures");
                }
                if (pt.kind == TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, p.location, "trait handles have no C ABI representation and cannot appear in 'ext fn' signatures");
                }
                if (pt.kind == TypeKind::Function) {
                    const auto *sig = program.fn_signature_at(pt.fn_index);
                    if (sig && sig->return_types.size() > 1) {
                        diag.report_error(DiagnosticStage::Sema, p.location, "multi-return function types cannot be used in extern function signatures (no C ABI representation)");
                    }
                }
                ef.params.push_back(pt);
            }

            if (ef.decl->return_type) {
                const auto rt = resolve_type(*ef.decl->return_type, module_path, program, diag);
                if (rt.kind == TypeKind::Union) {
                    diag.report_error(DiagnosticStage::Sema, ef.decl->location, "union types are not yet supported in extern function signatures");
                }
                if (rt.kind == TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, ef.decl->location, "trait handles have no C ABI representation and cannot appear in 'ext fn' signatures");
                }
                if (rt.kind == TypeKind::Function) {
                    const auto *sig = program.fn_signature_at(rt.fn_index);
                    if (sig && sig->return_types.size() > 1) {
                        diag.report_error(DiagnosticStage::Sema, ef.decl->location, "multi-return function types cannot be used in extern function signatures (no C ABI representation)");
                    }
                }
                ef.return_type = rt;
            }

            ef.is_variadic = ef.decl->is_variadic;
            ef.is_resolved = true;
        }

        void resolve_signatures_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                if (!std::holds_alternative<TypeSymbol>(sym)) continue;
                const auto loc = std::get<TypeSymbol>(sym).decl->location;
                // A bare-import alias's '.resolved' already shares the origin's GLOBAL
                // struct/enum/union/bitset/trait index (copied verbatim at declare time),
                // so no copy-back is needed here — forcing layout via the origin's own
                // (module_path, name) just ensures that shared global-index entry gets laid
                // out using the origin as the resolution context for its OWN field types,
                // which becomes automatically visible through the alias too.
                if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                    resolve_type_symbol(origin->second.module_path, origin->second.symbol_name, program, diag, loc);
                } else {
                    resolve_type_symbol(module_path, name, program, diag, loc);
                }
            }

            for (auto &[name, sym] : module.symbols) {
                if (auto *fn = std::get_if<FunctionSymbol>(&sym)) {
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        auto &origin_sym = ensure_function_signature_resolved(origin->second.module_path, origin->second.symbol_name, program, diag);
                        *fn = origin_sym;
                        fn->is_pub = false; // never re-export through the alias
                    } else {
                        ensure_function_signature_resolved(module_path, name, program, diag);
                    }
                } else if (auto *ef = std::get_if<ExtFunctionSymbol>(&sym)) {
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        auto &origin_sym = std::get<ExtFunctionSymbol>(program.modules.at(origin->second.module_path).symbols.at(origin->second.symbol_name));
                        resolve_ext_function_symbol(origin->second.module_path, origin_sym, program, diag);
                        *ef = origin_sym;
                        ef->is_pub = false;
                    } else {
                        resolve_ext_function_symbol(module_path, *ef, program, diag);
                    }
                }
            }
        }

        void resolve_values_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                if (auto *g = std::get_if<GlobalSymbol>(&sym)) {
                    const auto loc = g->decl->location;
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        resolve_global_symbol(origin->second.module_path, origin->second.symbol_name, program, diag, loc);
                        *g = std::get<GlobalSymbol>(program.modules.at(origin->second.module_path).symbols.at(origin->second.symbol_name));
                        g->is_pub = false;
                    } else {
                        resolve_global_symbol(module_path, name, program, diag, loc);
                    }
                } else if (auto *m = std::get_if<MacroSymbol>(&sym)) {
                    const auto loc = m->decl->location;
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        auto &origin_sym = resolve_macro_symbol(origin->second.module_path, origin->second.symbol_name, program, diag, loc);
                        *m = origin_sym;
                        m->is_pub = false;
                    } else {
                        resolve_macro_symbol(module_path, name, program, diag, loc);
                    }
                }
            }
        }

        void resolve_impl_signatures_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[type_name, method_map] : module.methods) {
                // Resolve the self type for this type name
                const auto self_type = resolve_type_symbol(module_path, type_name, program, diag, {});

                for (auto &info : method_map | std::views::values) {
                    if (info.is_resolved) continue;

                    info.self_type = self_type;

                    for (auto &p : info.decl->params) {
                        ResolvedType pt;
                        if (p.type) {
                            pt = resolve_type(*p.type, module_path, program, diag);
                        } else {
                            // ':=' inferred-type param — infer from the (required) default expr.
                            LocalScope empty;
                            pt = check_expr(*p.default_value, empty, module_path, program, diag, std::nullopt, 0);
                        }
                        if (p.is_variadic) {
                            info.is_variadic = true;
                            info.variadic_element_type = pt;
                            info.param_types.push_back(intern_slice(program, pt));
                        } else {
                            info.param_types.push_back(pt);
                        }
                    }
                    for (auto &rt : info.decl->return_types) {
                        info.return_types.push_back(resolve_type(rt, module_path, program, diag));
                    }

                    check_param_defaults(info.decl->params, info.param_types, info.required_params,
                                          info.param_default_is_const, module_path, program, diag);

                    info.is_resolved = true;
                }
            }
        }

        auto describe_signature(const bool is_mut_self, const std::vector<ResolvedType> &params,
                                 const std::vector<ResolvedType> &returns, const Program &program) -> std::string {
            std::string s = is_mut_self ? "(mut self" : "(self";
            for (const auto &p : params) {
                s += ", " + describe_type(p, program);
            }
            s += ")";
            if (!returns.empty()) {
                s += " -> ";
                if (returns.size() == 1) {
                    s += describe_type(returns[0], program);
                } else {
                    s += "(";
                    for (size_t i = 0; i < returns.size(); ++i) {
                        if (i) s += ", ";
                        s += describe_type(returns[i], program);
                    }
                    s += ")";
                }
            }
            return s;
        }

        // Resolves every trait-impl method's signature and checks it conforms to the
        // trait it implements: same name, same self/mut self, exactly matching resolved
        // param and return types. Conformance is checked against THIS impl block's own
        // methods only (not the type's bare 'impl' block) — trait impls are meant to be
        // an exact mirror of the trait's method set; anything extra belongs in the bare
        // impl instead (see the "extra method" diagnostic below).
        void resolve_trait_impl_signatures_for_program(Program &program, DiagnosticEngine &diag) {
            for (auto &impls : program.trait_impls_by_type | std::views::values) {
                for (auto &impl_info : impls) {
                    const auto self_type = resolve_type_symbol(impl_info.type_module, impl_info.type_name, program, diag, impl_info.location);

                    for (auto &info : impl_info.methods | std::views::values) {
                        if (info.is_resolved) continue;

                        info.self_type = self_type;

                        for (auto &p : info.decl->params) {
                            ResolvedType pt;
                            if (p.type) {
                                pt = resolve_type(*p.type, impl_info.impl_module, program, diag);
                            } else {
                                LocalScope empty;
                                pt = check_expr(*p.default_value, empty, impl_info.impl_module, program, diag, std::nullopt, 0);
                            }
                            if (p.is_variadic) {
                                info.is_variadic = true;
                                info.variadic_element_type = pt;
                                info.param_types.push_back(intern_slice(program, pt));
                            } else {
                                info.param_types.push_back(pt);
                            }
                        }
                        for (auto &rt : info.decl->return_types) {
                            info.return_types.push_back(resolve_type(rt, impl_info.impl_module, program, diag));
                        }

                        // A trait-impl method must never declare its own defaults — see the
                        // redeclare/add-without-trait validation below, matched against the
                        // trait's own defaults. required_params is always the full count here;
                        // all defaulting for a trait-backed method flows from TraitMethodInfo.
                        info.required_params = info.param_types.size();
                        info.param_default_is_const.assign(info.param_types.size(), false);

                        info.is_resolved = true;
                    }

                    const auto *trait_info = program.trait_at(impl_info.trait_index);
                    if (!trait_info) continue;

                    for (size_t trait_method_index = 0; trait_method_index < trait_info->methods.size(); ++trait_method_index) {
                        const auto &trait_method = trait_info->methods[trait_method_index];
                        const auto it = impl_info.methods.find(trait_method.name);
                        if (it == impl_info.methods.end()) {
                            diag.report_error(DiagnosticStage::Sema, impl_info.location,
                                std::format("missing implementation of trait method '{}' in 'impl {} for {}'",
                                    trait_method.name, impl_info.trait_name, impl_info.type_name));
                            continue;
                        }

                        auto &impl_method = it->second;
                        impl_method.trait_index = impl_info.trait_index;
                        impl_method.trait_method_index = static_cast<int>(trait_method_index);

                        const bool mismatch = impl_method.is_mut_self != trait_method.is_mut_self ||
                                               impl_method.is_variadic ||
                                               impl_method.param_types != trait_method.params ||
                                               impl_method.return_types != trait_method.return_types;
                        if (mismatch) {
                            diag.report_error(DiagnosticStage::Sema, impl_method.decl->location,
                                std::format(
                                    "method '{}' does not match trait '{}': expected {}, found {}",
                                    trait_method.name, impl_info.trait_name,
                                    describe_signature(trait_method.is_mut_self, trait_method.params, trait_method.return_types, program),
                                    describe_signature(impl_method.is_mut_self, impl_method.param_types, impl_method.return_types, program)));
                        }

                        for (size_t i = 0; i < impl_method.decl->params.size() && i < trait_method.params.size(); ++i) {
                            if (!impl_method.decl->params[i].default_value) continue;

                            if (i >= trait_method.required_params) {
                                diag.report_error(DiagnosticStage::Sema, impl_method.decl->params[i].location, std::format(
                                    "parameter '{}' in '{}.{}' redeclares a default value already declared by the trait "
                                    "'{}'. Remove the default from the implementation — it is inherited from the trait.",
                                    impl_method.decl->params[i].name, impl_info.type_name, trait_method.name, impl_info.trait_name));
                            } else {
                                diag.report_error(DiagnosticStage::Sema, impl_method.decl->params[i].location, std::format(
                                    "parameter '{}' in '{}.{}' declares a default value but the corresponding trait method "
                                    "'{}.{}' does not. Defaults on trait implementations must match the trait declaration.",
                                    impl_method.decl->params[i].name, impl_info.type_name, trait_method.name,
                                    impl_info.trait_name, trait_method.name));
                            }
                        }
                    }

                    for (const auto &[method_name, impl_method] : impl_info.methods) {
                        const bool declared_by_trait = std::ranges::any_of(trait_info->methods,
                            [&](const TraitMethodInfo &m) { return m.name == method_name; });
                        if (!declared_by_trait) {
                            diag.report_error(DiagnosticStage::Sema, impl_method.decl->location,
                                std::format("extra method '{}' not declared by trait '{}'; move it to 'impl {} {{ }}' instead",
                                    method_name, impl_info.trait_name, impl_info.type_name));
                        }
                    }
                }
            }
        }

        void check_struct_field_defaults_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (const auto &[name, sym] : module.symbols) {
                // A bare-import alias shares its struct's global index with the origin —
                // the origin module's own pass over this loop already checks these field
                // defaults once, correctly, under the origin's context.
                if (module.bare_import_origins.contains(name)) continue;
                const auto *ts = std::get_if<TypeSymbol>(&sym);
                if (!ts || !ts->resolved || ts->resolved->kind != TypeKind::Struct) continue;

                const auto *struct_decl = std::get_if<std::unique_ptr<ast::StructType>>(&ts->decl->type);
                if (!struct_decl) continue;

                const auto *struct_info_ptr = program.struct_at(ts->resolved->struct_index);
                if (!struct_info_ptr) continue;
                const auto &struct_info = *struct_info_ptr;
                LocalScope empty;

                for (size_t i = 0; i < (*struct_decl)->fields.size() && i < struct_info.fields.size(); ++i) {
                    const auto &field = (*struct_decl)->fields[i];
                    if (!field.init) continue;

                    const auto &field_type = struct_info.fields[i].type;
                    const auto init_ty = check_expr(*field.init, empty, module_path, program, diag, field_type, 0);
                    if (!is_assignable(init_ty, field_type)) {
                        diag.report_error(DiagnosticStage::Sema, field.location,
                                          std::format("default value type mismatch for field '{}'", field.name));
                    }
                }
            }
        }

        void check_bodies_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                // A bare-import alias shares its 'decl' AST pointer with the origin — the
                // origin module's own pass over this loop already type-checks that body
                // once, correctly, under the origin's context. Re-checking it here (under
                // the importer's context) would write into the WRONG per-module
                // 'expr_types' side table and spuriously fail to resolve any private
                // sibling the origin's body references.
                if (module.bare_import_origins.contains(name)) continue;
                const auto *fn = std::get_if<FunctionSymbol>(&sym);
                if (!fn) {
                    continue;
                }

                LocalScope locals;
                for (auto &[gname, gsym] : module.symbols) {
                    if (auto *g = std::get_if<GlobalSymbol>(&gsym)) {
                        locals[gname] = LocalBinding{.type = g->type, .is_mut = g->is_mut};
                    }
                }

                for (size_t i = 0; i < fn->decl->params.size(); ++i) {
                    locals[fn->decl->params[i].name] = LocalBinding{.type = fn->params[i], .is_mut = fn->decl->params[i].is_mut};
                }

                check_stmt(fn->decl->body, locals, module_path, program, diag, fn->return_types, 0);
            }

            // Check impl method bodies
            for (auto &method_map : module.methods | std::views::values) {
                for (auto &info : method_map | std::views::values) {
                    if (!info.is_resolved) continue;

                    LocalScope locals;
                    for (auto &[gname, gsym] : module.symbols) {
                        if (auto *g = std::get_if<GlobalSymbol>(&gsym)) {
                            locals[gname] = LocalBinding{.type = g->type, .is_mut = g->is_mut};
                        }
                    }

                    // Bind 'self' as a pointer to the type
                    const auto self_ptr = intern_pointer(program, info.self_type);
                    locals["self"] = LocalBinding{.type = self_ptr, .is_mut = info.is_mut_self};

                    for (size_t i = 0; i < info.decl->params.size(); ++i) {
                        locals[info.decl->params[i].name] = LocalBinding{
                            .type = info.param_types[i],
                            .is_mut = info.decl->params[i].is_mut,
                        };
                    }

                    check_stmt(info.decl->body, locals, module_path, program, diag, info.return_types, 0);
                }
            }
        }

        // Trait-impl method bodies live in a Program-level registry (not per-module
        // ProgramModule::methods), so they need their own body-check pass mirroring the
        // tail of check_bodies_for_module above. Locals/globals-in-scope come from the
        // module the 'impl TRAIT for TYPE' block itself is written in.
        void check_trait_impl_bodies_for_program(Program &program, DiagnosticEngine &diag) {
            for (auto &impls : program.trait_impls_by_type | std::views::values) {
                for (auto &impl_info : impls) {
                    const auto mod_it = program.modules.find(impl_info.impl_module);
                    if (mod_it == program.modules.end()) continue;

                    for (auto &info : impl_info.methods | std::views::values) {
                        if (!info.is_resolved) continue;

                        LocalScope locals;
                        for (auto &[gname, gsym] : mod_it->second.symbols) {
                            if (auto *g = std::get_if<GlobalSymbol>(&gsym)) {
                                locals[gname] = LocalBinding{.type = g->type, .is_mut = g->is_mut};
                            }
                        }

                        const auto self_ptr = intern_pointer(program, info.self_type);
                        locals["self"] = LocalBinding{.type = self_ptr, .is_mut = info.is_mut_self};

                        for (size_t i = 0; i < info.decl->params.size(); ++i) {
                            locals[info.decl->params[i].name] = LocalBinding{
                                .type = info.param_types[i],
                                .is_mut = info.decl->params[i].is_mut,
                            };
                        }

                        check_stmt(info.decl->body, locals, impl_info.impl_module, program, diag, info.return_types, 0);
                    }
                }
            }
        }
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag, const Options &options) -> Program {
        if (!program.ok) {
            return {};
        }

        Program out;
        out.options = options;
        seed_builtin_type_ids(out);

        // Reentrant/memoized rather than a flat loop: a module-scope 'when' condition may
        // reference another module's const (e.g. 'opts.target_os'), which needs that
        // module's symbol table built (and its value resolved) on demand, regardless of
        // Program::modules' unordered iteration order — see ensure_module_declared.
        for (const auto &path : program.modules | std::views::keys) {
            ensure_module_declared(program, path, out, diag);
        }

        // Runs after every module's symbol table (types + bare-impl method names) is
        // built, since coherence/collision/duplicate-impl checks need to see across
        // the whole program, not just one module at a time.
        register_trait_impls_for_program(program, out, diag);

        for (const auto &path : program.modules | std::views::keys) {
            resolve_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_impl_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        resolve_trait_impl_signatures_for_program(out, diag);

        // Every free-function/impl-method/trait-impl-method signature is resolved by this
        // point, but body-checking hasn't started — exactly what the five attributes' own
        // checks need (resolved return types, raw AST param/attribute/body shape).
        for (const auto &path : program.modules | std::views::keys) {
            validate_attributes_for_module(path, out.modules.at(path), out, diag);
            validate_method_attributes_for_module(path, out.modules.at(path), out, diag);
        }
        validate_trait_impl_attributes_for_program(out, diag);

        for (const auto &path : program.modules | std::views::keys) {
            resolve_values_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_struct_field_defaults_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_bodies_for_module(path, out.modules.at(path), out, diag);
        }

        check_trait_impl_bodies_for_program(out, diag);

        // Runs last: the cross-module '@init' reference walk doesn't need type-checked
        // bodies, but keeping it after every other pass makes "last" unambiguous.
        validate_init_dependencies_for_program(program, out, diag);

        out.ok = !diag.has_errors();
        for (auto &module : out.modules | std::views::values) {
            module.ok = out.ok;
        }

        return out;
    }

    auto find_type_module_and_name(const ResolvedType &ty, const Program &program) -> std::pair<std::string, std::string> {
        // The defining module is stored directly on the info struct for every kind. Use
        // it to avoid accidentally matching a bare-import TypeSymbol alias in an
        // importing module that resolves to the same ResolvedType (an alias's '.resolved'
        // is a verbatim copy of the origin's, sharing the same global index).
        const std::string *defining_module = nullptr;
        if (ty.kind == TypeKind::Struct) {
            if (const auto *info = program.struct_at(ty.struct_index)) defining_module = &info->module_path;
        } else if (ty.kind == TypeKind::Union) {
            if (const auto *info = program.union_at(ty.union_index)) defining_module = &info->module_path;
        } else if (ty.kind == TypeKind::Enum) {
            if (const auto *info = program.enum_at(ty.enum_index)) defining_module = &info->module_path;
        } else if (ty.kind == TypeKind::Bitset) {
            if (const auto *info = program.bitset_at(ty.bitset_index)) defining_module = &info->module_path;
        } else if (ty.kind == TypeKind::Trait) {
            if (const auto *info = program.trait_at(ty.trait_index)) defining_module = &info->module_path;
        }

        if (defining_module && !defining_module->empty()) {
            if (const auto mod_it = program.modules.find(*defining_module);
                mod_it != program.modules.end()) {
                for (const auto &[name, sym] : mod_it->second.symbols) {
                    if (const auto *ts = std::get_if<TypeSymbol>(&sym))
                        if (ts->resolved && *ts->resolved == ty)
                            return {*defining_module, name};
                }
            }
        }

        // Fallback for any ResolvedType kind not covered above (or a not-yet-populated
        // module_path). A bare-import alias TypeSymbol can in principle be matched here —
        // acceptable only because every kind that actually needs precise origin
        // attribution (struct/union/enum/bitset/trait, for find_method et al.) is already
        // handled by the fast path above.
        for (const auto &[path, mod] : program.modules) {
            for (const auto &[name, sym] : mod.symbols) {
                if (const auto *ts = std::get_if<TypeSymbol>(&sym))
                    if (ts->resolved && *ts->resolved == ty)
                        return {path, name};
            }
        }
        return {"", ""};
    }

    auto find_method(const ResolvedType &ty, const std::string &method_name, const Program &program) -> const MethodInfo * {
        const auto [mod_path, type_name] = find_type_module_and_name(ty, program);
        if (mod_path.empty()) return nullptr;

        const auto mod_it = program.modules.find(mod_path);
        if (mod_it == program.modules.end()) return nullptr;

        // Tier 1: the type's own bare 'impl' block.
        if (const auto type_it = mod_it->second.methods.find(type_name); type_it != mod_it->second.methods.end()) {
            if (const auto method_it = type_it->second.find(method_name); method_it != type_it->second.end()) {
                return &method_it->second;
            }
        }

        // Tier 2: trait-impl methods for this type, in declaration order. Collision checks at
        // trait-impl declaration time (register_trait_impls_for_program) guarantee at most one
        // trait impl can supply a given method name for a given type, so there's no ambiguity
        // to resolve by order here in a valid program.
        if (const auto trait_impls_it = program.trait_impls_by_type.find({mod_path, type_name});
            trait_impls_it != program.trait_impls_by_type.end()) {
            for (const auto &impl_info : trait_impls_it->second) {
                if (const auto method_it = impl_info.methods.find(method_name); method_it != impl_info.methods.end()) {
                    return &method_it->second;
                }
            }
        }

        return nullptr;
    }
}
