#include "common.hpp"

#include "compiler/lexer.hpp"

#include <algorithm>
#include <type_traits>

namespace lsp::handlers {
    namespace {
        // A stepping-stone while walking a dotted chain: either "we're inside
        // a module namespace" (after resolving an import) or "we're on a
        // value of this resolved type" (after resolving a local/global/field).
        struct Container {
            enum class Kind : uint8_t { None, Module, Type };
            Kind kind = Kind::None;
            std::string module_path;
            sema::ResolvedType type;
        };

        struct ParamInfo {
            std::string name;
            sema::ResolvedType type;
            SourceLocation location;
        };

        struct EnclosingFunction {
            std::vector<ParamInfo> params;
            const ast::Stmt *body = nullptr;
        };

        struct LocalInfo {
            SourceLocation location;
            sema::ResolvedType type;
        };

        auto symbol_to_container(const sema::Symbol &symbol) -> Container {
            return std::visit(
                [&]<typename T>(const T &sym) -> Container {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, sema::ImportSymbol>) {
                        return Container{.kind = Container::Kind::Module, .module_path = sym.module_path, .type = {}};
                    } else if constexpr (std::is_same_v<S, sema::GlobalSymbol>) {
                        return Container{.kind = Container::Kind::Type, .module_path = "", .type = sym.type};
                    } else if constexpr (std::is_same_v<S, sema::TypeSymbol>) {
                        if (sym.resolved) {
                            return Container{.kind = Container::Kind::Type, .module_path = "", .type = *sym.resolved};
                        }
                        return {};
                    } else {
                        return {};
                    }
                },
                symbol);
        }

        // Matches `member` against a struct's or union's fields by name.
        // Returns a Resolution with Kind::None if `type` isn't Struct/Union
        // or has no such member. Shared by step()'s Type-container walk and
        // by struct-literal field-designator resolution (`.field = value`),
        // which needs field lookup without the rest of step()'s chaining
        // machinery (enum/method lookup, Container production).
        auto match_struct_or_union_field(const sema::ResolvedType &type, const std::string &member,
                                          const sema::Program &program) -> Resolution {
            if (type.kind == sema::TypeKind::Struct && type.struct_index >= 0 &&
                static_cast<size_t>(type.struct_index) < program.structs.size()) {
                const auto &info = program.structs[type.struct_index];
                for (const auto &field : info.fields) {
                    if (field.name == member) {
                        return Resolution{
                            .kind = Resolution::Kind::StructField,
                            .name = member,
                            .location = field.location,
                            .type = field.type,
                            .struct_field = &field,
                        };
                    }
                }
            } else if (type.kind == sema::TypeKind::Union && type.union_index >= 0 &&
                       static_cast<size_t>(type.union_index) < program.unions.size()) {
                const auto &info = program.unions[type.union_index];
                for (const auto &member_info : info.members) {
                    if (member_info.name == member) {
                        return Resolution{
                            .kind = Resolution::Kind::UnionMember,
                            .name = member,
                            .location = member_info.location,
                            .type = member_info.type,
                            .union_member = &member_info,
                        };
                    }
                }
            }
            return {};
        }

        // EnumFieldInfo/TaggedUnionVariant carry no location of their own - fall back to the
        // enum/union type's own declaration site.
        auto type_decl_location(const sema::ResolvedType &type, const sema::Program &program) -> SourceLocation {
            SourceLocation loc{};
            const auto [mod_path, type_name] = sema::find_type_module_and_name(type, program);
            if (!type_name.empty()) {
                if (const auto mod_it = program.modules.find(mod_path); mod_it != program.modules.end()) {
                    if (const auto sym_it = mod_it->second.symbols.find(type_name); sym_it != mod_it->second.symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&sym_it->second)) {
                            loc = ts->location;
                        }
                    }
                }
            }
            return loc;
        }

        // Matches `member` against an enum's fields or a tagged union's variants by name.
        // Returns a Resolution with Kind::None if `type` isn't Enum/tagged-Union or has no such
        // field/variant. Shared by step()'s Type-container walk (qualified access, e.g.
        // `Module.Type.Field`) and by contextual variant-reference resolution (`.Field`), which
        // needs the lookup without the rest of step()'s chaining machinery.
        auto match_enum_or_variant(const sema::ResolvedType &type, const std::string &member,
                                    const sema::Program &program) -> Resolution {
            if (type.kind == sema::TypeKind::Enum && type.enum_index >= 0 &&
                static_cast<size_t>(type.enum_index) < program.enums.size()) {
                const auto &info = program.enums[type.enum_index];
                for (const auto &field : info.fields) {
                    if (field.name == member) {
                        return Resolution{
                            .kind = Resolution::Kind::EnumField,
                            .name = member,
                            .location = type_decl_location(type, program),
                            .type = type,
                        };
                    }
                }
            } else if (type.kind == sema::TypeKind::Union && type.union_index >= 0 &&
                       static_cast<size_t>(type.union_index) < program.unions.size()) {
                const auto &info = program.unions[type.union_index];
                if (info.is_tagged) {
                    for (const auto &variant : info.variants) {
                        if (variant.name == member) {
                            return Resolution{
                                .kind = Resolution::Kind::Variant,
                                .name = member,
                                .location = type_decl_location(type, program),
                                .type = type,
                            };
                        }
                    }
                }
            }
            return {};
        }

        // Resolves `member` against `type_in`: struct/union field, enum field/tagged-union
        // variant, or method - transparently dereferencing one level of pointer first (so
        // `p.field`/`p.method()` resolve whether `p` is `T` or `*T`). Kind::None if `member`
        // doesn't match anything. Shared by step()'s Type-container walk (which additionally
        // needs to know whether to keep chaining - only struct/union fields do) and by direct
        // AST-node-based resolution (which doesn't need to chain further, e.g. resolving a
        // member access whose receiver is a call/index result rather than an identifier chain).
        auto resolve_member(const sema::ResolvedType &type_in, const std::string &member,
                             const sema::Program &program) -> Resolution {
            auto type = type_in;
            if (type.kind == sema::TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(type.pointee_index);
                if (!pointee) return {};
                type = *pointee;
            }

            if (auto field_res = match_struct_or_union_field(type, member, program);
                field_res.kind != Resolution::Kind::None) {
                return field_res;
            }
            if (auto variant_res = match_enum_or_variant(type, member, program);
                variant_res.kind != Resolution::Kind::None) {
                return variant_res;
            }
            // Struct/union/enum method call, e.g. `hash_map.init(...)`.
            if (const auto *method = sema::find_method(type, member, program)) {
                return Resolution{
                    .kind = Resolution::Kind::Method,
                    .name = member,
                    .location = method->decl ? method->decl->location : SourceLocation{},
                    .method = method,
                };
            }
            return {};
        }

        // One step of a dotted-chain walk: resolves `member` within
        // `container`, returning both the Resolution describing it and the
        // Container to keep chaining from (Kind::None if `member` is a dead
        // end - e.g. a method or a function/macro symbol has no members).
        auto step(const Container &container, const std::string &member, const sema::Program &program)
            -> std::pair<Resolution, Container> {
            if (container.kind == Container::Kind::Module) {
                const auto mod_it = program.modules.find(container.module_path);
                if (mod_it == program.modules.end()) return {};
                const auto sym_it = mod_it->second.symbols.find(member);
                if (sym_it == mod_it->second.symbols.end()) return {};

                Resolution res{
                    .kind = Resolution::Kind::Symbol,
                    .name = member,
                    .module_path = container.module_path,
                    .symbol = &sym_it->second,
                };
                return {res, symbol_to_container(sym_it->second)};
            }

            if (container.kind == Container::Kind::Type) {
                auto res = resolve_member(container.type, member, program);
                if (res.kind == Resolution::Kind::StructField || res.kind == Resolution::Kind::UnionMember) {
                    return {res, Container{.kind = Container::Kind::Type, .module_path = "", .type = res.type}};
                }
                if (res.kind != Resolution::Kind::None) {
                    return {res, {}};
                }
            }

            return {};
        }

        // Bundles the mutable sema state a local-variable-type lookup needs:
        // resolving the declared type annotation (when present) via
        // sema::resolve_declared_type() needs a mutable Program& and a
        // DiagnosticEngine& (harmless to call again here: types are interned,
        // so this cannot produce new errors or diverge from what sema itself
        // already resolved during body checking).
        struct LocalLookupContext {
            const sema::ProgramModule &sema_module;
            sema::Program &sema_program;
            const std::string &module_path;
            DiagnosticEngine &diag;
        };

        // Mirrors sema_check.cpp's own VarDeclStmt handling: when a declared type annotation
        // is present, it - not the initializer's own natural type - is the variable's actual
        // type (`locals[v.name] = LocalBinding{.type = has_declared_ty ? declared_ty : init_ty,
        // ...}`). Without this, hovering a var like `mut p: *T = try alloc(...)` would show
        // `alloc`'s raw return type (e.g. `anyptr`) instead of the declared/coerced-to type.
        auto resolve_var_decl_type(const ast::VarDeclStmt &node, const LocalLookupContext &ctx) -> sema::ResolvedType {
            if (const auto declared = sema::resolve_declared_type(node.type, node.init, ctx.module_path, ctx.sema_program, ctx.diag, node.location)) {
                return *declared;
            }
            if (node.init) {
                if (const auto it = ctx.sema_module.expr_types.find(sema::get_expr_key(*node.init));
                    it != ctx.sema_module.expr_types.end()) {
                    return it->second;
                }
            }
            return {};
        }

        void walk_stmt_for_locals(const ast::Stmt &stmt, const std::string &name, const size_t before_line,
                                   const LocalLookupContext &ctx, std::optional<LocalInfo> &best) {
            std::visit(
                [&]<typename T>(const T &node) {
                    using U = std::decay_t<T>;
                    if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                        for (const auto &s : node->stmts) walk_stmt_for_locals(s, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                        walk_stmt_for_locals(node->then_stmt, name, before_line, ctx, best);
                        if (node->else_stmt) walk_stmt_for_locals(*node->else_stmt, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                        walk_stmt_for_locals(node->body, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                        if (node->location.line <= before_line &&
                            (node->element_name == name || node->index_name == name)) {
                            best = LocalInfo{.location = node->location, .type = {}};
                        }
                        walk_stmt_for_locals(node->body, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                        for (const auto &arm : node->arms) walk_stmt_for_locals(arm.body, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                        walk_stmt_for_locals(node->body, name, before_line, ctx, best);
                    } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                        if (node.name == name && node.location.line <= before_line) {
                            best = LocalInfo{.location = node.location, .type = resolve_var_decl_type(node, ctx)};
                        }
                    } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                        if (node.location.line <= before_line &&
                            std::ranges::find(node.names, name) != node.names.end()) {
                            best = LocalInfo{.location = node.location, .type = {}};
                        }
                    }
                    // ExprStmt, ContinueStmt, BreakStmt, ReturnStmt,
                    // ReturnErrStmt, ReturnOkStmt declare no names.
                },
                stmt);
        }

        auto find_local(const ast::Stmt &body, const LocalLookupContext &ctx, const std::string &name,
                         const size_t before_line) -> std::optional<LocalInfo> {
            std::optional<LocalInfo> best;
            walk_stmt_for_locals(body, name, before_line, ctx, best);
            return best;
        }

        auto find_enclosing_function(const ast::Module &module, const sema::ProgramModule &sema_module, const size_t line)
            -> EnclosingFunction {
            EnclosingFunction best;
            size_t best_line = 0;

            auto consider = [&](const size_t decl_line, std::vector<ParamInfo> params, const ast::Stmt *body) {
                if (decl_line <= line && decl_line >= best_line) {
                    best_line = decl_line;
                    best = EnclosingFunction{.params = std::move(params), .body = body};
                }
            };

            for (const auto &decl : module) {
                if (const auto *fn = std::get_if<ast::FunctionDecl>(&decl)) {
                    const auto sym_it = sema_module.symbols.find(fn->name);
                    const auto *sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::FunctionSymbol>(&sym_it->second) : nullptr;
                    std::vector<ParamInfo> params;
                    for (size_t i = 0; i < fn->params.size(); ++i) {
                        sema::ResolvedType type{};
                        if (sym && i < sym->params.size()) type = sym->params[i];
                        params.push_back({fn->params[i].name, type, fn->params[i].location});
                    }
                    consider(fn->location.line, std::move(params), &fn->body);
                } else if (const auto *ext = std::get_if<ast::ExtFunctionDecl>(&decl)) {
                    const auto sym_it = sema_module.symbols.find(ext->name);
                    const auto *sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::ExtFunctionSymbol>(&sym_it->second) : nullptr;
                    std::vector<ParamInfo> params;
                    for (size_t i = 0; i < ext->params.size(); ++i) {
                        sema::ResolvedType type{};
                        if (sym && i < sym->params.size()) type = sym->params[i];
                        params.push_back({ext->params[i].name, type, ext->params[i].location});
                    }
                    consider(ext->location.line, std::move(params), nullptr);
                } else if (const auto *macro = std::get_if<ast::MacroDecl>(&decl)) {
                    const auto sym_it = sema_module.symbols.find(macro->name);
                    const auto *sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::MacroSymbol>(&sym_it->second) : nullptr;
                    std::vector<ParamInfo> params;
                    for (size_t i = 0; i < macro->params.size(); ++i) {
                        sema::ResolvedType type{};
                        if (sym && i < sym->params.size()) type = sym->params[i];
                        params.push_back({macro->params[i].name, type, macro->params[i].location});
                    }
                    consider(macro->location.line, std::move(params), nullptr);
                } else if (const auto *impl = std::get_if<ast::ImplDecl>(&decl)) {
                    const auto methods_it = sema_module.methods.find(impl->target.name);
                    for (const auto &fn : impl->functions) {
                        const sema::MethodInfo *sym = nullptr;
                        if (methods_it != sema_module.methods.end()) {
                            if (const auto mit = methods_it->second.find(fn.name); mit != methods_it->second.end()) {
                                sym = &mit->second;
                            }
                        }
                        std::vector<ParamInfo> params;
                        if (sym) params.push_back({"self", sym->self_type, fn.self_location});
                        for (size_t i = 0; i < fn.params.size(); ++i) {
                            sema::ResolvedType type{};
                            if (sym && i < sym->param_types.size()) type = sym->param_types[i];
                            params.push_back({fn.params[i].name, type, fn.params[i].location});
                        }
                        consider(fn.location.line, std::move(params), &fn.body);
                    }
                }
            }

            return best;
        }

        // If tokens[index] is a struct-literal field designator - preceded by
        // '.' whose own predecessor, found by walking backward over balanced
        // ()/[]/{} pairs and skipping top-level commas, is an unmatched '{' -
        // returns that '{' token's location (which is exactly how
        // ast::StructExpr::location is captured by the parser, see
        // parse_braced_initializer). Returns nullopt otherwise: dotted chains
        // are already handled by chain_prefix before this is ever called, so
        // by construction tokens[index-2] is never an Identifier here - the
        // only remaining shapes are field designators (preceded by '{'/',')
        // or something else entirely (call args, index/array contexts,
        // preceded by ')'/']'/other punctuation), which this correctly
        // rejects by requiring the immediate predecessor to be '{' or ','.
        auto enclosing_struct_literal_brace(const std::vector<Token> &tokens, const size_t index) -> std::optional<SourceLocation> {
            if (index < 2 || tokens[index - 1].kind != TokenKind::Dot) return std::nullopt;
            const auto prev_kind = tokens[index - 2].kind;
            if (prev_kind != TokenKind::LBrace && prev_kind != TokenKind::Comma) return std::nullopt;

            int depth = 0;
            for (size_t j = index - 2;; --j) {
                switch (tokens[j].kind) {
                    case TokenKind::RBrace:
                    case TokenKind::RParen:
                    case TokenKind::RBracket:
                        ++depth;
                        break;
                    case TokenKind::LBrace:
                        if (depth == 0) return tokens[j].location;
                        --depth;
                        break;
                    case TokenKind::LParen:
                    case TokenKind::LBracket:
                        if (depth == 0) return std::nullopt;
                        --depth;
                        break;
                    default:
                        break;
                }
                if (j == 0) break;
            }
            return std::nullopt;
        }

        auto location_matches(const SourceLocation &a, const SourceLocation &b) -> bool {
            return a.line == b.line && a.column == b.column;
        }

        auto find_expr_by_location(const ast::Stmt &stmt, const SourceLocation &target) -> const ast::Expr *;

        // Recursively searches `expr` (and everything nested inside it) for the exact
        // ast::Expr slot whose location equals `target`. Shapes matched: a
        // BracedInitializerExpr whose alternative is StructExpr (struct-literal field
        // designator resolution, `.field = value`); a DotIdentExpr/TaggedVariantExpr
        // (contextual variant-reference resolution, `.Variant` / `.Variant{...}`); and a
        // MemberExpr (member-access resolution, `expr.field`) - the latter needs its own
        // per-node location (set fresh per postfix-chain iteration in parse_postfix, see
        // ast.cpp) rather than the token-based chain_prefix reconstruction below, since it
        // works even when the receiver isn't a plain identifier chain (`f().field`,
        // `a[0].field`). Returns nullptr if none found. Source locations are unique per
        // position, so at most one node in the whole tree can match - no separate
        // innermost/outermost disambiguation is needed beyond exact equality (callers already
        // narrow `target` to the specific token they care about before calling this).
        //
        // Mirrors the dispatch shape of check_expr/check_stmt in
        // sema_check.cpp (which already exhaustively enumerate every
        // Expr/Stmt alternative and their nested Expr/Stmt fields), just
        // recursing instead of type-checking.
        //
        // Known gap: TaggedVariantExpr::payload is a bare
        // std::optional<StructExpr>, not wrapped in an ast::Expr, so it has
        // no corresponding get_expr_key() slot - field designators inside a
        // tagged-union-variant literal payload (`.Foo{ .x = 1 }`) are out of
        // scope for this helper.
        auto find_expr_by_location(const ast::Expr &expr, const SourceLocation &target) -> const ast::Expr * {
            return std::visit(
                [&]<typename T>(const T &node) -> const ast::Expr * {
                    using U = std::decay_t<T>;
                    if constexpr (std::is_same_v<U, std::unique_ptr<ast::UnaryExpr>>) {
                        return find_expr_by_location(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BinaryExpr>>) {
                        if (const auto *r = find_expr_by_location(node->lhs, target)) return r;
                        return find_expr_by_location(node->rhs, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TernaryExpr>>) {
                        if (const auto *r = find_expr_by_location(node->condition, target)) return r;
                        if (const auto *r = find_expr_by_location(node->then_expr, target)) return r;
                        return find_expr_by_location(node->else_expr, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AssignExpr>>) {
                        if (const auto *r = find_expr_by_location(node->target, target)) return r;
                        return find_expr_by_location(node->value, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CallExpr>>) {
                        if (const auto *r = find_expr_by_location(node->callee, target)) return r;
                        for (const auto &arg : node->args) {
                            if (const auto *r = find_expr_by_location(arg, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IncrDecrExpr>>) {
                        return find_expr_by_location(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SizeOfExpr>>) {
                        return find_expr_by_location(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::LenExpr>>) {
                        return find_expr_by_location(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CastExpr>>) {
                        if (const auto *r = find_expr_by_location(node->value, target)) return r;
                        if (node->len_expr) return find_expr_by_location(*node->len_expr, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IndexExpr>>) {
                        if (const auto *r = find_expr_by_location(node->operand, target)) return r;
                        return find_expr_by_location(node->index, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SliceExpr>>) {
                        if (const auto *r = find_expr_by_location(node->operand, target)) return r;
                        if (const auto *r = find_expr_by_location(node->start, target)) return r;
                        return find_expr_by_location(node->end, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MemberExpr>>) {
                        if (location_matches(node->location, target)) return &expr;
                        return find_expr_by_location(node->object, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MatchExpr>>) {
                        if (const auto *r = find_expr_by_location(node->operand, target)) return r;
                        for (const auto &arm : node->arms) {
                            if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                                if (lit->expr) {
                                    if (const auto *r = find_expr_by_location(*lit->expr, target)) return r;
                                }
                            }
                            if (const auto *r = find_expr_by_location(arm.value, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BracedInitializerExpr>>) {
                        return std::visit(
                            [&]<typename V>(const V &alt) -> const ast::Expr * {
                                using W = std::decay_t<V>;
                                if constexpr (std::is_same_v<W, ast::StructExpr>) {
                                    for (const auto &field : alt.fields) {
                                        if (const auto *r = find_expr_by_location(field.expr, target)) return r;
                                    }
                                    if (location_matches(alt.location, target)) return &expr;
                                    return nullptr;
                                } else if constexpr (std::is_same_v<W, ast::ArrayExpr>) {
                                    for (const auto &v : alt.values) {
                                        if (const auto *r = find_expr_by_location(v, target)) return r;
                                    }
                                    return nullptr;
                                } else {
                                    return nullptr; // EmptyExpr
                                }
                            },
                            *node);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TryExpr>>) {
                        return find_expr_by_location(node->call, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::RangeExpr>>) {
                        if (node->lower) {
                            if (const auto *r = find_expr_by_location(*node->lower, target)) return r;
                        }
                        return find_expr_by_location(node->upper, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SpreadExpr>>) {
                        return find_expr_by_location(node->operand, target);
                    } else if constexpr (std::is_same_v<U, ast::DotIdentExpr>) {
                        return location_matches(node.location, target) ? &expr : nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TaggedVariantExpr>>) {
                        return location_matches(node->location, target) ? &expr : nullptr;
                    } else {
                        // Literals, IdentExpr, ImportExpr, IotaExpr,
                        // DefaultExpr, UndefinedExpr - none contain a nested Expr.
                        return nullptr;
                    }
                },
                expr);
        }

        auto find_expr_by_location(const ast::Stmt &stmt, const SourceLocation &target) -> const ast::Expr * {
            return std::visit(
                [&]<typename T>(const T &node) -> const ast::Expr * {
                    using U = std::decay_t<T>;
                    if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                        for (const auto &s : node->stmts) {
                            if (const auto *r = find_expr_by_location(s, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                        if (const auto *r = find_expr_by_location(node->condition, target)) return r;
                        if (const auto *r = find_expr_by_location(node->then_stmt, target)) return r;
                        if (node->else_stmt) return find_expr_by_location(*node->else_stmt, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                        if (const auto *r = find_expr_by_location(node->condition, target)) return r;
                        return find_expr_by_location(node->body, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                        if (const auto *r = find_expr_by_location(node->iterable, target)) return r;
                        return find_expr_by_location(node->body, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                        if (const auto *r = find_expr_by_location(node->operand, target)) return r;
                        for (const auto &arm : node->arms) {
                            if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                                if (lit->expr) {
                                    if (const auto *r = find_expr_by_location(*lit->expr, target)) return r;
                                }
                            }
                            if (const auto *r = find_expr_by_location(arm.body, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                        return find_expr_by_location(node->body, target);
                    } else if constexpr (std::is_same_v<U, ast::ExprStmt>) {
                        return find_expr_by_location(node.expr, target);
                    } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                        if (node.init) return find_expr_by_location(*node.init, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                        return find_expr_by_location(node.init, target);
                    } else if constexpr (std::is_same_v<U, ast::ReturnStmt>) {
                        for (const auto &e : node.return_values) {
                            if (const auto *r = find_expr_by_location(e, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, ast::ReturnErrStmt>) {
                        return find_expr_by_location(node.error_value, target);
                    } else if constexpr (std::is_same_v<U, ast::ReturnOkStmt>) {
                        for (const auto &e : node.return_values) {
                            if (const auto *r = find_expr_by_location(e, target)) return r;
                        }
                        return nullptr;
                    } else {
                        // ContinueStmt, BreakStmt declare no expressions.
                        return nullptr;
                    }
                },
                stmt);
        }
    }

    auto token_at(const std::vector<Token> &tokens, const size_t line, const size_t column) -> std::optional<size_t> {
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto &t = tokens[i];
            if (t.location.line != line) continue;
            const auto start = t.location.column;
            const auto end = start + t.lexeme.size();
            if (column >= start && column < end) {
                return i;
            }
        }
        return std::nullopt;
    }

    auto chain_prefix(const std::vector<Token> &tokens, const size_t index) -> std::vector<std::string> {
        std::vector<std::string> prefix;
        size_t i = index;
        while (i >= 2 && tokens[i - 1].kind == TokenKind::Dot && tokens[i - 2].kind == TokenKind::Identifier) {
            prefix.push_back(tokens[i - 2].lexeme);
            i -= 2;
        }
        std::ranges::reverse(prefix);
        return prefix;
    }

    auto resolve_at(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                     const size_t line, const size_t column) -> Resolution {
        DiagnosticEngine throwaway_diag(*result.source_manager);
        const auto source_file = result.source_manager->load(path, throwaway_diag);
        if (source_file.text.empty()) return {};

        auto tokens = lexer::tokenize(source_file.text, source_file.filename, throwaway_diag);
        const auto idx_opt = token_at(tokens, line, column);
        if (!idx_opt) return {};

        // sizeof/len are dedicated keyword tokens (TokenKind::KwSizeOf/KwLen), not
        // TokenKind::Identifier, so they'd otherwise be rejected by the guard below - resolve
        // them directly as a synthetic builtin, always usize (see SizeOfExpr/LenExpr handling
        // in sema_check.cpp).
        if (tokens[*idx_opt].kind == TokenKind::KwSizeOf || tokens[*idx_opt].kind == TokenKind::KwLen) {
            return Resolution{
                .kind = Resolution::Kind::Builtin,
                .name = tokens[*idx_opt].kind == TokenKind::KwSizeOf ? "sizeof" : "len",
                .type = sema::ResolvedType{.kind = sema::TypeKind::USize},
            };
        }
        if (tokens[*idx_opt].kind != TokenKind::Identifier) return {};
        const auto idx = *idx_opt;

        const auto mod_it = result.ast_program.modules.find(module_path);
        const auto sema_mod_it = result.sema_program.modules.find(module_path);
        if (mod_it == result.ast_program.modules.end() || sema_mod_it == result.sema_program.modules.end()) {
            return {};
        }

        const LocalLookupContext ctx{
            .sema_module = sema_mod_it->second,
            .sema_program = result.sema_program,
            .module_path = module_path,
            .diag = throwaway_diag,
        };

        const auto enclosing = find_enclosing_function(mod_it->second, sema_mod_it->second, line);

        auto resolve_base_name = [&](const std::string &name) -> std::optional<Resolution> {
            for (const auto &p : enclosing.params) {
                if (p.name == name) {
                    return Resolution{.kind = Resolution::Kind::Param, .name = name, .location = p.location, .type = p.type};
                }
            }
            if (enclosing.body) {
                if (const auto local = find_local(*enclosing.body, ctx, name, line)) {
                    return Resolution{.kind = Resolution::Kind::Local, .name = name, .location = local->location, .type = local->type};
                }
            }
            if (const auto sym_it = sema_mod_it->second.symbols.find(name); sym_it != sema_mod_it->second.symbols.end()) {
                return Resolution{
                    .kind = Resolution::Kind::Symbol,
                    .name = name,
                    .module_path = module_path,
                    .symbol = &sym_it->second,
                };
            }
            return std::nullopt;
        };

        const auto prefix = chain_prefix(tokens, idx);

        // Resolve '.name' positions directly off the AST's own node before falling back to
        // the token-based chain_prefix walk below. chain_prefix can only reconstruct pure
        // identifier.identifier chains; it comes back empty - or worse, a bogus partial chain
        // - as soon as a call/index appears anywhere in the receiver (`f().field`,
        // `a[0].field`, `a.f().field`). Sema has already fully resolved the receiver's type
        // regardless of how complex it is, so looking that up directly handles every case
        // uniformly: MemberExpr (member access) and DotIdentExpr/TaggedVariantExpr
        // (contextual variant reference, `.Variant`). Falls through to chain_prefix for
        // module-qualified chains (`greet.hello`), which this doesn't handle since a module
        // alias isn't itself a typed value in expr_types.
        if (enclosing.body && idx >= 1 && tokens[idx - 1].kind == TokenKind::Dot) {
            if (const auto *found = find_expr_by_location(*enclosing.body, tokens[idx - 1].location)) {
                if (const auto *member_expr = std::get_if<std::unique_ptr<ast::MemberExpr>>(found)) {
                    if (const auto ty_it = ctx.sema_module.expr_types.find(sema::get_expr_key((*member_expr)->object));
                        ty_it != ctx.sema_module.expr_types.end()) {
                        if (auto res = resolve_member(ty_it->second, tokens[idx].lexeme, result.sema_program);
                            res.kind != Resolution::Kind::None) {
                            return res;
                        }
                    }
                } else if (const auto ty_it = ctx.sema_module.expr_types.find(sema::get_expr_key(*found));
                           ty_it != ctx.sema_module.expr_types.end()) {
                    if (auto res = match_enum_or_variant(ty_it->second, tokens[idx].lexeme, result.sema_program);
                        res.kind != Resolution::Kind::None) {
                        return res;
                    }
                }
            }
        }

        if (prefix.empty()) {
            // Struct-literal field designator (`.field = value`), e.g. inside
            // `{ .name = name, .member = member_slot }` - resolve against the
            // literal's contextual struct type before falling back to plain
            // identifier lookup, so a same-named local/param in scope can't
            // silently hijack the answer.
            if (enclosing.body) {
                if (const auto brace_loc = enclosing_struct_literal_brace(tokens, idx)) {
                    if (const auto *literal_expr = find_expr_by_location(*enclosing.body, *brace_loc)) {
                        if (const auto ty_it = ctx.sema_module.expr_types.find(sema::get_expr_key(*literal_expr));
                            ty_it != ctx.sema_module.expr_types.end()) {
                            if (auto field_res = match_struct_or_union_field(ty_it->second, tokens[idx].lexeme, result.sema_program);
                                field_res.kind != Resolution::Kind::None) {
                                return field_res;
                            }
                        }
                    }
                }
            }
            return resolve_base_name(tokens[idx].lexeme).value_or(Resolution{});
        }

        const auto base = resolve_base_name(prefix[0]);
        if (!base) return {};

        Container container = base->kind == Resolution::Kind::Symbol
                                   ? symbol_to_container(*base->symbol)
                                   : Container{.kind = Container::Kind::Type, .module_path = "", .type = base->type};

        for (size_t i = 1; i < prefix.size(); ++i) {
            auto [res, next] = step(container, prefix[i], result.sema_program);
            if (next.kind == Container::Kind::None) return {};
            container = next;
        }

        auto [final_res, unused] = step(container, tokens[idx].lexeme, result.sema_program);
        return final_res;
    }
}
