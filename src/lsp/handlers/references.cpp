#include "references.hpp"

#include "../uri.hpp"
#include "ast_walker.hpp"
#include "common.hpp"

#include <type_traits>

namespace lsp::handlers {
    namespace {
        using json = nlohmann::json;

        auto location_json(const SourceLocation &loc) -> json {
            if (loc.filename.empty()) return nullptr;
            const auto zero_line = loc.line == 0 ? 0 : loc.line - 1;
            const auto zero_column = loc.column == 0 ? 0 : loc.column - 1;
            return {
                {"uri", path_to_uri(std::string(loc.filename))},
                {"range", {
                    {"start", {{"line", zero_line}, {"character", zero_column}}},
                    {"end", {{"line", zero_line}, {"character", zero_column}}},
                }},
            };
        }

        // Whether a freshly-resolved reference `res` names the same declaration as `target`.
        // Struct/union/method info is compared by pointer identity - stable for the lifetime
        // of one ProgramResult, since nothing mutates program.structs/unions/methods after
        // analysis finishes. Local/param declarations have no comparable pointer, so identity
        // is their own unique declaration-site SourceLocation instead.
        auto same_declaration(const Resolution &a, const Resolution &b) -> bool {
            if (a.kind != b.kind) return false;
            switch (a.kind) {
            case Resolution::Kind::Symbol:
                return a.symbol == b.symbol;
            case Resolution::Kind::Local:
            case Resolution::Kind::Param:
                return a.location.filename == b.location.filename && a.location.line == b.location.line &&
                       a.location.column == b.location.column;
            case Resolution::Kind::StructField:
                return a.struct_field == b.struct_field;
            case Resolution::Kind::UnionMember:
                return a.union_member == b.union_member;
            case Resolution::Kind::Method:
                return a.method == b.method;
            case Resolution::Kind::EnumField:
            case Resolution::Kind::Variant:
                return a.name == b.name && a.type.kind == b.type.kind && a.type.enum_index == b.type.enum_index &&
                       a.type.union_index == b.type.union_index;
            default:
                return false; // Builtin/None are not meaningful reference targets
            }
        }

        // One function/method-like scope to walk: its params (base-name resolution) and body
        // (locals-in-scope resolution via find_local, same as resolve_at). Free functions/
        // globals/macros with no locals still fit here with an empty/absent body.
        struct Scope {
            std::vector<ParamInfo> params;
            const ast::Stmt *body = nullptr;
        };

        // Resolves a bare identifier `name` used at `line` within `scope`, mirroring
        // resolve_at's resolve_base_name lambda exactly: params, then locals declared before
        // this line, then module-scope symbols.
        auto resolve_bare_name(const std::string &name, const size_t line, const Scope &scope,
                                const LocalLookupContext &ctx, const std::string &module_path) -> std::optional<Resolution> {
            for (const auto &p : scope.params) {
                if (p.name == name) {
                    return Resolution{.kind = Resolution::Kind::Param, .name = name, .location = p.location, .type = p.type};
                }
            }
            if (scope.body) {
                if (const auto local = find_local(*scope.body, ctx, name, line)) {
                    return Resolution{.kind = Resolution::Kind::Local, .name = name, .location = local->location, .type = local->type};
                }
            }
            if (const auto sym_it = ctx.sema_module.symbols.find(name); sym_it != ctx.sema_module.symbols.end()) {
                return Resolution{.kind = Resolution::Kind::Symbol, .name = name, .module_path = module_path, .symbol = &sym_it->second};
            }
            return std::nullopt;
        }

        // Walks one function/method body collecting every reference that resolves to `target`.
        void collect_references_in_scope(const Scope &scope, const LocalLookupContext &ctx, const std::string &module_path,
                                          const sema::Program &program, const Resolution &target, std::vector<json> &out) {
            AstVisitor visitor;
            visitor.on_expr = [&](const ast::Expr &expr) {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (const auto res = resolve_bare_name(ident->name, ident->location.line, scope, ctx, module_path)) {
                        if (same_declaration(*res, target)) out.push_back(location_json(ident->location));
                    }
                    return;
                }
                const auto *member_ptr = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr);
                if (!member_ptr) return;
                const auto &member = **member_ptr;

                if (const auto ty_it = ctx.sema_module.expr_types.find(sema::get_expr_key(member.object));
                    ty_it != ctx.sema_module.expr_types.end()) {
                    if (auto res = resolve_member(ty_it->second, member.member, program); res.kind != Resolution::Kind::None) {
                        if (same_declaration(res, target)) out.push_back(location_json(member.location));
                    }
                    return;
                }

                // Module-qualified access (e.g. 'greet.hello') - the object isn't a typed
                // value, so it has no expr_types entry; resolve it as a bare name instead and
                // step into its module if it turns out to be an import alias.
                if (const auto *object_ident = std::get_if<ast::IdentExpr>(&member.object)) {
                    if (const auto object_res = resolve_bare_name(object_ident->name, member.location.line, scope, ctx, module_path);
                        object_res && object_res->kind == Resolution::Kind::Symbol) {
                        const auto container = symbol_to_container(*object_res->symbol);
                        if (container.kind != Container::Kind::None) {
                            if (auto [res, next] = step(container, member.member, program); res.kind != Resolution::Kind::None) {
                                if (same_declaration(res, target)) out.push_back(location_json(member.location));
                            }
                        }
                    }
                }
            };
            walk_stmt(scope.body ? *scope.body : ast::Stmt{ast::ExprStmt{}}, visitor);
        }
    }

    auto handle_references(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                            const size_t line, const size_t column, const bool include_declaration) -> json {
        const auto target = resolve_at(result, module_path, path, line, column);
        if (target.kind == Resolution::Kind::None || target.kind == Resolution::Kind::Builtin) {
            return json::array();
        }

        std::vector<json> out;
        if (include_declaration && target.kind != Resolution::Kind::Symbol) {
            // Kind::Symbol's declaration site is handled per-symbol-kind in the module loop
            // below (it may be a FunctionDecl/VarDecl/TypeDecl/etc.); everything else has its
            // declaration-site location directly on the Resolution already.
            if (auto loc = location_json(target.location); !loc.is_null()) out.push_back(std::move(loc));
        }

        for (const auto &[mod_path, decls] : result.ast_program.modules) {
            const auto sema_mod_it = result.sema_program.modules.find(mod_path);
            if (sema_mod_it == result.sema_program.modules.end()) continue;

            DiagnosticEngine throwaway_diag(*result.source_manager);
            const LocalLookupContext ctx{
                .sema_module = sema_mod_it->second,
                .sema_program = result.sema_program,
                .module_path = mod_path,
                .diag = throwaway_diag,
            };

            for (const auto &decl : decls) {
                if (const auto *fn = std::get_if<ast::FunctionDecl>(&decl)) {
                    std::vector<ParamInfo> params;
                    for (const auto &p : fn->params) params.push_back({p.name, {}, p.location});
                    collect_references_in_scope({.params = std::move(params), .body = &fn->body}, ctx, mod_path,
                                                 result.sema_program, target, out);
                    if (include_declaration && target.kind == Resolution::Kind::Symbol) {
                        if (const auto *fn_sym = std::get_if<sema::FunctionSymbol>(target.symbol);
                            fn_sym && fn_sym->decl == fn) {
                            out.push_back(location_json(fn->location));
                        }
                    }
                } else if (const auto *var = std::get_if<ast::VarDecl>(&decl)) {
                    if (var->init) {
                        collect_references_in_scope({.params = {}, .body = nullptr}, ctx, mod_path, result.sema_program,
                                                     target, out);
                        AstVisitor visitor;
                        visitor.on_expr = [&](const ast::Expr &expr) {
                            if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                                if (const auto res = resolve_bare_name(ident->name, ident->location.line,
                                                                        Scope{}, ctx, mod_path)) {
                                    if (same_declaration(*res, target)) out.push_back(location_json(ident->location));
                                }
                            }
                        };
                        walk_expr(*var->init, visitor);
                    }
                    if (include_declaration && target.kind == Resolution::Kind::Symbol) {
                        if (const auto *global_sym = std::get_if<sema::GlobalSymbol>(target.symbol);
                            global_sym && global_sym->decl == var) {
                            out.push_back(location_json(var->location));
                        }
                    }
                } else if (const auto *macro = std::get_if<ast::MacroDecl>(&decl)) {
                    std::vector<ParamInfo> params;
                    for (const auto &p : macro->params) params.push_back({p.name, {}, p.location});
                    AstVisitor visitor;
                    const Scope scope{.params = std::move(params), .body = nullptr};
                    visitor.on_expr = [&](const ast::Expr &expr) {
                        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                            if (const auto res = resolve_bare_name(ident->name, ident->location.line, scope, ctx, mod_path)) {
                                if (same_declaration(*res, target)) out.push_back(location_json(ident->location));
                            }
                        }
                    };
                    walk_expr(macro->expr_template, visitor);
                    if (include_declaration && target.kind == Resolution::Kind::Symbol) {
                        if (const auto *macro_sym = std::get_if<sema::MacroSymbol>(target.symbol);
                            macro_sym && macro_sym->decl == macro) {
                            out.push_back(location_json(macro->location));
                        }
                    }
                } else if (const auto *impl = std::get_if<ast::ImplDecl>(&decl)) {
                    const auto methods_it = sema_mod_it->second.methods.find(impl->target.name);
                    for (const auto &fn : impl->functions) {
                        const sema::MethodInfo *sym = nullptr;
                        if (methods_it != sema_mod_it->second.methods.end()) {
                            if (const auto mit = methods_it->second.find(fn.name); mit != methods_it->second.end()) sym = &mit->second;
                        }
                        std::vector<ParamInfo> params;
                        params.push_back({"self", sym ? sym->self_type : sema::ResolvedType{}, fn.self_location});
                        for (const auto &p : fn.params) params.push_back({p.name, {}, p.location});
                        collect_references_in_scope({.params = std::move(params), .body = &fn.body}, ctx, mod_path,
                                                     result.sema_program, target, out);
                        if (include_declaration && target.kind == Resolution::Kind::Method && target.method == sym) {
                            out.push_back(location_json(fn.location));
                        }
                    }
                } else if (const auto *trait_impl = std::get_if<ast::TraitImplDecl>(&decl)) {
                    for (const auto &fn : trait_impl->functions) {
                        std::vector<ParamInfo> params;
                        params.push_back({"self", {}, fn.self_location});
                        for (const auto &p : fn.params) params.push_back({p.name, {}, p.location});
                        collect_references_in_scope({.params = std::move(params), .body = &fn.body}, ctx, mod_path,
                                                     result.sema_program, target, out);
                    }
                }
            }
        }

        return out;
    }
}
