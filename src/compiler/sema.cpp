#include "sema.hpp"

#include <algorithm>
#include <format>
#include <ranges>

namespace sema {
    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diagnostics);
    void register_trait_impls_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag);

    namespace {
        void resolve_signatures_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                if (std::holds_alternative<TypeSymbol>(sym)) {
                    const auto loc = std::get<TypeSymbol>(sym).decl->location;
                    resolve_type_symbol(module_path, name, program, diag, loc);
                }
            }

            for (auto &sym : module.symbols | std::views::values) {
                if (auto *fn = std::get_if<FunctionSymbol>(&sym)) {
                    for (auto &p : fn->decl->params) {
                        const auto pt = resolve_type(p.type, module_path, program, diag);
                        if (p.is_variadic) {
                            fn->is_variadic = true;
                            fn->variadic_element_type = pt;
                            fn->params.push_back(intern_slice(program, pt));
                        } else {
                            fn->params.push_back(pt);
                        }
                    }
                    for (auto &rt : fn->decl->return_types) {
                        fn->return_types.push_back(resolve_type(rt, module_path, program, diag));
                    }
                } else if (auto *ef = std::get_if<ExtFunctionSymbol>(&sym)) {
                    for (auto &p : ef->decl->params) {
                        const auto pt = resolve_type(p.type, module_path, program, diag);
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
                        ef->params.push_back(pt);
                    }

                    if (ef->decl->return_type) {
                        const auto rt = resolve_type(*ef->decl->return_type, module_path, program, diag);
                        if (rt.kind == TypeKind::Union) {
                            diag.report_error(DiagnosticStage::Sema, ef->decl->location, "union types are not yet supported in extern function signatures");
                        }
                        if (rt.kind == TypeKind::Trait) {
                            diag.report_error(DiagnosticStage::Sema, ef->decl->location, "trait handles have no C ABI representation and cannot appear in 'ext fn' signatures");
                        }
                        if (rt.kind == TypeKind::Function) {
                            const auto *sig = program.fn_signature_at(rt.fn_index);
                            if (sig && sig->return_types.size() > 1) {
                                diag.report_error(DiagnosticStage::Sema, ef->decl->location, "multi-return function types cannot be used in extern function signatures (no C ABI representation)");
                            }
                        }
                        ef->return_type = rt;
                    }

                    ef->is_variadic = ef->decl->is_variadic;
                }
            }
        }

        void resolve_values_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                if (std::holds_alternative<GlobalSymbol>(sym)) {
                    const auto loc = std::get<GlobalSymbol>(sym).decl->location;
                    resolve_global_symbol(module_path, name, program, diag, loc);
                } else if (std::holds_alternative<MacroSymbol>(sym)) {
                    const auto loc = std::get<MacroSymbol>(sym).decl->location;
                    resolve_macro_symbol(module_path, name, program, diag, loc);
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
                        const auto pt = resolve_type(p.type, module_path, program, diag);
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

                    info.is_resolved = true;
                }
            }
        }

        // Minimal human-readable rendering of a resolved type, used only for trait
        // conformance diagnostics (there is no general ResolvedType-to-string formatter
        // elsewhere in sema).
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
                            const auto pt = resolve_type(p.type, impl_info.impl_module, program, diag);
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

                        info.is_resolved = true;
                    }

                    const auto *trait_info = program.trait_at(impl_info.trait_index);
                    if (!trait_info) continue;

                    for (const auto &trait_method : trait_info->methods) {
                        const auto it = impl_info.methods.find(trait_method.name);
                        if (it == impl_info.methods.end()) {
                            diag.report_error(DiagnosticStage::Sema, impl_info.location,
                                std::format("missing implementation of trait method '{}' in 'impl {} for {}'",
                                    trait_method.name, impl_info.trait_name, impl_info.type_name));
                            continue;
                        }

                        const auto &impl_method = it->second;
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
            for (const auto &sym : module.symbols | std::views::values) {
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
            for (auto &sym : module.symbols | std::views::values) {
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

    auto check_program(const ast::Program &program, DiagnosticEngine &diag) -> Program {
        if (!program.ok) {
            return {};
        }

        Program out;

        for (auto &[path, decls] : program.modules) {
            build_symbol_table_for_module(program, path, out.modules[path], out, decls, diag);
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

        out.ok = !diag.has_errors();
        for (auto &module : out.modules | std::views::values) {
            module.ok = out.ok;
        }

        return out;
    }

    auto find_type_module_and_name(const ResolvedType &ty, const Program &program) -> std::pair<std::string, std::string> {
        // For struct/union, the defining module is stored directly on the info struct.
        // Use it to avoid accidentally matching a type alias in an importing module that
        // resolves to the same ResolvedType.
        const std::string *defining_module = nullptr;
        if (ty.kind == TypeKind::Struct) {
            if (const auto *info = program.struct_at(ty.struct_index)) defining_module = &info->module_path;
        } else if (ty.kind == TypeKind::Union) {
            if (const auto *info = program.union_at(ty.union_index)) defining_module = &info->module_path;
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

        // Fallback for enums and other types that don't store a module_path.
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
