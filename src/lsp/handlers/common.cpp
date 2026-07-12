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
                auto type = container.type;
                if (type.kind == sema::TypeKind::Pointer) {
                    const auto *pointee = program.pointee_at(type.pointee_index);
                    if (!pointee) return {};
                    type = *pointee;
                }

                if (auto field_res = match_struct_or_union_field(type, member, program);
                    field_res.kind != Resolution::Kind::None) {
                    return {field_res, Container{.kind = Container::Kind::Type, .module_path = "", .type = field_res.type}};
                }

                if (type.kind == sema::TypeKind::Enum && type.enum_index >= 0 &&
                    static_cast<size_t>(type.enum_index) < program.enums.size()) {
                    const auto &info = program.enums[type.enum_index];
                    for (const auto &field : info.fields) {
                        if (field.name == member) {
                            // EnumFieldInfo carries no location - fall back to
                            // the enum type's own declaration site.
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
                            Resolution res{.kind = Resolution::Kind::EnumField, .name = member, .location = loc, .type = type};
                            return {res, {}};
                        }
                    }
                }

                // Struct/union/enum method call, e.g. `hash_map.init(...)`.
                if (const auto *method = sema::find_method(type, member, program)) {
                    Resolution res{
                        .kind = Resolution::Kind::Method,
                        .name = member,
                        .location = method->decl ? method->decl->location : SourceLocation{},
                        .method = method,
                    };
                    return {res, {}};
                }
            }

            return {};
        }

        // Bundles the mutable sema state a local-variable-type lookup needs:
        // most locals have their type read straight out of expr_types (the
        // already-computed type of their init expression), but a `mut x: T`
        // with no initializer has no init expr to look up - for that case we
        // fall back to resolving the declared type annotation directly via
        // sema::resolve_type(), which needs a mutable Program& and a
        // DiagnosticEngine& (harmless to call again here: types are interned,
        // so this cannot produce new errors or diverge from what sema itself
        // already resolved during body checking).
        struct LocalLookupContext {
            const sema::ProgramModule &sema_module;
            sema::Program &sema_program;
            const std::string &module_path;
            DiagnosticEngine &diag;
        };

        auto resolve_var_decl_type(const ast::VarDeclStmt &node, const LocalLookupContext &ctx) -> sema::ResolvedType {
            if (node.init) {
                if (const auto it = ctx.sema_module.expr_types.find(sema::get_expr_key(*node.init));
                    it != ctx.sema_module.expr_types.end()) {
                    return it->second;
                }
            }
            if (node.type) {
                return sema::resolve_type(*node.type, ctx.module_path, ctx.sema_program, ctx.diag);
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

        auto find_struct_literal_expr(const ast::Stmt &stmt, const SourceLocation &target) -> const ast::Expr *;

        // Recursively searches `expr` (and everything nested inside it) for
        // the exact ast::Expr slot holding a BracedInitializerExpr whose
        // alternative is StructExpr and whose StructExpr::location equals
        // `target`. Returns nullptr if none found. Source locations are
        // unique per position, so at most one node in the whole tree can
        // match - no separate innermost/outermost disambiguation is needed
        // beyond exact equality (enclosing_struct_literal_brace already found
        // the innermost enclosing '{' via token-stream scanning).
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
        auto find_struct_literal_expr(const ast::Expr &expr, const SourceLocation &target) -> const ast::Expr * {
            return std::visit(
                [&]<typename T>(const T &node) -> const ast::Expr * {
                    using U = std::decay_t<T>;
                    if constexpr (std::is_same_v<U, std::unique_ptr<ast::UnaryExpr>>) {
                        return find_struct_literal_expr(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BinaryExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->lhs, target)) return r;
                        return find_struct_literal_expr(node->rhs, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TernaryExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->condition, target)) return r;
                        if (const auto *r = find_struct_literal_expr(node->then_expr, target)) return r;
                        return find_struct_literal_expr(node->else_expr, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AssignExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->target, target)) return r;
                        return find_struct_literal_expr(node->value, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CallExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->callee, target)) return r;
                        for (const auto &arg : node->args) {
                            if (const auto *r = find_struct_literal_expr(arg, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IncrDecrExpr>>) {
                        return find_struct_literal_expr(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SizeOfExpr>>) {
                        return find_struct_literal_expr(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::LenExpr>>) {
                        return find_struct_literal_expr(node->operand, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CastExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->value, target)) return r;
                        if (node->len_expr) return find_struct_literal_expr(*node->len_expr, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IndexExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->operand, target)) return r;
                        return find_struct_literal_expr(node->index, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SliceExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->operand, target)) return r;
                        if (const auto *r = find_struct_literal_expr(node->start, target)) return r;
                        return find_struct_literal_expr(node->end, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MemberExpr>>) {
                        return find_struct_literal_expr(node->object, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MatchExpr>>) {
                        if (const auto *r = find_struct_literal_expr(node->operand, target)) return r;
                        for (const auto &arm : node->arms) {
                            if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                                if (lit->expr) {
                                    if (const auto *r = find_struct_literal_expr(*lit->expr, target)) return r;
                                }
                            }
                            if (const auto *r = find_struct_literal_expr(arm.value, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BracedInitializerExpr>>) {
                        return std::visit(
                            [&]<typename V>(const V &alt) -> const ast::Expr * {
                                using W = std::decay_t<V>;
                                if constexpr (std::is_same_v<W, ast::StructExpr>) {
                                    for (const auto &field : alt.fields) {
                                        if (const auto *r = find_struct_literal_expr(field.expr, target)) return r;
                                    }
                                    if (location_matches(alt.location, target)) return &expr;
                                    return nullptr;
                                } else if constexpr (std::is_same_v<W, ast::ArrayExpr>) {
                                    for (const auto &v : alt.values) {
                                        if (const auto *r = find_struct_literal_expr(v, target)) return r;
                                    }
                                    return nullptr;
                                } else {
                                    return nullptr; // EmptyExpr
                                }
                            },
                            *node);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TryExpr>>) {
                        return find_struct_literal_expr(node->call, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::RangeExpr>>) {
                        if (node->lower) {
                            if (const auto *r = find_struct_literal_expr(*node->lower, target)) return r;
                        }
                        return find_struct_literal_expr(node->upper, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SpreadExpr>>) {
                        return find_struct_literal_expr(node->operand, target);
                    } else {
                        // Literals, IdentExpr, ImportExpr, IotaExpr, DotIdentExpr,
                        // TaggedVariantExpr (payload out of scope, see above),
                        // DefaultExpr, UndefinedExpr - none contain a nested Expr.
                        return nullptr;
                    }
                },
                expr);
        }

        auto find_struct_literal_expr(const ast::Stmt &stmt, const SourceLocation &target) -> const ast::Expr * {
            return std::visit(
                [&]<typename T>(const T &node) -> const ast::Expr * {
                    using U = std::decay_t<T>;
                    if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                        for (const auto &s : node->stmts) {
                            if (const auto *r = find_struct_literal_expr(s, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                        if (const auto *r = find_struct_literal_expr(node->condition, target)) return r;
                        if (const auto *r = find_struct_literal_expr(node->then_stmt, target)) return r;
                        if (node->else_stmt) return find_struct_literal_expr(*node->else_stmt, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                        if (const auto *r = find_struct_literal_expr(node->condition, target)) return r;
                        return find_struct_literal_expr(node->body, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                        if (const auto *r = find_struct_literal_expr(node->iterable, target)) return r;
                        return find_struct_literal_expr(node->body, target);
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                        if (const auto *r = find_struct_literal_expr(node->operand, target)) return r;
                        for (const auto &arm : node->arms) {
                            if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                                if (lit->expr) {
                                    if (const auto *r = find_struct_literal_expr(*lit->expr, target)) return r;
                                }
                            }
                            if (const auto *r = find_struct_literal_expr(arm.body, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                        return find_struct_literal_expr(node->body, target);
                    } else if constexpr (std::is_same_v<U, ast::ExprStmt>) {
                        return find_struct_literal_expr(node.expr, target);
                    } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                        if (node.init) return find_struct_literal_expr(*node.init, target);
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                        return find_struct_literal_expr(node.init, target);
                    } else if constexpr (std::is_same_v<U, ast::ReturnStmt>) {
                        for (const auto &e : node.return_values) {
                            if (const auto *r = find_struct_literal_expr(e, target)) return r;
                        }
                        return nullptr;
                    } else if constexpr (std::is_same_v<U, ast::ReturnErrStmt>) {
                        return find_struct_literal_expr(node.error_value, target);
                    } else if constexpr (std::is_same_v<U, ast::ReturnOkStmt>) {
                        for (const auto &e : node.return_values) {
                            if (const auto *r = find_struct_literal_expr(e, target)) return r;
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
        if (!idx_opt || tokens[*idx_opt].kind != TokenKind::Identifier) return {};
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

        if (prefix.empty()) {
            // Struct-literal field designator (`.field = value`), e.g. inside
            // `{ .name = name, .member = member_slot }` - resolve against the
            // literal's contextual struct type before falling back to plain
            // identifier lookup, so a same-named local/param in scope can't
            // silently hijack the answer.
            if (enclosing.body) {
                if (const auto brace_loc = enclosing_struct_literal_brace(tokens, idx)) {
                    if (const auto *literal_expr = find_struct_literal_expr(*enclosing.body, *brace_loc)) {
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
