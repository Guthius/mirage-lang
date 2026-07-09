#include "sema.hpp"

#include <format>
#include <ranges>

namespace sema {
    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diagnostics);

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
                        if (pt.kind == TypeKind::Function) {
                            const auto &sig = program.fn_signatures.at(pt.fn_index);
                            if (sig.return_types.size() > 1) {
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
                        if (rt.kind == TypeKind::Function) {
                            const auto &sig = program.fn_signatures.at(rt.fn_index);
                            if (sig.return_types.size() > 1) {
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
                        info.param_types.push_back(resolve_type(p.type, module_path, program, diag));
                    }
                    for (auto &rt : info.decl->return_types) {
                        info.return_types.push_back(resolve_type(rt, module_path, program, diag));
                    }

                    info.is_resolved = true;
                }
            }
        }

        void check_struct_field_defaults_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (const auto &sym : module.symbols | std::views::values) {
                const auto *ts = std::get_if<TypeSymbol>(&sym);
                if (!ts || !ts->resolved || ts->resolved->kind != TypeKind::Struct) continue;

                const auto *struct_decl = std::get_if<std::unique_ptr<ast::StructType>>(&ts->decl->type);
                if (!struct_decl) continue;

                const auto &struct_info = program.structs.at(ts->resolved->struct_index);
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
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag) -> Program {
        if (!program.ok) {
            return {};
        }

        Program out;

        for (auto &[path, decls] : program.modules) {
            build_symbol_table_for_module(program, path, out.modules[path], out, decls, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_impl_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_values_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_struct_field_defaults_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_bodies_for_module(path, out.modules.at(path), out, diag);
        }

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
        if (ty.kind == TypeKind::Struct && ty.struct_index >= 0)
            defining_module = &program.structs[ty.struct_index].module_path;
        else if (ty.kind == TypeKind::Union && ty.union_index >= 0)
            defining_module = &program.unions[ty.union_index].module_path;

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

        const auto type_it = mod_it->second.methods.find(type_name);
        if (type_it == mod_it->second.methods.end()) return nullptr;

        const auto method_it = type_it->second.find(method_name);
        if (method_it == type_it->second.end()) return nullptr;

        return &method_it->second;
    }
}
