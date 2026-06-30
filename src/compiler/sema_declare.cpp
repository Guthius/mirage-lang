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
        void declare_type(const ast::TypeDecl &decl, ProgramModule &module, Program &sema_program, DiagnosticEngine &diag) {
            std::optional<ResolvedType> resolved = std::nullopt;

            int struct_slot = -1;
            int enum_slot = -1;
            if (std::holds_alternative<std::unique_ptr<ast::StructType>>(decl.type)) {
                struct_slot = static_cast<int>(module.structs.size());
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
            }

            if (!declare_symbol(module.symbols, decl.name, TypeSymbol{.decl = &decl, .resolved = resolved, .is_pub = decl.is_pub, .location = decl.location}, decl.location, diag)) {
                return;
            }

            if (struct_slot >= 0) {
                module.structs.push_back(StructInfo{.is_packed = std::get<std::unique_ptr<ast::StructType>>(decl.type)->is_packed});
            }
            if (enum_slot >= 0) {
                sema_program.enums.push_back(EnumInfo{});
            }
        }

        void declare_global(const ast::VarDecl &decl, const ast::Program &program, const std::string &module_path, ProgramModule &module, DiagnosticEngine &diag) {
            if (decl.init && std::holds_alternative<ast::ImportExpr>(*decl.init)) {
                const auto &import_expr = std::get<ast::ImportExpr>(*decl.init);

                if (const auto resolved_path = resolve_import(program, module_path, import_expr.module_name); resolved_path.empty()) {
                    diag.report_error(DiagnosticStage::Sema, import_expr.location, std::format("import '{}' was not resolved", import_expr.module_name));
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
                        declare_global(v, program, module_path, module, diag);
                    } else if constexpr (std::is_same_v<V, ast::MacroDecl>) {
                        declare_symbol(module.symbols, v.name, MacroSymbol{.decl = &v, .is_pub = v.is_pub, .is_resolved = false}, v.location, diag);
                    } else if constexpr (std::is_same_v<V, ast::TypeDecl>) {
                        declare_type(v, module, sema_program, diag);
                    }
                },
                decl);
        }
    }
}
