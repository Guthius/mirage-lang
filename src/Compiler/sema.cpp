#include "sema.hpp"

#include <ranges>

namespace sema {
    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, const ast::Module &decls, DiagnosticEngine &diagnostics);

    namespace {
        void resolve_signatures_for_module(const std::string &module_path, ProgramModule &module, ProgramResult &program, DiagnosticEngine &diag) {
            for (auto &[name, sym] : module.symbols) {
                if (std::holds_alternative<TypeSymbol>(sym)) {
                    const auto loc = std::get<TypeSymbol>(sym).decl->location;
                    resolve_type_symbol(module_path, name, program, diag, loc);
                }
            }

            for (auto &sym : module.symbols | std::views::values) {
                if (auto *fn = std::get_if<FunctionSymbol>(&sym)) {
                    for (auto &p : fn->decl->params) {
                        fn->params.push_back(resolve_type(p.type, module_path, program, diag));
                    }
                    for (auto &rt : fn->decl->return_types) {
                        fn->return_types.push_back(resolve_type(rt, module_path, program, diag));
                    }
                } else if (auto *ef = std::get_if<ExtFunctionSymbol>(&sym)) {
                    for (auto &p : ef->decl->params) {
                        ef->params.push_back(resolve_type(p.type, module_path, program, diag));
                    }

                    if (ef->decl->return_type) {
                        ef->return_type = resolve_type(*ef->decl->return_type, module_path, program, diag);
                    }
                }
            }
        }

        void resolve_values_for_module(const std::string &module_path, ProgramModule &module, ProgramResult &program, DiagnosticEngine &diag) {
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

        void check_bodies_for_module(const std::string &module_path, ProgramModule &module, ProgramResult &program, DiagnosticEngine &diag) {
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
        }
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag) -> ProgramResult {
        if (!program.ok) {
            return {};
        }

        ProgramResult out;

        for (auto &[path, decls] : program.modules) {
            build_symbol_table_for_module(program, path, out.modules[path], decls, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_values_for_module(path, out.modules.at(path), out, diag);
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
}
