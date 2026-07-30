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

        // Which enum an EnumField resolution ultimately belongs to.
        //
        // A bitset flag resolves to Kind::EnumField carrying the BITSET as its type
        // (enum_index == -1, bitset_index set), but 'bitset(Stream_Mode, u16)' derives its
        // flags FROM Stream_Mode -- '.Close' in a '{.Close}' literal and 'Close' in the enum
        // declaration are one declaration, and references must report them together.
        // Comparing bitset_index directly, as this used to, split them apart.
        //
        // Normalizing to the member enum keeps the case that guard was protecting: two
        // bitsets over DIFFERENT enums still have different identities, and two bitsets over
        // the SAME enum correctly agree, which comparing bitset_index got wrong in the other
        // direction.
        auto enum_identity_index(const Resolution &r, const sema::Program &program) -> int {
            if (r.type.kind == sema::TypeKind::Bitset) {
                const auto *info = program.bitset_at(r.type.bitset_index);
                return info ? info->member_enum_type.enum_index : -1;
            }
            return r.type.enum_index;
        }

        // Whether a freshly-resolved reference `a` names the same declaration as `b`.
        // Struct/union/method info is compared by pointer identity - stable for the lifetime
        // of one ProgramResult, since nothing mutates program.structs/unions/methods after
        // analysis finishes. Local/param declarations have no comparable pointer, so identity
        // is their own unique declaration-site SourceLocation instead.
        auto same_declaration(const Resolution &a, const Resolution &b, const sema::Program &program) -> bool {
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
                // A trait-handle dispatch resolves to a TraitMethodInfo with no MethodInfo, so
                // comparing 'method' alone would call two different trait methods identical
                // (both null).
                return a.method == b.method && a.trait_method == b.trait_method;
            case Resolution::Kind::EnumField:
            case Resolution::Kind::Variant:
                // Deliberately NOT comparing type.kind: the same enum field is reachable as
                // Enum through its own declaration and as Bitset through a flag literal.
                // enum_identity_index above collapses those two spellings onto one identity.
                return a.name == b.name && a.type.union_index == b.type.union_index &&
                       enum_identity_index(a, program) == enum_identity_index(b, program);
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
                        if (same_declaration(*res, target, program)) out.push_back(location_json(ident->location));
                    }
                    return;
                }
                // Bare '.Variant' / '.Flag'. This is the dominant spelling in real code --
                // every enum, tagged-union and bitset example in the repo uses it -- and the
                // walker did not look at it at all, so "Find All References" on an enum field
                // or a variant found only the rare fully-qualified 'Type.Variant' usages and
                // typically reported zero results for a symbol used everywhere.
                //
                // The contextual type is recorded on the expression itself by sema, so the same
                // match_enum_or_variant path the qualified form uses applies directly. That
                // also means bitset flags are covered as soon as LSPH-2 taught that function
                // about TypeKind::Bitset.
                if (const auto *dot = std::get_if<ast::DotIdentExpr>(&expr)) {
                    if (const auto ty_it = ctx.sema_module.exprs.expr_types.find(sema::get_expr_key(expr));
                        ty_it != ctx.sema_module.exprs.expr_types.end()) {
                        if (auto res = match_enum_or_variant(ty_it->second, dot->name, program);
                            res.kind != Resolution::Kind::None && same_declaration(res, target, program)) {
                            out.push_back(location_json(dot->location));
                        }
                    }
                    return;
                }

                // '{.Close, .Flush}' -- a bitset literal. Its flags are not expression nodes
                // (BitsetExpr stores names, not Exprs), so they carry their own locations and
                // are matched by name against the literal's own bitset type.
                if (const auto *braced_ptr = std::get_if<std::unique_ptr<ast::BracedInitializerExpr>>(&expr)) {
                    const auto *bitset = std::get_if<ast::BitsetExpr>(braced_ptr->get());
                    if (!bitset) return;
                    const auto ty_it = ctx.sema_module.exprs.expr_types.find(sema::get_expr_key(expr));
                    if (ty_it == ctx.sema_module.exprs.expr_types.end()) return;
                    for (const auto &member : bitset->members) {
                        if (auto res = match_enum_or_variant(ty_it->second, member.name, program);
                            res.kind != Resolution::Kind::None && same_declaration(res, target, program)) {
                            out.push_back(location_json(member.location));
                        }
                    }
                    return;
                }

                // '.Variant{...}' / '.Variant(...)' -- the payload-carrying spelling of the
                // same thing.
                if (const auto *tagged_ptr = std::get_if<std::unique_ptr<ast::TaggedVariantExpr>>(&expr)) {
                    const auto &tagged = **tagged_ptr;
                    if (const auto ty_it = ctx.sema_module.exprs.expr_types.find(sema::get_expr_key(expr));
                        ty_it != ctx.sema_module.exprs.expr_types.end()) {
                        if (auto res = match_enum_or_variant(ty_it->second, tagged.variant_name, program);
                            res.kind != Resolution::Kind::None && same_declaration(res, target, program)) {
                            out.push_back(location_json(tagged.location));
                        }
                    }
                    return;
                }

                const auto *member_ptr = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr);
                if (!member_ptr) return;
                const auto &member = **member_ptr;

                if (const auto ty_it = ctx.sema_module.exprs.expr_types.find(sema::get_expr_key(member.object));
                    ty_it != ctx.sema_module.exprs.expr_types.end()) {
                    if (auto res = resolve_member(ty_it->second, member.member, program); res.kind != Resolution::Kind::None) {
                        if (same_declaration(res, target, program)) out.push_back(location_json(member.location));
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
                                if (same_declaration(res, target, program)) out.push_back(location_json(member.location));
                            }
                        }
                    }
                }
            };

            // Match/switch arm patterns ('.Variant:', '.Variant(&v):'). Patterns are not
            // expressions, so on_expr never sees them -- and arms are where variants are most
            // used, so without this a references request on a variant found close to nothing.
            visitor.on_pattern = [&](const ast::MatchExpr::VariantPattern &pattern, const ast::Expr &operand) {
                const auto key = sema::get_expr_key(operand);
                const auto ty_it = ctx.sema_module.exprs.expr_types.find(key);
                if (ty_it == ctx.sema_module.exprs.expr_types.end()) return;

                // 'match err { ... }' dispatches on the INNER representation, not on the
                // Ok/Failed wrapper the operand's recorded type describes -- so the arms name
                // variants of the effective type. sema records that rewrite; use it when
                // present, exactly as codegen does.
                auto operand_type = ty_it->second;
                if (const auto unwrap_it = ctx.sema_module.exprs.expr_error_match_unwrap.find(key);
                    unwrap_it != ctx.sema_module.exprs.expr_error_match_unwrap.end()) {
                    operand_type = unwrap_it->second.effective_type;
                }

                if (auto res = match_enum_or_variant(operand_type, pattern.name, program);
                    res.kind != Resolution::Kind::None && same_declaration(res, target, program)) {
                    out.push_back(location_json(pattern.name_location));
                }
            };

            // Was 'walk_stmt(scope.body ? *scope.body : ast::Stmt{ast::ExprStmt{}}, visitor)',
            // which does not compile: the ternary's operands are a const lvalue reference and a
            // temporary, so it materializes a copy of ast::Stmt -- a variant of unique_ptrs,
            // whose copy constructor is deleted. It went unnoticed because this file was never
            // in the build.
            if (scope.body) {
                walk_stmt(*scope.body, visitor);
            }
        }
    }

    auto handle_references(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                            const size_t line, const size_t column, const bool include_declaration) -> json {
        const auto target = resolve_at(result, module_path, path, line, column);
        // AsmRegister joins None/Builtin here (LSPH-11): it is synthetic, has no declaration
        // site, and same_declaration's default arm rejects it anyway -- so without this guard
        // the request walked every module in the import closure doing real resolution work
        // before returning the empty array it was always going to return.
        if (target.kind == Resolution::Kind::None || target.kind == Resolution::Kind::Builtin ||
            target.kind == Resolution::Kind::AsmRegister) {
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
                                    if (same_declaration(*res, target, result.sema_program)) out.push_back(location_json(ident->location));
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
                                if (same_declaration(*res, target, result.sema_program)) out.push_back(location_json(ident->location));
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
                    // Look the method's own MethodInfo up so 'self' gets its real type and, with
                    // includeDeclaration, the 'fn' line itself is reported -- both of which the
                    // ImplDecl branch above already did. Without it, "Find All References" on a
                    // trait-impl method never included its declaration.
                    const sema::TraitImplInfo *impl_info = nullptr;
                    if (const auto it = result.sema_program.trait_impls_by_type.find({mod_path, trait_impl->type_name.name});
                        it != result.sema_program.trait_impls_by_type.end()) {
                        for (const auto &candidate : it->second) {
                            if (candidate.trait_name == trait_impl->trait_name.name) {
                                impl_info = &candidate;
                                break;
                            }
                        }
                    }

                    for (const auto &fn : trait_impl->functions) {
                        const sema::MethodInfo *sym = nullptr;
                        if (impl_info) {
                            if (const auto mit = impl_info->methods.find(fn.name); mit != impl_info->methods.end()) {
                                sym = &mit->second;
                            }
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
                }
            }
        }

        return out;
    }
}
