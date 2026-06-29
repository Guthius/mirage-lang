#include "sema.hpp"

#include <format>

namespace sema {
    auto declare_symbol(SymbolTable &symbol_table, std::string name, Symbol symbol, const SourceLocation &location, DiagnosticEngine &diagnostics) -> bool {
        if (symbol_table.contains(name)) {
            diagnostics.report_error(DiagnosticStage::Sema, location, std::format("redefinition if '{}'", name));
            return false;
        }

        symbol_table[name] = std::move(symbol);
        return true;
    }

    void declare_type(const ast::TypeDecl &decl, ProgramModule &module, DiagnosticEngine &diagnostics) {
        if (module.symbols.contains(decl.name)) {
            diagnostics.report_error(DiagnosticStage::Sema, decl.location, std::format("redefinition if '{}'", decl.name));
            return;
        }

        std::optional<ResolvedType> resolved = std::nullopt;
        if (std::holds_alternative<std::unique_ptr<ast::StructType>>(decl.type)) {
            module.structs.push_back(StructInfo{
                .is_packed = std::get<std::unique_ptr<ast::StructType>>(decl.type)->is_packed,
            });

            resolved = ResolvedType{
                .kind = TypeKind::Struct,
                .struct_index = static_cast<int>(module.structs.size()) - 1,
            };
        }

        module.symbols[decl.name] = TypeSymbol{
            .decl = &decl,
            .resolved = resolved,
            .is_pub = decl.is_pub,
            .location = decl.location,
        };
    }

    auto resolve_import(const ast::Program &program, const std::string &importer, const std::string &imported) -> std::string {
        if (const auto importer_it = program.module_imports.find(importer); importer_it != program.module_imports.end()) {
            if (const auto module_it = importer_it->second.find(imported); module_it != importer_it->second.end()) {
                return module_it->second;
            }
        }

        return {};
    }

    void declare_global(const ast::VarDecl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, DiagnosticEngine &diagnostics) {
        if (module.symbols.contains(decl.name)) {
            diagnostics.report_error(DiagnosticStage::Sema, decl.location, std::format("redefinition if '{}'", decl.name));
            return;
        }

        if (decl.init && std::holds_alternative<ast::ImportExpr>(*decl.init)) {
            const auto &[imported_module_name, location] = std::get<ast::ImportExpr>(*decl.init);

            if (const auto resolved_path = resolve_import(program, module_path, imported_module_name); module_path.empty()) {
                diagnostics.report_error(DiagnosticStage::Sema, location, std::format("import '{}' was not resolved", imported_module_name));
            } else {
                declare_symbol(
                    module.symbols,
                    decl.name,
                    ImportSymbol{
                        .module_path = resolved_path,
                        .is_pub = decl.is_pub,
                    },
                    decl.location, diagnostics);
            }

            return;
        }

        module.symbols[decl.name] = GlobalSymbol{
            .type = ResolvedType{.kind = TypeKind::Invalid},
            .is_mut = decl.is_mut,
            .is_pub = decl.is_pub,
        };
    }

    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, const ast::Module &decls, DiagnosticEngine &diagnostics) {
        for (auto &decl : decls) {
            std::visit(
                [&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::FunctionDecl>) {
                        declare_symbol(module.symbols, v.name, FunctionSymbol{.is_pub = v.is_pub}, v.location, diagnostics);
                    } else if constexpr (std::is_same_v<V, ast::ExtFunctionDecl>) {
                        declare_symbol(module.symbols, v.name, ExtFunctionSymbol{.is_pub = v.is_pub}, v.location, diagnostics);
                    } else if constexpr (std::is_same_v<V, ast::VarDecl>) {
                        declare_global(v, program, module_path, module, diagnostics);
                    } else if constexpr (std::is_same_v<V, ast::MacroDecl>) {
                        declare_symbol(module.symbols, v.name, MacroSymbol{.is_pub = v.is_pub}, v.location, diagnostics);
                    } else if constexpr (std::is_same_v<V, ast::TypeDecl>) {
                        declare_type(v, module, diagnostics);
                    }
                },
                decl);
        }
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diagnostics) -> ProgramResult {
        ProgramResult res;
        if (!program.ok) {
            return res;
        }

        for (auto &[path, decls] : program.modules) {
            build_symbol_table_for_module(program, path, res.modules[path], decls, diagnostics);
        }

        return res;
    }
}
