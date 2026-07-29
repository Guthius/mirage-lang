#include "common.hpp"

#include "ast_walker.hpp"
#include "compiler/asm_registers.hpp"
#include "compiler/lexer.hpp"

#include <algorithm>
#include <type_traits>

namespace lsp::handlers {
    auto build_bracket_index(const std::vector<Token> &tokens) -> std::unordered_map<size_t, size_t> {
        std::unordered_map<size_t, size_t> matches;
        std::vector<size_t> stack;

        const auto matching_open = [](const TokenKind close) -> TokenKind {
            switch (close) {
            case TokenKind::RParen:   return TokenKind::LParen;
            case TokenKind::RBrace:   return TokenKind::LBrace;
            case TokenKind::RBracket: return TokenKind::LBracket;
            default:                  return TokenKind::Eof; // unreachable given the switch below
            }
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            switch (tokens[i].kind) {
            case TokenKind::LParen:
            case TokenKind::LBrace:
            case TokenKind::LBracket:
                stack.push_back(i);
                break;
            case TokenKind::RParen:
            case TokenKind::RBrace:
            case TokenKind::RBracket:
                if (!stack.empty() && tokens[stack.back()].kind == matching_open(tokens[i].kind)) {
                    matches[stack.back()] = i;
                    stack.pop_back();
                }
                break;
            default:
                break;
            }
        }

        return matches;
    }

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
                return {
                    res, Container{.kind = Container::Kind::Type, .module_path = "", .type = res.type}
                };
            }
            if (res.kind != Resolution::Kind::None) {
                return {res, {}};
            }
        }

        return {};
    }

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
                    for (const auto &s : node->stmts)
                        walk_stmt_for_locals(s, name, before_line, ctx, best);
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
                    for (const auto &arm : node->arms)
                        walk_stmt_for_locals(arm.body, name, before_line, ctx, best);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                    walk_stmt_for_locals(node->body, name, before_line, ctx, best);
                } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                    if (node.name == name && node.location.line <= before_line) {
                        best = LocalInfo{.location = node.location, .type = resolve_var_decl_type(node, ctx)};
                    }
                } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                    if (node.location.line <= before_line) {
                        if (const auto idx = std::ranges::find(node.names, name); idx != node.names.end()) {
                            const auto name_index = static_cast<size_t>(idx - node.names.begin());
                            best = LocalInfo{.location = node.location, .type = resolve_group_decl_name_type(node, name_index, ctx)};
                        }
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

    // Generalizes walk_stmt_for_locals from "search for one specific name" to "record every
    // name declared before `before_line`" - same shadowing semantics (a later declaration of
    // the same name overwrites the earlier entry, since blocks are walked in source order).
    void collect_stmt_locals(const ast::Stmt &stmt, const size_t before_line, const LocalLookupContext &ctx,
                              std::unordered_map<std::string, LocalInfo> &out) {
        std::visit(
            [&]<typename T>(const T &node) {
                using U = std::decay_t<T>;
                if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                    for (const auto &s : node->stmts)
                        collect_stmt_locals(s, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                    collect_stmt_locals(node->then_stmt, before_line, ctx, out);
                    if (node->else_stmt) collect_stmt_locals(*node->else_stmt, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                    collect_stmt_locals(node->body, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                    if (node->location.line <= before_line) {
                        if (node->index_name != "_") out[node->index_name] = LocalInfo{.location = node->location, .type = {}};
                        out[node->element_name] = LocalInfo{.location = node->location, .type = {}};
                    }
                    collect_stmt_locals(node->body, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                    for (const auto &arm : node->arms)
                        collect_stmt_locals(arm.body, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                    collect_stmt_locals(node->body, before_line, ctx, out);
                } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                    if (node.location.line <= before_line) {
                        out[node.name] = LocalInfo{.location = node.location, .type = resolve_var_decl_type(node, ctx)};
                    }
                } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                    if (node.location.line <= before_line) {
                        for (size_t i = 0; i < node.names.size(); ++i) {
                            out[node.names[i]] = LocalInfo{.location = node.location, .type = resolve_group_decl_name_type(node, i, ctx)};
                        }
                    }
                }
                // ExprStmt, ContinueStmt, BreakStmt, ReturnStmt,
                // ReturnErrStmt, ReturnOkStmt declare no names.
            },
            stmt);
    }

    auto collect_locals_in_scope(const ast::Stmt &body, const LocalLookupContext &ctx, const size_t before_line)
        -> std::unordered_map<std::string, LocalInfo> {
        std::unordered_map<std::string, LocalInfo> out;
        collect_stmt_locals(body, before_line, ctx, out);
        return out;
    }

    // Scans forward from `start_index` (a declaration's own first token, e.g. 'fn'/'ext
    // fn'/'macro') for the ASI-inserted or explicit ';' that terminates it, tracking
    // paren/bracket depth so a ';' inside e.g. a macro's parameter type isn't mistaken for
    // the terminator. Used only for decl kinds with no '{ ... }' body (ext fn, macro),
    // where there's no brace to bracket-match against.
    auto find_declaration_end_line(const std::vector<Token> &tokens, const size_t start_index) -> size_t {
        int depth = 0;
        for (size_t i = start_index; i < tokens.size(); ++i) {
            switch (tokens[i].kind) {
            case TokenKind::LParen:
            case TokenKind::LBracket:
            case TokenKind::LBrace:
                ++depth;
                break;
            case TokenKind::RParen:
            case TokenKind::RBracket:
            case TokenKind::RBrace:
                if (depth > 0) --depth;
                break;
            case TokenKind::Semicolon:
                if (depth == 0) return tokens[i].location.line;
                break;
            default:
                break;
            }
        }
        return tokens.empty() ? start_index : tokens.back().location.line;
    }

    // Real [start,end] line containment, replacing the old "closest preceding decl by
    // line" heuristic (wrong for a cursor sitting after a short function but before the
    // next one starts - it would incorrectly attribute that position to the earlier
    // function). AST nodes carry only a first-token point location, never a span, so the
    // end line has to come from the token stream: for FunctionDecl/ImplDecl::Function,
    // the body's own location IS its opening '{' (parse_block_stmt captures it that way),
    // so its bracket-index match gives the real end directly; ext fn/macro have no body
    // block, so their end is wherever their own declaration's terminating ';' is.
    auto find_enclosing_function(const ast::Module &module, const sema::ProgramModule &sema_module,
                                 const sema::Program &program, const std::vector<Token> &tokens, const size_t line) -> EnclosingFunction {
        const auto bracket_index = build_bracket_index(tokens);

        auto end_line_for = [&](const SourceLocation &decl_location, const ast::Stmt *body) -> size_t {
            if (body) {
                if (const auto *block = std::get_if<std::unique_ptr<ast::BlockStmt>>(body)) {
                    if (const auto open_idx = token_at(tokens, (*block)->location.line, (*block)->location.column)) {
                        if (const auto match_it = bracket_index.find(*open_idx); match_it != bracket_index.end()) {
                            return tokens[match_it->second].location.line;
                        }
                    }
                }
                return decl_location.line;
            }
            if (const auto start_idx = token_at(tokens, decl_location.line, decl_location.column)) {
                return find_declaration_end_line(tokens, *start_idx);
            }
            return decl_location.line;
        };

        EnclosingFunction best;
        size_t best_start_line = 0;
        bool found = false;

        auto consider = [&](const SourceLocation &decl_location, std::vector<ParamInfo> params, const ast::Stmt *body) {
            const auto start_line = decl_location.line;
            if (start_line > line) return;
            if (line > end_line_for(decl_location, body)) return;

            // Candidate spans shouldn't overlap (Mirage has no nested function decls), so
            // this is just tie-breaking for the approximate ext-fn/macro end line: prefer
            // whichever candidate starts latest, same as the old heuristic did.
            if (!found || start_line >= best_start_line) {
                best_start_line = start_line;
                found = true;
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
                consider(fn->location, std::move(params), &fn->body);
            } else if (const auto *ext = std::get_if<ast::ExtFunctionDecl>(&decl)) {
                const auto sym_it = sema_module.symbols.find(ext->name);
                const auto *sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::ExtFunctionSymbol>(&sym_it->second) : nullptr;
                std::vector<ParamInfo> params;
                for (size_t i = 0; i < ext->params.size(); ++i) {
                    sema::ResolvedType type{};
                    if (sym && i < sym->params.size()) type = sym->params[i];
                    params.push_back({ext->params[i].name, type, ext->params[i].location});
                }
                consider(ext->location, std::move(params), nullptr);
            } else if (const auto *macro = std::get_if<ast::MacroDecl>(&decl)) {
                const auto sym_it = sema_module.symbols.find(macro->name);
                const auto *sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::MacroSymbol>(&sym_it->second) : nullptr;
                std::vector<ParamInfo> params;
                for (size_t i = 0; i < macro->params.size(); ++i) {
                    sema::ResolvedType type{};
                    if (sym && i < sym->params.size()) type = sym->params[i];
                    params.push_back({macro->params[i].name, type, macro->params[i].location});
                }
                consider(macro->location, std::move(params), nullptr);
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
                    consider(fn.location, std::move(params), &fn.body);
                }
            } else if (const auto *type_decl = std::get_if<ast::TypeDecl>(&decl)) {
                // A trait method ('type T = trait { fn foo(self, ...) -> ... }') has no body
                // (trait method declarations cannot have one - see parse_trait_method_decl in
                // ast.cpp) and never gets an ImplDecl::Function/MethodInfo of its own - without
                // this branch, a cursor sitting on 'self' or a named param inside one has no
                // EnclosingFunction at all, so resolve_base_name can never find them (see the
                // bug this fixed). 'self' is given the trait's OWN handle type (there being no
                // concrete Self type a trait declaration can name).
                const auto *trait_type = std::get_if<std::unique_ptr<ast::TraitType>>(&type_decl->type);
                if (!trait_type) continue;

                const auto sym_it = sema_module.symbols.find(type_decl->name);
                const auto *type_sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::TypeSymbol>(&sym_it->second) : nullptr;
                if (!type_sym || !type_sym->resolved || type_sym->resolved->kind != sema::TypeKind::Trait) continue;

                const auto *trait_info = program.trait_at(type_sym->resolved->trait_index);
                if (!trait_info) continue;

                for (const auto &method_info : trait_info->methods) {
                    const auto *method_decl = method_info.decl;
                    if (!method_decl) continue;

                    std::vector<ParamInfo> params;
                    params.push_back({"self", *type_sym->resolved, method_decl->self_location});
                    for (size_t i = 0; i < method_decl->params.size(); ++i) {
                        sema::ResolvedType type{};
                        if (i < method_info.params.size()) type = method_info.params[i];
                        params.push_back({method_decl->params[i].name, type, method_decl->params[i].location});
                    }
                    consider(method_decl->location, std::move(params), nullptr);
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
                    // Matches on the node's own location first (cursor on the 'size_of' keyword
                    // itself, e.g. for hover's constant-value display), else keeps looking inside
                    // the operand. Mirrors AlignOfExpr/LenExpr immediately below.
                    if (location_matches(node->location, target)) return &expr;
                    return find_expr_by_location(node->operand, target);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AlignOfExpr>>) {
                    if (location_matches(node->location, target)) return &expr;
                    return find_expr_by_location(node->operand, target);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::LenExpr>>) {
                    if (location_matches(node->location, target)) return &expr;
                    return find_expr_by_location(node->operand, target);
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CastExpr>>) {
                    if (const auto *r = find_expr_by_location(node->value, target)) return r;
                    if (node->len_expr) return find_expr_by_location(*node->len_expr, target);
                    return nullptr;
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                    if (const auto *r = find_expr_by_location(node->operand, target)) return r;
                    // Type-tagged args (e.g. 'List[SomeType]') aren't searched here, matching
                    // this function's existing scope of only ever finding Expr nodes.
                    for (const auto &arg : node->args) {
                        if (const auto *expr_arg = std::get_if<ast::Expr>(&arg.value)) {
                            if (const auto *r = find_expr_by_location(*expr_arg, target)) return r;
                        }
                    }
                    return nullptr;
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
                } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TypeExpr>>) {
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

    auto callee_name_location(const ast::Expr &callee, const std::vector<Token> &tokens) -> std::optional<SourceLocation> {
        if (const auto *ident = std::get_if<ast::IdentExpr>(&callee)) {
            return ident->location;
        }
        if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&callee)) {
            if (const auto dot_idx = token_at(tokens, (*member)->location.line, (*member)->location.column)) {
                if (*dot_idx + 1 < tokens.size()) return tokens[*dot_idx + 1].location;
            }
        }
        return std::nullopt;
    }

    auto callee_return_types(const Resolution &res) -> std::vector<sema::ResolvedType> {
        if (res.kind == Resolution::Kind::Method) {
            return res.method ? res.method->return_types : std::vector<sema::ResolvedType>{};
        }
        if (res.kind != Resolution::Kind::Symbol || !res.symbol) return {};

        return std::visit(
            [&]<typename T>(const T &sym) -> std::vector<sema::ResolvedType> {
                using S = std::decay_t<T>;
                if constexpr (std::is_same_v<S, sema::FunctionSymbol>) {
                    return sym.return_types;
                }
                // ExtFunctionSymbol/MacroSymbol never multi-return; GlobalSymbol/ImportSymbol/
                // TypeSymbol aren't callable at all.
                return {};
            },
            *res.symbol);
    }

    // A register or variable operand found inside an 'asm { ... }'/'asm -> reg { ... }' body at
    // some cursor position - copied by value (rather than returning an ast::AsmOperand* /
    // AsmRegisterOperand*) since AsmExpr::result_register isn't itself stored in the
    // ast::AsmOperand variant, so a single pointer-returning type can't represent both sources
    // uniformly.
    struct AsmOperandHit {
        bool is_register = false;
        std::string name;
        uint32_t width_bits = 0; // meaningful only if is_register
        bool is_address = false; // meaningful only if !is_register ('&var' vs bare 'var')
    };

    // Finds the (register or variable) operand within any 'asm { ... }'/'asm -> reg { ... }'
    // construct reachable from `body` whose own span contains 1-based (line, column). Needed
    // because asm bodies are raw-captured as a single opaque token in the main lexer's token
    // stream (see asm_lexer.cpp's raw-capture) - every line but the block's first has no token
    // of its own there at all, so resolve_at_tokens()'s ordinary token-based path below can
    // never see these identifiers. Each AsmOperand's own location is already accurate in the
    // *original* file's coordinates (asm diagnostics already report real file positions off
    // these same fields), so this walks the AST directly via the shared ast_walker instead.
    auto find_asm_operand_at(const ast::Stmt &body, const size_t line, const size_t column) -> std::optional<AsmOperandHit> {
        std::optional<AsmOperandHit> found;

        auto matches = [&](const SourceLocation &loc, const size_t name_len) {
            return name_len > 0 && loc.line == line && column >= loc.column && column < loc.column + name_len;
        };
        auto check_instructions = [&](const std::vector<ast::AsmInstruction> &instructions) {
            for (const auto &instr : instructions) {
                if (found) return;
                for (const auto &operand : instr.operands) {
                    if (found) return;
                    std::visit(
                        [&]<typename T>(const T &o) {
                            if constexpr (std::is_same_v<T, ast::AsmRegisterOperand>) {
                                if (matches(o.location, o.name.size())) {
                                    found = AsmOperandHit{.is_register = true, .name = o.name, .width_bits = o.width_bits};
                                }
                            } else if constexpr (std::is_same_v<T, ast::AsmVariableOperand>) {
                                // '&' (if present) is part of the token AsmVariableOperand::location
                                // anchors to - see asm_lexer.cpp's lex_address_of.
                                const auto len = o.name.size() + (o.is_address ? 1 : 0);
                                if (matches(o.location, len)) {
                                    found = AsmOperandHit{.is_register = false, .name = o.name, .is_address = o.is_address};
                                }
                            }
                            // AsmImmediateOperand - a literal, nothing to hover.
                        },
                        operand);
                }
            }
        };

        AstVisitor visitor;
        visitor.on_stmt = [&](const ast::Stmt &s) {
            if (const auto *asm_stmt = std::get_if<std::unique_ptr<ast::AsmStmt>>(&s)) {
                check_instructions((*asm_stmt)->instructions);
            }
        };
        visitor.on_expr = [&](const ast::Expr &e) {
            if (const auto *asm_expr = std::get_if<std::unique_ptr<ast::AsmExpr>>(&e)) {
                check_instructions((*asm_expr)->instructions);
                if (!found) {
                    const auto &reg = (*asm_expr)->result_register;
                    if (matches(reg.location, reg.name.size())) {
                        found = AsmOperandHit{.is_register = true, .name = reg.name, .width_bits = reg.width_bits};
                    }
                }
            }
        };
        walk_stmt(body, visitor);

        return found;
    }

    // Fallback search for size_of()/align_of()/len() used OUTSIDE any function/method body - a
    // module-scope const's initializer, or a macro's expression template - neither of which
    // find_enclosing_function() exposes a body for (EnclosingFunction::body is only ever set
    // for FunctionDecl/ImplDecl::Function, never ExtFunctionDecl/MacroDecl - see its own doc
    // comment above).
    auto find_expr_by_location_in_module(const ast::Module &module, const SourceLocation &target) -> const ast::Expr * {
        for (const auto &decl : module) {
            if (const auto *var = std::get_if<ast::VarDecl>(&decl)) {
                if (var->init) {
                    if (const auto *found = find_expr_by_location(*var->init, target)) return found;
                }
            } else if (const auto *macro = std::get_if<ast::MacroDecl>(&decl)) {
                if (const auto *found = find_expr_by_location(macro->expr_template, target)) return found;
            }
        }
        return nullptr;
    }

    // Fallback for an enum field or union(enum) variant name hovered at its OWN declaration
    // site (e.g. 'Red' in 'type Color = enum(i32) { Red = 0 }') - these names are never module
    // symbols in their own right (only the enclosing type's name, 'Color', is), so
    // resolve_base_name can't find them by the ordinary module-symbol-table lookup. Scans every
    // EnumType/UnionType TypeDecl in the module for a field/member whose own name-token span
    // contains (line, column), returning the same Kind::EnumField/Variant shape a usage-site
    // '.Field' reference resolves to (see match_enum_or_variant) - hover.cpp's rendering for
    // both is shared for free as a result.
    auto resolve_type_decl_field_at(const ast::Module &module, const sema::ProgramModule &sema_module,
                                     const size_t line, const size_t column) -> std::optional<Resolution> {
        const auto matches = [&](const SourceLocation &loc, const size_t name_len) {
            return name_len > 0 && loc.line == line && column >= loc.column && column < loc.column + name_len;
        };

        for (const auto &decl : module) {
            const auto *type_decl = std::get_if<ast::TypeDecl>(&decl);
            if (!type_decl) continue;

            const auto sym_it = sema_module.symbols.find(type_decl->name);
            const auto *type_sym = sym_it != sema_module.symbols.end() ? std::get_if<sema::TypeSymbol>(&sym_it->second) : nullptr;
            if (!type_sym || !type_sym->resolved) continue;

            if (const auto *enum_type = std::get_if<std::unique_ptr<ast::EnumType>>(&type_decl->type)) {
                for (const auto &field : (*enum_type)->fields) {
                    if (matches(field.location, field.name.size())) {
                        return Resolution{.kind = Resolution::Kind::EnumField, .name = field.name,
                                           .location = field.location, .type = *type_sym->resolved};
                    }
                }
            } else if (const auto *union_type = std::get_if<std::unique_ptr<ast::UnionType>>(&type_decl->type)) {
                if ((*union_type)->is_tagged) {
                    for (const auto &member : (*union_type)->members) {
                        if (matches(member.location, member.name.size())) {
                            return Resolution{.kind = Resolution::Kind::Variant, .name = member.name,
                                               .location = member.location, .type = *type_sym->resolved};
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    // Resolves a 'size_of'/'align_of' operand's own resolved type: an IdentExpr/(single-hop)
    // MemberExpr naming a TYPE (never run through check_expr, so never cached in expr_types -
    // see sema_check.cpp's SizeOfExpr/AlignOfExpr cases) is looked up directly against the
    // symbol table, exactly mirroring codegen.cpp's Generator::sizeof_operand()/
    // align_of_operand() and type_resolver.cpp's Resolver::sizeof_expr_operand()/
    // align_of_expr_operand() (this intentionally only replicates their single-hop
    // MemberExpr case, not the fuller multi-hop walk_namespace_chain those use - a qualified
    // type name nested two or more modules deep falls back to the ordinary value path
    // below and simply won't resolve a type name specially, same as an unresolvable ordinary
    // expression). Everything else (an ordinary value expression, or a TypeExpr for
    // pointer/array/slice/fn-ptr/builtin spellings that can't be written as an IdentExpr) was
    // already resolved and cached into expr_types by the whole-program check.
    auto sizeof_align_operand_type(const ast::Expr &operand, const std::string &module_path,
                                    const sema::Program &program) -> sema::ResolvedType {
        if (const auto *ident = std::get_if<ast::IdentExpr>(&operand)) {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                if (const auto sym_it = mod_it->second.symbols.find(ident->name); sym_it != mod_it->second.symbols.end()) {
                    if (const auto *ts = std::get_if<sema::TypeSymbol>(&sym_it->second); ts && ts->resolved) {
                        return *ts->resolved;
                    }
                }
            }
        } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&operand)) {
            if (const auto *obj_ident = std::get_if<ast::IdentExpr>(&(*member)->object)) {
                if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                    if (const auto sym_it = mod_it->second.symbols.find(obj_ident->name); sym_it != mod_it->second.symbols.end()) {
                        if (const auto *imp = std::get_if<sema::ImportSymbol>(&sym_it->second)) {
                            if (const auto target_mod_it = program.modules.find(imp->module_path); target_mod_it != program.modules.end()) {
                                if (const auto target_sym_it = target_mod_it->second.symbols.find((*member)->member);
                                    target_sym_it != target_mod_it->second.symbols.end()) {
                                    if (const auto *ts = std::get_if<sema::TypeSymbol>(&target_sym_it->second); ts && ts->resolved) {
                                        return *ts->resolved;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
            if (const auto ty_it = mod_it->second.expr_types.find(sema::get_expr_key(operand)); ty_it != mod_it->second.expr_types.end()) {
                return ty_it->second;
            }
        }
        return {};
    }

    // Builds the Kind::Builtin Resolution for a 'size_of'/'align_of'/'len' keyword token,
    // filling in the operand's resolved type and (when statically known) its folded value if
    // `found` is the corresponding SizeOfExpr/AlignOfExpr/LenExpr AST node - i.e. resolve_at_
    // tokens()/its module-scope fallback managed to locate it. `found` is nullptr only when
    // that lookup failed (shouldn't normally happen for a real keyword token, but resolve_at is
    // a best-effort UI utility, not a parser, so this degrades to the bare "builtin, type
    // usize" answer rather than crashing).
    auto resolve_builtin_at(const ast::Expr *found, const TokenKind keyword, const std::string &module_path,
                            const sema::Program &program) -> Resolution {
        Resolution res{
            .kind = Resolution::Kind::Builtin,
            .name = keyword == TokenKind::KwSizeOf ? "size_of" : keyword == TokenKind::KwAlignOf ? "align_of" : "len",
            .type = sema::ResolvedType{.kind = sema::TypeKind::USize},
        };
        if (!found) return res;

        if (const auto *size_of_expr = std::get_if<std::unique_ptr<ast::SizeOfExpr>>(found)) {
            res.builtin_operand_type = sizeof_align_operand_type((*size_of_expr)->operand, module_path, program);
            if (res.builtin_operand_type.kind != sema::TypeKind::Invalid) {
                res.builtin_const_value = sema::resolved_type_size(res.builtin_operand_type, program);
            }
        } else if (const auto *align_of_expr = std::get_if<std::unique_ptr<ast::AlignOfExpr>>(found)) {
            res.builtin_operand_type = sizeof_align_operand_type((*align_of_expr)->operand, module_path, program);
            if (res.builtin_operand_type.kind != sema::TypeKind::Invalid) {
                res.builtin_const_value = sema::resolved_type_align(res.builtin_operand_type, program);
            }
        } else if (const auto *len_expr = std::get_if<std::unique_ptr<ast::LenExpr>>(found)) {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                if (const auto ty_it = mod_it->second.expr_types.find(sema::get_expr_key((*len_expr)->operand));
                    ty_it != mod_it->second.expr_types.end()) {
                    res.builtin_operand_type = ty_it->second;
                    // Only a fixed-size array's length is a compile-time constant; a slice's is
                    // a runtime field (see sema_check.cpp's LenExpr case).
                    if (res.builtin_operand_type.kind == sema::TypeKind::Array) {
                        if (const auto *arr = program.array_at(res.builtin_operand_type.array_index)) {
                            res.builtin_const_value = arr->count;
                        }
                    }
                }
            }
        }
        return res;
    }

    // Scans forward from `from_idx` (inclusive) tracking paren/bracket/brace depth, returning
    // the index of the first token in `kinds` found at depth 0, or nullopt if none is found
    // before the token stream ends.
    auto find_token_at_depth_zero(const std::vector<Token> &tokens, const size_t from_idx,
                                   std::initializer_list<TokenKind> kinds) -> std::optional<size_t> {
        int depth = 0;
        for (size_t i = from_idx; i < tokens.size(); ++i) {
            switch (tokens[i].kind) {
            case TokenKind::LParen:
            case TokenKind::LBracket:
            case TokenKind::LBrace:
                ++depth;
                break;
            case TokenKind::RParen:
            case TokenKind::RBracket:
            case TokenKind::RBrace:
                if (depth > 0) --depth;
                break;
            default:
                if (depth == 0 && std::ranges::find(kinds, tokens[i].kind) != kinds.end()) {
                    return i;
                }
                break;
            }
        }
        return std::nullopt;
    }

    auto raw_const_init_text(const std::vector<Token> &tokens, const std::string_view source_text,
                              const SourceLocation &decl_location) -> std::string {
        // A module-scope VarDecl's own location is its 'const'/'mut' keyword (see
        // parse_var_decl in ast.cpp) - not the initializer expression's, which (unlike this
        // scan) can't be used directly here: this AST stores each expression node's own
        // single DEFINING token location, not a true span start, and for a compound top-level
        // initializer (e.g. a BinaryExpr) that's the OPERATOR's position, not the expression's
        // first token (see ast.cpp's parse_multiplicative/parse_bitwise_and et al., which
        // capture 'location' at the operator, before or after consuming it, inconsistently
        // across precedence levels) - starting from decl_location and finding the real ':='/'='
        // token by scanning instead sidesteps that entirely.
        const auto decl_idx_opt = token_at(tokens, decl_location.line, decl_location.column);
        if (!decl_idx_opt) return {};

        const auto assign_idx_opt = find_token_at_depth_zero(tokens, *decl_idx_opt, {TokenKind::ColonEqual, TokenKind::Equal});
        if (!assign_idx_opt || *assign_idx_opt + 1 >= tokens.size()) return {};
        const auto init_start_idx = *assign_idx_opt + 1;

        const auto semi_idx_opt = find_token_at_depth_zero(tokens, init_start_idx, {TokenKind::Semicolon});
        const auto end_offset = semi_idx_opt && *semi_idx_opt > 0
                                     ? tokens[*semi_idx_opt - 1].location.offset + tokens[*semi_idx_opt - 1].lexeme.size()
                                     : source_text.size();

        const auto start_offset = tokens[init_start_idx].location.offset;
        if (end_offset <= start_offset || end_offset > source_text.size()) return {};
        return std::string(source_text.substr(start_offset, end_offset - start_offset));
    }

    auto resolve_at_tokens(analysis::ProgramResult &result, const std::string &module_path,
                          const std::vector<Token> &tokens, const size_t line, const size_t column) -> Resolution {
        const auto mod_it = result.ast_program.modules.find(module_path);
        const auto sema_mod_it = result.sema_program.modules.find(module_path);
        if (mod_it == result.ast_program.modules.end() || sema_mod_it == result.sema_program.modules.end()) {
            return {};
        }

        DiagnosticEngine throwaway_diag(*result.source_manager);
        const LocalLookupContext ctx{
            .sema_module = sema_mod_it->second,
            .sema_program = result.sema_program,
            .module_path = module_path,
            .diag = throwaway_diag,
            .tokens = &tokens,
            .program_result = &result,
        };

        const auto enclosing = find_enclosing_function(mod_it->second, sema_mod_it->second, result.sema_program, tokens, line);

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

        // Asm operand hover (register or variable) - must run before the ordinary token-based
        // path below, since asm bodies are raw-captured as a single opaque token in `tokens`
        // (see find_asm_operand_at's own doc comment for why token_at() can't see inside one).
        if (enclosing.body) {
            if (const auto hit = find_asm_operand_at(*enclosing.body, line, column)) {
                if (hit->is_register) {
                    Resolution res{.kind = Resolution::Kind::AsmRegister, .name = hit->name, .asm_register_width_bits = hit->width_bits};
                    if (const auto *info = asm_registers::lookup_register(hit->name)) {
                        res.asm_register_family = std::string(info->family);
                    }
                    return res;
                }
                // Variable operand - resolves exactly like an ordinary identifier reference
                // (param, then local, then module symbol); sema's own asm diagnostics already
                // report "unknown identifier" for one that resolves to nothing here.
                return resolve_base_name(hit->name).value_or(Resolution{});
            }
        }

        const auto idx_opt = token_at(tokens, line, column);
        if (!idx_opt) return {};
        const auto idx = *idx_opt;

        // size_of/align_of/len are dedicated keyword tokens (TokenKind::KwSizeOf/KwAlignOf/
        // KwLen), not TokenKind::Identifier, so they'd otherwise be rejected by the guard below -
        // resolve them directly as a synthetic Kind::Builtin, filling in the operand's type and
        // (when statically known) its folded constant value.
        if (tokens[idx].kind == TokenKind::KwSizeOf || tokens[idx].kind == TokenKind::KwAlignOf || tokens[idx].kind == TokenKind::KwLen) {
            const ast::Expr *found = enclosing.body ? find_expr_by_location(*enclosing.body, tokens[idx].location) : nullptr;
            if (!found) found = find_expr_by_location_in_module(mod_it->second, tokens[idx].location);
            return resolve_builtin_at(found, tokens[idx].kind, module_path, result.sema_program);
        }
        if (tokens[idx].kind != TokenKind::Identifier) return {};

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
            if (const auto base = resolve_base_name(tokens[idx].lexeme)) return *base;
            // Not a local/param/module symbol - could still be an enum field/union(enum)
            // variant name at its OWN declaration site (e.g. 'Red' in 'enum(i32) { Red = 0 }'),
            // which isn't a symbol in its own right. See resolve_type_decl_field_at's own doc
            // comment for why this needs to be a whole-module scan rather than something
            // resolve_base_name itself could ever find.
            return resolve_type_decl_field_at(mod_it->second, sema_mod_it->second, line, column).value_or(Resolution{});
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

    auto resolve_at(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                    const size_t line, const size_t column) -> Resolution {
        DiagnosticEngine throwaway_diag(*result.source_manager);
        const auto source_file = result.source_manager->load(path, throwaway_diag);
        if (source_file.text.empty()) return {};

        const auto tokens = lexer::tokenize(source_file.text, source_file.filename, throwaway_diag);
        return resolve_at_tokens(result, module_path, tokens, line, column);
    }

    auto resolve_group_decl_name_type(const ast::VarDeclGroupStmt &node, const size_t name_index,
                                       const LocalLookupContext &ctx) -> sema::ResolvedType {
        if (!ctx.tokens || !ctx.program_result) return {};
        if (name_index >= node.names.size()) return {};

        bool is_try = false;
        const ast::Expr *inner = &node.init;
        if (const auto *try_expr = std::get_if<std::unique_ptr<ast::TryExpr>>(&node.init)) {
            is_try = true;
            inner = &(*try_expr)->call;
        }

        const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(inner);
        if (!call) return {};

        const auto callee_loc = callee_name_location((*call)->callee, *ctx.tokens);
        if (!callee_loc) return {};

        const auto callee_res = resolve_at_tokens(*ctx.program_result, ctx.module_path, *ctx.tokens, callee_loc->line, callee_loc->column);
        auto returns = callee_return_types(callee_res);
        if (returns.empty()) return {};

        // 'try f()' strips the trailing error(...) slot off the call's own return list before
        // matching it against the group decl's names - mirrors sema_check.cpp's
        // VarDeclGroupStmt handling exactly (see check_group_call_returns's caller there).
        if (is_try) {
            const auto &last = returns.back();
            if (last.kind == sema::TypeKind::Union) {
                if (const auto *info = ctx.sema_program.union_at(last.union_index); info && info->is_error_union) {
                    returns.pop_back();
                }
            }
        }

        if (name_index >= returns.size()) return {};
        return returns[name_index];
    }
}
