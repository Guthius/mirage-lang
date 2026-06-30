#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"
#include "module_resolver.hpp"
#include "resolved_type.hpp"
#include "symbol_table.hpp"

#include <optional>
#include <set>
#include <unordered_map>

namespace sema {

    struct StructField {
        std::string name;
        ResolvedType type;
        uint32_t offset = 0;
    };

    struct StructInfo {
        std::vector<StructField> fields;
        uint32_t size = 0;
        uint32_t align = 1;
        bool is_packed = false;
        bool layout_done = false;
    };

    struct ArrayInfo {
        ResolvedType element_type;
        uint64_t count = 0;
        uint32_t size = 0;
        uint32_t align = 1;
    };

    struct SliceInfo {
        ResolvedType element_type;
    };

    struct EnumFieldInfo {
        std::string name;
        int64_t value = 0;
    };

    struct EnumInfo {
        ResolvedType underlying_type;
        std::vector<EnumFieldInfo> fields;
        bool layout_done = false;
    };

    struct ProgramModule {
        SymbolTable symbols;
        std::vector<ResolvedType> pointer_pointees;
        std::vector<StructInfo> structs;
        std::vector<ArrayInfo> arrays;
        std::vector<SliceInfo> slices;
        std::unordered_map<const void *, ResolvedType> expr_types;
        bool ok = false;
    };

    struct ResolveState {
        std::set<std::pair<std::string, std::string>> alias_resolving;
        std::set<std::pair<std::string, std::string>> struct_resolving;
        std::set<std::pair<std::string, std::string>> value_resolving;
    };

    struct Program {
        std::unordered_map<std::string, ProgramModule> modules;
        std::vector<EnumInfo> enums;
        ResolveState resolve_state;
        bool ok = false;
    };

    struct LocalBinding {
        ResolvedType type;
        bool is_mut = false;
    };

    using LocalScope = std::unordered_map<std::string, LocalBinding>;

    inline auto get_expr_key(const ast::Expr &expr) -> const void * {
        return std::visit([](const auto &v) -> const void * { return &v; }, expr);
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag) -> Program;
    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType;
    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool;
    auto intern_pointer(ProgramModule &module, const ResolvedType &pointee) -> ResolvedType;
    auto intern_slice(ProgramModule &module, const ResolvedType &element) -> ResolvedType;
    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_global_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType;
    auto resolve_macro_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> MacroSymbol &;
    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, std::optional<ResolvedType> expected, int loop_depth) -> ResolvedType;
    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth) -> void;
    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program) -> bool;
}
