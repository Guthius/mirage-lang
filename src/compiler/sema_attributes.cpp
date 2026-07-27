#include "sema.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <set>

namespace sema {
    auto find_attribute(const std::vector<ast::Attribute> &attrs, const std::string_view name) -> const ast::Attribute * {
        for (const auto &attr : attrs) {
            if (attr.name == name) return &attr;
        }
        return nullptr;
    }

    namespace {
        auto is_void_or_error_return(const std::vector<ResolvedType> &return_types, const Program &program) -> bool {
            if (return_types.empty()) return true;
            if (return_types.size() != 1) return false;
            const auto &rt = return_types.front();
            if (rt.kind != TypeKind::Union) return false;
            const auto *info = program.union_at(rt.union_index);
            return info != nullptr && info->is_error_union;
        }

        // Mirrors sema::get_expr_location's by-value/boxed-alternative handling, but for Stmt.
        auto stmt_location(const ast::Stmt &stmt) -> SourceLocation {
            return std::visit([](const auto &v) -> SourceLocation {
                if constexpr (requires { v->location; }) {
                    return v->location;
                } else {
                    return v.location;
                }
            }, stmt);
        }

        // '@naked' requires the function body to contain only inline-asm statements — purely
        // structural, unrelated to check_asm_instructions/check_asm_stmt's semantic validation
        // (sema_check.cpp). Returns the first offending statement, or nullptr if the body is
        // all-asm (or isn't a block at all, which shouldn't happen for a real function body).
        auto find_first_non_asm_stmt(const ast::Stmt &body) -> const ast::Stmt * {
            const auto *block = std::get_if<std::unique_ptr<ast::BlockStmt>>(&body);
            if (!block) return nullptr;
            for (const auto &stmt : (*block)->stmts) {
                if (!std::holds_alternative<std::unique_ptr<ast::AsmStmt>>(stmt)) {
                    return &stmt;
                }
            }
            return nullptr;
        }

        void validate_no_return_attribute(const ast::Attribute &attr, const FunctionSymbol &fn, const Program &program, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@no_return' takes no arguments");
            }
            if (!is_void_or_error_return(fn.return_types, program)) {
                diag.warn(DiagnosticStage::Sema, attr.location,
                    "a '@no_return' function with a value return type will never return its value");
            }
        }

        void validate_naked_attribute(const ast::Attribute &attr, const ast::FunctionDecl &decl, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@naked' takes no arguments");
            }
            if (const auto *bad_stmt = find_first_non_asm_stmt(decl.body)) {
                diag.warn(DiagnosticStage::Sema, stmt_location(*bad_stmt),
                    "'@naked' function contains a non-'asm' statement; naked functions should "
                    "contain only inline 'asm' blocks, since the compiler emits no prologue/"
                    "epilogue for them");
            }
        }

        void validate_always_inline_attribute(const ast::Attribute &attr, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@always_inline' takes no arguments");
            }
        }

        void validate_section_attribute(const ast::Attribute &attr, const std::string &module_path, Program &program, DiagnosticEngine &diag) {
            if (attr.args.size() != 1) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@section' requires exactly one string argument");
                return;
            }

            LocalScope empty;
            const auto u8_slice = intern_slice(program, ResolvedType{.kind = TypeKind::U8});
            const auto arg_type = check_expr(attr.args[0], empty, module_path, program, diag, u8_slice, 0);
            if (!is_assignable(arg_type, u8_slice)) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@section' argument must be a compile-time constant '[]u8' expression");
                return;
            }
            if (!is_constant_expr(attr.args[0], module_path, program)) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@section' argument must be a compile-time constant expression");
                return;
            }
            if (const auto folded = evaluate_const_value(attr.args[0], module_path, program, diag)) {
                if (const auto *str = std::get_if<std::string>(&*folded); str && str->empty()) {
                    diag.report_error(DiagnosticStage::Sema, attr.location, "'@section' argument must not be an empty string");
                }
            }
        }

        void validate_init_structural(const ast::Attribute &attr, const ast::FunctionDecl &decl, const FunctionSymbol &fn, const Program &program, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' takes no arguments");
            }
            if (!decl.params.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' functions may not take parameters");
            }
            // Deliberately no 'pub' requirement: '_init' is emitted into the same LLVM module
            // as every other function in the program (this is a whole-program compile, not
            // separate translation units linked afterward), so it can call a private '@init'
            // function directly — and in the common case, a module's initializer is exactly
            // the kind of thing that should NOT be an ordinary externally-callable symbol.
            if (!is_void_or_error_return(fn.return_types, program)) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' functions must return nothing or 'error(...)'");
            }
        }
    }

    // Runs after every free function/impl-method/trait-impl-method signature is resolved
    // (called from check_program right after resolve_trait_impl_signatures_for_program), but
    // before body-checking — none of the five attributes' own checks need type-checked
    // bodies, only resolved signatures and raw AST shape (attribute args, param/return lists,
    // the body's top-level statement list for '@naked').
    void validate_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
        for (auto &sym : module.symbols | std::views::values) {
            auto *fn = std::get_if<FunctionSymbol>(&sym);
            if (!fn || !fn->decl || fn->decl->attributes.empty()) continue;
            const auto &attrs = fn->decl->attributes;

            if (const auto *a = find_attribute(attrs, "no_return")) validate_no_return_attribute(*a, *fn, program, diag);
            if (const auto *a = find_attribute(attrs, "naked")) validate_naked_attribute(*a, *fn->decl, diag);
            if (const auto *a = find_attribute(attrs, "always_inline")) validate_always_inline_attribute(*a, diag);
            if (const auto *a = find_attribute(attrs, "section")) validate_section_attribute(*a, module_path, program, diag);
            if (const auto *init_attr = find_attribute(attrs, "init")) {
                validate_init_structural(*init_attr, *fn->decl, *fn, program, diag);

                if (find_attribute(attrs, "no_return")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@no_return' cannot be combined: an initializer must return "
                        "control so the next one can run");
                }
                if (find_attribute(attrs, "naked")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@naked' cannot be combined: init functions must be callable normally");
                }
                if (find_attribute(attrs, "always_inline")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@always_inline' cannot be combined: init functions are called "
                        "from the generated '_init', inlining them defeats the purpose");
                }
            }
            if (find_attribute(attrs, "naked") && find_attribute(attrs, "always_inline")) {
                diag.report_error(DiagnosticStage::Sema, find_attribute(attrs, "naked")->location,
                    "'@naked' and '@always_inline' cannot be combined: a naked function has no body to inline");
            }
        }
    }

    namespace {
        using ForeignRefCallback = std::function<void(const std::string &target_module, const std::string &symbol_name, const SourceLocation &loc)>;

        void walk_expr_for_foreign_refs(const ast::Expr &expr, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);
        void walk_stmt_for_foreign_refs(const ast::Stmt &stmt, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);
        void walk_when_stmt_for_foreign_refs(const ast::WhenStmt &when, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);

        // Resolves 'expr' to a module path IF it denotes a namespace value: an import-alias
        // identifier (`const opts := import(...); opts.field`), an inline 'import(...)'
        // expression (`import(...).field`), or a chain of '.field' accesses through re-exported
        // namespaces. Returns nullopt for any ordinary value expression. Mirrors
        // sema_check.cpp's identically-shaped try_resolve_namespace_chain, which can't be
        // reused directly since it's private to that file's anonymous namespace.
        auto resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, const std::set<std::string> &locals, const Program &program) -> std::optional<std::string> {
            if (const auto *imp = std::get_if<ast::ImportExpr>(&expr)) {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) return std::nullopt;
                const auto path_it = mod_it->second.inline_import_paths.find(imp);
                if (path_it == mod_it->second.inline_import_paths.end()) return std::nullopt;
                return path_it->second;
            }
            if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                if (locals.contains(ident->name)) return std::nullopt;
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) return std::nullopt;
                const auto sym_it = mod_it->second.symbols.find(ident->name);
                if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                if (const auto *imp_sym = std::get_if<ImportSymbol>(&sym_it->second)) return imp_sym->module_path;
                return std::nullopt;
            }
            if (const auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                const auto inner = resolve_namespace_chain((*mem)->object, module_path, locals, program);
                if (!inner) return std::nullopt;
                const auto mod_it = program.modules.find(*inner);
                if (mod_it == program.modules.end()) return std::nullopt;
                const auto sym_it = mod_it->second.symbols.find((*mem)->member);
                if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                if (const auto *imp_sym = std::get_if<ImportSymbol>(&sym_it->second); imp_sym && imp_sym->is_pub) {
                    return imp_sym->module_path;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        // Exhaustive walk over every ast::Expr alternative, invoking 'on_foreign_ref' for every
        // '<namespace>.<member>' access whose base resolves (via resolve_namespace_chain) to a
        // module other than 'module_path'. This is a structural reference-finder, not a type
        // checker — it deliberately doesn't need LocalScope/expr_types, since it only cares
        // whether an identifier names an import alias, not what type anything has. 'locals'
        // tracks names bound so far (params, 'mut'/'const' locals, for-loop index/element
        // names) so a shadowing local never gets mistaken for a module-level import alias.
        void walk_expr_for_foreign_refs(const ast::Expr &expr, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            std::visit([&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> || std::is_same_v<V, ast::LiteralFloatExpr> ||
                              std::is_same_v<V, ast::LiteralStringExpr> || std::is_same_v<V, ast::LiteralCharExpr> ||
                              std::is_same_v<V, ast::LiteralBoolExpr> || std::is_same_v<V, ast::LiteralNilExpr> ||
                              std::is_same_v<V, ast::IdentExpr> || std::is_same_v<V, ast::ImportBinExpr> ||
                              std::is_same_v<V, ast::IotaExpr> || std::is_same_v<V, ast::DotIdentExpr> ||
                              std::is_same_v<V, ast::DefaultExpr> || std::is_same_v<V, ast::UndefinedExpr> ||
                              std::is_same_v<V, std::unique_ptr<ast::TypeExpr>> || std::is_same_v<V, std::unique_ptr<ast::AsmExpr>>) {
                    // Leaves w.r.t. foreign-reference detection: no nested Expr fields relevant
                    // here (TypeExpr wraps a Type, not a value; AsmExpr's operands are
                    // registers/immediates/local variables only, never cross-module symbols).

                } else if constexpr (std::is_same_v<V, ast::ImportExpr>) {
                    // Handled by the MemberExpr case below (when this IS the '.object' of one) —
                    // a bare 'import(...)' with no '.field' access doesn't reference any symbol.

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    if (const auto target_module = resolve_namespace_chain(v->object, module_path, locals, program)) {
                        on_foreign_ref(*target_module, v->member, v->location);
                    } else {
                        walk_expr_for_foreign_refs(v->object, locals, module_path, program, on_foreign_ref);
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    walk_expr_for_foreign_refs(v->lhs, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->rhs, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->then_expr, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->else_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->then_expr, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->else_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    walk_expr_for_foreign_refs(v->target, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                    walk_expr_for_foreign_refs(v->callee, locals, module_path, program, on_foreign_ref);
                    for (const auto &arg : v->args) walk_expr_for_foreign_refs(arg, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                    if (v->default_value) walk_expr_for_foreign_refs(*v->default_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                    if (v->default_value) walk_expr_for_foreign_refs(*v->default_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StackAllocExpr>>) {
                    walk_expr_for_foreign_refs(v->size, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    walk_expr_for_foreign_refs(v->value, locals, module_path, program, on_foreign_ref);
                    if (v->len_expr) walk_expr_for_foreign_refs(*v->len_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->index, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->start, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->end, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    for (const auto &arm : v->arms) {
                        if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                            walk_expr_for_foreign_refs(*lit->expr, locals, module_path, program, on_foreign_ref);
                        }
                        walk_expr_for_foreign_refs(arm.value, locals, module_path, program, on_foreign_ref);
                    }
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    std::visit([&]<typename BT>(const BT &bv) {
                        using BV = std::decay_t<BT>;
                        if constexpr (std::is_same_v<BV, ast::StructExpr>) {
                            for (const auto &field : bv.fields) walk_expr_for_foreign_refs(field.expr, locals, module_path, program, on_foreign_ref);
                        } else if constexpr (std::is_same_v<BV, ast::ArrayExpr>) {
                            for (const auto &value : bv.values) walk_expr_for_foreign_refs(value, locals, module_path, program, on_foreign_ref);
                        }
                        // EmptyExpr, BitsetExpr: leaves, nothing further to recurse into.
                    }, *v);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                    if (v->payload) {
                        for (const auto &field : v->payload->fields) walk_expr_for_foreign_refs(field.expr, locals, module_path, program, on_foreign_ref);
                    }
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                    walk_expr_for_foreign_refs(v->call, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::RangeExpr>>) {
                    if (v->lower) walk_expr_for_foreign_refs(*v->lower, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->upper, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SpreadExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else {
                    static_assert(!sizeof(V), "walk_expr_for_foreign_refs: unhandled ast::Expr alternative");
                }
            }, expr);
        }

        void walk_when_stmt_for_foreign_refs(const ast::WhenStmt &when, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            walk_expr_for_foreign_refs(when.condition, locals, module_path, program, on_foreign_ref);
            for (const auto &s : when.then_block.stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
            if (when.else_branch) {
                std::visit([&]<typename ET>(const ET &ev) {
                    using EV = std::decay_t<ET>;
                    if constexpr (std::is_same_v<EV, ast::BlockStmt>) {
                        for (const auto &s : ev.stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
                    } else if constexpr (std::is_same_v<EV, std::unique_ptr<ast::WhenStmt>>) {
                        walk_when_stmt_for_foreign_refs(*ev, locals, module_path, program, on_foreign_ref);
                    } else {
                        static_assert(!sizeof(EV), "walk_when_stmt_for_foreign_refs: unhandled else_branch alternative");
                    }
                }, *when.else_branch);
            }
        }

        // Exhaustive walk over every ast::Stmt alternative. See walk_expr_for_foreign_refs's
        // doc comment above for the general approach; 'locals' is threaded through and grown
        // as declarations are encountered, in source order, matching how references can only
        // ever target an already-declared local.
        void walk_stmt_for_foreign_refs(const ast::Stmt &stmt, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            std::visit([&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    for (const auto &s : v->stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_stmt_for_foreign_refs(v->then_stmt, locals, module_path, program, on_foreign_ref);
                    if (v->else_stmt) walk_stmt_for_foreign_refs(*v->else_stmt, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                    walk_expr_for_foreign_refs(v->iterable, locals, module_path, program, on_foreign_ref);
                    locals.insert(v->index_name);
                    locals.insert(v->element_name);
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    for (const auto &arm : v->arms) {
                        if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                            walk_expr_for_foreign_refs(*lit->expr, locals, module_path, program, on_foreign_ref);
                        }
                        walk_stmt_for_foreign_refs(arm.body, locals, module_path, program, on_foreign_ref);
                    }
                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    walk_expr_for_foreign_refs(v.expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    if (v.init) walk_expr_for_foreign_refs(*v.init, locals, module_path, program, on_foreign_ref);
                    locals.insert(v.name);
                } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                    walk_expr_for_foreign_refs(v.init, locals, module_path, program, on_foreign_ref);
                    for (const auto &name : v.names) locals.insert(name);
                } else if constexpr (std::is_same_v<V, ast::ContinueStmt> || std::is_same_v<V, ast::BreakStmt>) {
                    // leaves
                } else if constexpr (std::is_same_v<V, ast::ReturnStmt> || std::is_same_v<V, ast::ReturnOkStmt>) {
                    for (const auto &value : v.return_values) walk_expr_for_foreign_refs(value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                    walk_expr_for_foreign_refs(v.error_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                    walk_expr_for_foreign_refs(v.data, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                    walk_expr_for_foreign_refs(v.message, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                    walk_when_stmt_for_foreign_refs(*v, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) {
                    // leaf: asm operands are registers/immediates/local variables only.
                } else {
                    static_assert(!sizeof(V), "walk_stmt_for_foreign_refs: unhandled ast::Stmt alternative");
                }
            }, stmt);
        }
    }

    // Builds the '@init' cross-module dependency graph from ACTUAL symbol references made in
    // '@init' function bodies (not the coarser "module A imports module B" signal), then
    // topologically sorts it into Program::init_call_order. Dependency *nodes* are modules
    // that declare at least one '@init' function (same-module '@init' functions run in
    // source declaration order — no cycle is possible there by construction); an edge
    // 'A -> B' means some '@init' function in module A references a symbol declared in
    // module B, and B itself has '@init' function(s) that must therefore run first.
    void validate_init_dependencies_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag) {
        struct InitFn {
            std::string name;
            const ast::FunctionDecl *decl = nullptr;
        };

        std::map<std::string, std::vector<InitFn>> by_module; // preserves ast::Module's source order
        std::set<std::string> has_init;

        for (const auto &[module_path, decls] : ast_program.modules) {
            for (const auto &decl : decls) {
                if (const auto *fn_decl = std::get_if<ast::FunctionDecl>(&decl)) {
                    if (find_attribute(fn_decl->attributes, "init")) {
                        by_module[module_path].push_back(InitFn{.name = fn_decl->name, .decl = fn_decl});
                        has_init.insert(module_path);
                    }
                }
            }
        }
        if (has_init.empty()) return;

        // module -> (dependency module -> first-seen referencing location)
        std::map<std::string, std::map<std::string, SourceLocation>> edges;
        for (const auto &module_path : has_init) {
            for (const auto &init_fn : by_module.at(module_path)) {
                std::set<std::string> locals;
                walk_stmt_for_foreign_refs(init_fn.decl->body, locals, module_path, sema_program,
                    [&](const std::string &target_module, const std::string &, const SourceLocation &loc) {
                        if (target_module == module_path || !has_init.contains(target_module)) return;
                        auto &module_edges = edges[module_path];
                        module_edges.try_emplace(target_module, loc);
                    });
            }
        }

        // Kahn's algorithm: a module's in-degree is how many other has-init modules IT
        // depends on (edges[m].size()); it becomes ready once all of those have been placed.
        std::map<std::string, int> in_degree;
        for (const auto &m : has_init) in_degree[m] = static_cast<int>(edges[m].size());

        std::map<std::string, std::vector<std::string>> dependents; // B -> every A with edges[A] containing B
        for (const auto &[a, deps] : edges) {
            for (const auto &b : deps | std::views::keys) dependents[b].push_back(a);
        }

        std::vector<std::string> ready;
        for (const auto &[m, deg] : in_degree) {
            if (deg == 0) ready.push_back(m);
        }

        std::vector<std::string> order;
        while (!ready.empty()) {
            std::ranges::sort(ready); // deterministic among mutually-independent modules
            const auto m = ready.front();
            ready.erase(ready.begin());
            order.push_back(m);
            for (const auto &dependent : dependents[m]) {
                if (--in_degree[dependent] == 0) ready.push_back(dependent);
            }
        }

        if (order.size() != has_init.size()) {
            std::vector<std::string> stuck;
            for (const auto &[m, deg] : in_degree) {
                if (deg > 0) stuck.push_back(m);
            }
            std::ranges::sort(stuck);
            const auto &a = stuck.front();
            const auto &[b, loc] = *edges.at(a).begin();
            diag.report_error(DiagnosticStage::Sema, loc, std::format(
                "circular '@init' dependency: module '{}' requires module '{}' to initialize "
                "first, but '{}' also (transitively) requires '{}' to initialize first",
                a, b, b, a));
            return;
        }

        for (const auto &module_path : order) {
            for (const auto &init_fn : by_module.at(module_path)) {
                sema_program.init_call_order.emplace_back(module_path, init_fn.name);
            }
        }
    }
}
