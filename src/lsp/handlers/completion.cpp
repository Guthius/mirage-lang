#include "completion.hpp"

#include "../type_printer.hpp"
#include "common.hpp"

#include "compiler/lexer.hpp"

#include <ranges>
#include <type_traits>

namespace lsp::handlers {
    namespace {
        using json = nlohmann::json;

        // LSP CompletionItemKind values (textDocument/completion's protocol constants).
        enum class ItemKind : int {
            Text = 1,
            Method = 2,
            Function = 3,
            Field = 5,
            Variable = 6,
            Class = 7,
            Interface = 8,
            Module = 9,
            Property = 10,
            Enum = 13,
            Keyword = 14,
            EnumMember = 20,
            Constant = 21,
            Struct = 22,
        };

        auto item(std::string label, const ItemKind kind, std::string detail = "") -> json {
            json out{{"label", label}, {"kind", static_cast<int>(kind)}};
            if (!detail.empty()) out["detail"] = std::move(detail);
            return out;
        }

        auto starts_with(const std::string &text, const std::string &prefix) -> bool {
            return prefix.empty() || text.starts_with(prefix);
        }

        void add_keywords(const std::string &prefix, std::vector<json> &out) {
            for (const auto &spelling : lexer::keyword_spellings() | std::views::keys) {
                if (starts_with(std::string(spelling), prefix)) {
                    out.emplace_back(item(std::string(spelling), ItemKind::Keyword));
                }
            }
        }

        // Builtin type spellings offerable as a generic argument. Deliberately a fixed list
        // rather than a filter over keyword_spellings(): most keywords ('if', 'return',
        // 'match') are meaningless in a type position, and offering them there is noise.
        constexpr std::string_view BUILTIN_TYPE_NAMES[] = {
            "bool", "byte", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
            "usize", "f32", "f64", "anyptr", "type",
        };

        // Type names only: what belongs in a generic argument list. The counterpart to
        // add_module_symbols, which offers every symbol kind and would suggest functions and
        // globals in a position where neither can appear.
        void add_type_names(const sema::SymbolTable &symbols, const std::string &prefix, std::vector<json> &out) {
            for (const auto &name : BUILTIN_TYPE_NAMES) {
                if (starts_with(std::string(name), prefix)) {
                    out.emplace_back(item(std::string(name), ItemKind::Keyword, "builtin type"));
                }
            }
            for (const auto &[name, symbol] : symbols) {
                if (!starts_with(name, prefix)) continue;
                if (std::holds_alternative<sema::TypeSymbol>(symbol)) {
                    out.emplace_back(item(name, ItemKind::Struct, "type"));
                } else if (std::holds_alternative<sema::ImportSymbol>(symbol)) {
                    // A module can qualify a type ('mod.Thing'), so it is still worth offering.
                    out.emplace_back(item(name, ItemKind::Module, "module"));
                }
            }
        }

        void add_locals_and_params(const EnclosingFunction &enclosing, const LocalLookupContext &ctx,
                                    const size_t before_line, const sema::Program &program,
                                    const std::string &module_path, const std::string &prefix,
                                    std::vector<json> &out) {
            for (const auto &p : enclosing.params) {
                if (starts_with(p.name, prefix)) {
                    out.emplace_back(item(p.name, ItemKind::Variable,
                                           ": " + (p.display_override ? *p.display_override : type_to_string(p.type, program, module_path))));
                }
            }
            if (enclosing.body) {
                for (const auto &[name, info] : collect_locals_in_scope(*enclosing.body, ctx, before_line)) {
                    if (starts_with(name, prefix)) {
                        out.emplace_back(item(name, ItemKind::Variable,
                                               ": " + (info.display_override ? *info.display_override : type_to_string(info.type, program, module_path))));
                    }
                }
            }
        }

        void add_module_symbols(const sema::SymbolTable &symbols, const sema::Program &program,
                                 const std::string &module_path, const std::string &prefix, std::vector<json> &out) {
            for (const auto &[name, symbol] : symbols) {
                if (!starts_with(name, prefix)) continue;

                std::visit(
                    [&]<typename T>(const T &sym) {
                        using S = std::decay_t<T>;
                        if constexpr (std::is_same_v<S, sema::GlobalSymbol>) {
                            out.emplace_back(item(name, sym.is_mut ? ItemKind::Variable : ItemKind::Constant,
                                                   ": " + type_to_string(sym.type, program, module_path)));
                        } else if constexpr (std::is_same_v<S, sema::FunctionSymbol> || std::is_same_v<S, sema::ExtFunctionSymbol>) {
                            out.emplace_back(item(name, ItemKind::Function, "fn(" + std::to_string(sym.params.size()) + " args)"));
                        } else if constexpr (std::is_same_v<S, sema::MacroSymbol>) {
                            out.emplace_back(item(name, ItemKind::Function,
                                                   "macro -> " + type_to_string(sym.result_type, program, module_path)));
                        } else if constexpr (std::is_same_v<S, sema::ImportSymbol>) {
                            out.emplace_back(item(name, ItemKind::Module, "module"));
                        } else if constexpr (std::is_same_v<S, sema::TypeSymbol>) {
                            out.emplace_back(item(name, ItemKind::Struct, "type"));
                        }
                    },
                    symbol);
            }
        }

        // Whether the cursor sits inside an unclosed '[' whose opener follows an identifier,
        // and if so the index of the token naming what is being indexed or instantiated.
        //
        // The parser itself refuses to decide this ('arr[i]' and 'List[i32]' are the same
        // production, IndexOrInstantiateExpr, classified in sema once the identifier's
        // declaration is known). Here there is no parse at all -- completion tokenizes a
        // buffer that is usually mid-edit and often unparseable -- so the bracket structure
        // is walked directly, backwards, counting depth.
        auto enclosing_bracket_owner(const std::vector<Token> &tokens, const size_t anchor) -> std::optional<size_t> {
            int depth = 0;
            for (size_t i = anchor; i-- > 0;) {
                const auto kind = tokens[i].kind;
                if (kind == TokenKind::RBracket) {
                    ++depth;
                } else if (kind == TokenKind::LBracket) {
                    if (depth == 0) {
                        // Unclosed '[' at this level: its owner is whatever precedes it.
                        if (i == 0 || tokens[i - 1].kind != TokenKind::Identifier) return std::nullopt;
                        return i - 1;
                    }
                    --depth;
                } else if (kind == TokenKind::LBrace || kind == TokenKind::RBrace ||
                           kind == TokenKind::Semicolon) {
                    // A statement or block boundary: the cursor is not inside a bracket that
                    // opened before it. Stops the scan running away over the whole file.
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        // Enumerates struct fields/union members/tagged-union variants/enum fields/methods
        // (including trait methods) of `type_in` - the enumeration counterpart to
        // resolve_member()'s "look up one named member" - transparently dereferencing one
        // level of pointer first, same as resolve_member.
        void add_type_members(const sema::ResolvedType &type_in, const sema::Program &program,
                               const std::string &module_path, const std::string &prefix, std::vector<json> &out) {
            auto type = type_in;
            if (type.kind == sema::TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(type.pointee_index);
                if (!pointee) return;
                type = *pointee;
            }

            if (type.kind == sema::TypeKind::Struct) {
                if (const auto *info = program.struct_at(type.struct_index)) {
                    for (const auto &field : info->fields) {
                        if (starts_with(field.name, prefix)) {
                            out.emplace_back(item(field.name, ItemKind::Field, ": " + type_to_string(field.type, program, module_path)));
                        }
                    }
                }
            } else if (type.kind == sema::TypeKind::Union) {
                if (const auto *info = program.union_at(type.union_index)) {
                    if (info->is_tagged) {
                        for (const auto &variant : info->variants) {
                            if (starts_with(variant.name, prefix)) {
                                out.emplace_back(item(variant.name, ItemKind::EnumMember, "variant"));
                            }
                        }
                    } else {
                        for (const auto &member : info->members) {
                            if (starts_with(member.name, prefix)) {
                                out.emplace_back(item(member.name, ItemKind::Field, ": " + type_to_string(member.type, program, module_path)));
                            }
                        }
                    }
                }
            } else if (type.kind == sema::TypeKind::Enum) {
                if (const auto *info = program.enum_at(type.enum_index)) {
                    for (const auto &field : info->fields) {
                        if (starts_with(field.name, prefix)) {
                            out.emplace_back(item(field.name, ItemKind::EnumMember, "enum field"));
                        }
                    }
                }
            } else if (type.kind == sema::TypeKind::Bitset) {
                // A bitset's completions are its member enum's fields (LSPH-2): the bitset
                // declares none of its own.
                //
                // Reached exclusively via the CONTEXTUAL '.Name' path: a flag is never
                // written with a receiver, only as a bare '.Name' taking its meaning from the
                // expected type (see expected_type_at). This arm was unreachable, and
                // untested, until that path existed.
                if (const auto *info = program.bitset_at(type.bitset_index)) {
                    if (const auto *member_enum = program.enum_at(info->member_enum_type.enum_index)) {
                        for (const auto &field : member_enum->fields) {
                            if (starts_with(field.name, prefix)) {
                                out.emplace_back(item(field.name, ItemKind::EnumMember, "bitset flag"));
                            }
                        }
                    }
                }
            } else if (type.kind == sema::TypeKind::Trait) {
                // Dynamic dispatch through a handle (LSPH-1): the methods live on the trait
                // itself, not on any concrete implementing type, so the method sweep below --
                // which keys off find_type_module_and_name and ProgramModule::methods -- never
                // finds them. 'shape.<complete>' where 'shape: Drawable' offered nothing.
                if (const auto *trait = program.trait_at(type.trait_index)) {
                    for (const auto &method : trait->methods) {
                        if (starts_with(method.name, prefix)) {
                            out.emplace_back(item(method.name, ItemKind::Method, "trait method"));
                        }
                    }
                }
            }

            const auto [type_module, type_name] = sema::find_type_module_and_name(type, program);
            if (type_name.empty()) return;

            if (const auto mod_it = program.modules.find(type_module); mod_it != program.modules.end()) {
                if (const auto methods_it = mod_it->second.methods.find(type_name); methods_it != mod_it->second.methods.end()) {
                    for (const auto &method_name : methods_it->second | std::views::keys) {
                        if (starts_with(method_name, prefix)) {
                            out.emplace_back(item(method_name, ItemKind::Method, "method"));
                        }
                    }
                }
            }

            if (const auto impls_it = program.trait_impls_by_type.find({type_module, type_name});
                impls_it != program.trait_impls_by_type.end()) {
                for (const auto &trait_impl : impls_it->second) {
                    for (const auto &method_name : trait_impl.methods | std::views::keys) {
                        if (starts_with(method_name, prefix)) {
                            out.emplace_back(item(method_name, ItemKind::Method, "trait method"));
                        }
                    }
                }
            }
        }
    }

    // Four completion contexts:
    //
    //   'receiver.<cursor>'    member access, via chain_prefix + add_type_members
    //   '.<cursor>'            CONTEXTUAL: a bare '.Name' whose meaning comes from the
    //                          expected type - an enum field, a tagged-union variant, a
    //                          bitset flag. See expected_type_at.
    //   'Generic[<cursor>]'    type arguments, via enclosing_bracket_owner + add_type_names
    //   anything else          keywords, locals/params, module symbols
    // Only these three kinds have contextual '.Name' members worth offering. Anything else
    // means the surrounding shape determined a type, but not one a leading dot can name.
    auto has_contextual_members(const sema::ResolvedType &type) -> bool {
        return type.kind == sema::TypeKind::Enum || type.kind == sema::TypeKind::Bitset ||
               type.kind == sema::TypeKind::Union;
    }

    auto is_assignment_op(const TokenKind kind) -> bool {
        switch (kind) {
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::AmpEqual:
        case TokenKind::PipeEqual:
        case TokenKind::CaretEqual:
            return true;
        default:
            return false;
        }
    }

    // The type a bare '.Name' at 'dot_index' takes its meaning from, or nullopt when nothing
    // in the surrounding text determines one.
    //
    // Two strategies, in this order:
    //
    //  1. ASK SEMA. check_expr's DotIdentExpr case returns the expected type as the node's
    //     OWN type, so when the buffer parsed and this '.Name' made it into the tree, its
    //     recorded type IS the answer. Exact, free, and handles arbitrarily complex contexts
    //     (a nested call argument, a match arm on a field access, ...) with no new analysis.
    //
    //  2. INFER FROM TOKENS. Completion runs on buffers that are usually mid-edit and often
    //     unparseable — a bare '.' with nothing after it typically kills the whole enclosing
    //     declaration, since there is no error-recovery expression node — so strategy 1 is
    //     unavailable exactly when the user is most likely to be asking. These are the shapes
    //     worth recovering, in decreasing order of how often they come up.
    //
    // Deliberately conservative: an unrecognised shape returns nullopt and the caller falls
    // back to what it did before, rather than guessing.
    auto expected_type_at(const std::vector<Token> &tokens, const size_t dot_index,
                           const EnclosingFunction &enclosing, const LocalLookupContext &ctx,
                           const sema::ProgramModule &sema_module, sema::Program &program,
                           const std::function<std::optional<Resolution>(const std::string &)> &resolve_base_name)
        -> std::optional<sema::ResolvedType> {
        // (1) sema's own answer.
        if (enclosing.body) {
            if (const auto *found = find_expr_by_location(*enclosing.body, tokens[dot_index].location)) {
                if (const auto *ty = find_expr_type(ctx, *found); ty && has_contextual_members(*ty)) {
                    return *ty;
                }
            }
        }

        // (1b) Still sema's answer, one level out. Inside a braced literal the '.Name' items
        // are often NOT expression nodes — a bitset literal's members are plain strings on
        // BitsetExpr, with no expr_types entry of their own — but the LITERAL is, and its
        // recorded type is exactly the type those names belong to.
        if (enclosing.body) {
            if (const auto brace_loc = enclosing_struct_literal_brace(tokens, dot_index + 1)) {
                if (const auto *literal = find_expr_by_location(*enclosing.body, *brace_loc)) {
                    if (const auto *ty = find_expr_type(ctx, *literal); ty && has_contextual_members(*ty)) {
                        return *ty;
                    }
                }
            }
        }

        const auto type_of_name = [&](const std::string &name) -> std::optional<sema::ResolvedType> {
            if (const auto res = resolve_base_name(name); res && has_contextual_members(res->type)) {
                return res->type;
            }
            return std::nullopt;
        };

        // (2a) 'modes += .' / 'modes = .' — assignment to something already typed. Covers the
        // bitset case DEFERRED called out by name.
        if (dot_index >= 2 && is_assignment_op(tokens[dot_index - 1].kind) &&
            tokens[dot_index - 2].kind == TokenKind::Identifier) {
            if (auto ty = type_of_name(tokens[dot_index - 2].lexeme)) return ty;
        }

        // (2b) 'const m: Mode = .' — an explicit type annotation names the answer outright.
        if (dot_index >= 3 && tokens[dot_index - 1].kind == TokenKind::Equal &&
            tokens[dot_index - 2].kind == TokenKind::Identifier &&
            tokens[dot_index - 3].kind == TokenKind::Colon) {
            if (const auto sym_it = sema_module.symbols.find(tokens[dot_index - 2].lexeme);
                sym_it != sema_module.symbols.end()) {
                if (const auto *ts = std::get_if<sema::TypeSymbol>(&sym_it->second);
                    ts && ts->resolved && has_contextual_members(*ts->resolved)) {
                    return *ts->resolved;
                }
            }
        }

        // (2c) A match/switch arm: '.Variant' takes its meaning from the scrutinee. Walk back
        // to the innermost enclosing '{' and check whether the construct that opened it is a
        // match or switch; if so, the tokens between the keyword and the brace are the
        // scrutinee. Only a plain identifier chain is reconstructed here — anything more
        // (a call, an index) is what strategy 1 is for.
        int depth = 0;
        for (size_t i = dot_index; i-- > 0;) {
            const auto kind = tokens[i].kind;
            if (kind == TokenKind::RBrace) {
                ++depth;
            } else if (kind == TokenKind::LBrace) {
                if (depth > 0) { --depth; continue; }
                // Unmatched '{': this is our enclosing block. Is it a match/switch body?
                for (size_t j = i; j-- > 0;) {
                    if (tokens[j].kind == TokenKind::KwMatch || tokens[j].kind == TokenKind::KwSwitch) {
                        // Scrutinee is tokens (j, i). Take the last identifier chain in it.
                        if (i >= 1 && tokens[i - 1].kind == TokenKind::Identifier) {
                            auto names = chain_prefix(tokens, i - 1);
                            names.push_back(tokens[i - 1].lexeme);
                            if (auto ty = type_of_name(names.front()); ty && names.size() == 1) return ty;
                            // A dotted chain: resolve the base, then step through the fields.
                            if (const auto base = resolve_base_name(names.front())) {
                                const auto container = walk_chain(chain_start(*base), std::span(names).subspan(1), program);
                                if (container.kind == Container::Kind::Type && has_contextual_members(container.type)) {
                                    return container.type;
                                }
                            }
                        }
                        return std::nullopt;
                    }
                    // Anything that can't be part of a scrutinee expression ends the search.
                    if (tokens[j].kind == TokenKind::RBrace || tokens[j].kind == TokenKind::Semicolon ||
                        tokens[j].kind == TokenKind::LBrace) {
                        break;
                    }
                }
                break;
            }
        }

        return std::nullopt;
    }

    auto handle_completion(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, const size_t line, const size_t column) -> json {
        DiagnosticEngine throwaway_diag(*result.source_manager);
        const auto source_file = result.source_manager->load(path, throwaway_diag);
        if (source_file.text.empty()) return json::array();

        const auto tokens = lexer::tokenize(source_file.text, source_file.filename, throwaway_diag);

        const auto mod_it = result.ast_program.modules.find(module_path);
        const auto sema_mod_it = result.sema_program.modules.find(module_path);
        if (mod_it == result.ast_program.modules.end() || sema_mod_it == result.sema_program.modules.end()) {
            return json::array();
        }

        // Resolved up here, ahead of the cursor analysis that used to precede it, only so the
        // context below can carry this declaration's template ExprSideTables - see
        // LocalLookupContext::template_exprs. Nothing between here and its old position
        // depends on it.
        const auto enclosing = find_enclosing_function(mod_it->second, sema_mod_it->second, result.sema_program, module_path, throwaway_diag, tokens, line);

        const LocalLookupContext ctx{
            .sema_module = sema_mod_it->second,
            .sema_program = result.sema_program,
            .module_path = module_path,
            .diag = throwaway_diag,
            .tokens = &tokens,
            .program_result = &result,
            .template_exprs = template_exprs_for(enclosing, result.sema_program),
        };

        // Find whatever's immediately before the cursor: either a partially-typed identifier
        // (filter completion by its text so far) or, if the cursor sits right after a token
        // with nothing typed yet, that token itself (relevant only when it's a '.', which
        // triggers member completion with an empty filter).
        std::string prefix;
        std::optional<size_t> identifier_index;
        std::optional<size_t> chain_anchor; // index whose "chain_prefix(tokens, .)" gives the receiver chain

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto &t = tokens[i];
            if (t.location.line != line) continue;
            const auto [start, end] = token_span(t);
            // Half-open at the LEFT, the opposite of common.cpp's token_at — deliberately.
            // The question here is "did the cursor just follow this token", not "is it on it";
            // see token_span's doc comment. Do NOT align these two.
            if (column > start && column <= end) {
                if (t.kind == TokenKind::Identifier) {
                    identifier_index = i;
                    prefix = t.lexeme.substr(0, column - start);
                    chain_anchor = i;
                } else if (column == end) {
                    chain_anchor = i + 1; // cursor right after this token, nothing typed since
                }
                break;
            }
        }

        std::vector<std::string> chain_prefix_names;
        bool dot_triggered = false;
        bool contextual_dot = false;
        if (chain_anchor) {
            chain_prefix_names = chain_prefix(tokens, *chain_anchor);
            if (!chain_prefix_names.empty()) {
                dot_triggered = true;
            } else if (*chain_anchor >= 1 && tokens[*chain_anchor - 1].kind == TokenKind::Dot) {
                // A '.' with no identifier chain before it. Either a CONTEXTUAL '.Name' — whose
                // meaning comes from the expected type — or a member access on a receiver
                // chain_prefix cannot reconstruct (a call or index result). expected_type_at
                // tells the two apart: it answers only for the former.
                contextual_dot = true;
            }
        }

        auto resolve_base = [&](const std::string &name) -> std::optional<Resolution> {
            return resolve_base_name(name, line, enclosing.params, enclosing.body, ctx);
        };

        std::vector<json> items;

        if (contextual_dot) {
            if (const auto expected = expected_type_at(tokens, *chain_anchor - 1, enclosing, ctx,
                                                        sema_mod_it->second, result.sema_program,
                                                        resolve_base)) {
                add_type_members(*expected, result.sema_program, module_path, prefix, items);
                return items;
            }
            // Nothing here determines a type. With nothing typed after the '.' there is
            // genuinely nothing to offer (the pre-existing behaviour for an unreconstructable
            // receiver); with a partially-typed name, fall through to value completions, which
            // is what that case did before.
            if (!identifier_index) return json::array();
        }

        if (dot_triggered) {
            const auto base = resolve_base(chain_prefix_names[0]);
            if (!base) return json::array();

            const auto container = walk_chain(chain_start(*base), std::span(chain_prefix_names).subspan(1), result.sema_program);

            if (container.kind == Container::Kind::Module) {
                if (const auto other_mod = result.sema_program.modules.find(container.module_path);
                    other_mod != result.sema_program.modules.end()) {
                    add_module_symbols(other_mod->second.symbols, result.sema_program, module_path, prefix, items);
                }
            } else if (container.kind == Container::Kind::Type) {
                add_type_members(container.type, result.sema_program, module_path, prefix, items);
            }
        } else {
            // Inside 'Something[...]' (LSPH-9). Whether that is a generic instantiation or an
            // ordinary index cannot be decided from tokens alone -- it is the same production
            // to the parser, resolved in sema once the identifier's declaration is known, and
            // here there is no parse to consult. So it is decided by what the identifier
            // actually names: a generic type declaration means type arguments, anything else
            // (or nothing recognizable, in a buffer mid-edit) means an index.
            //
            // Degrading toward the value completions rather than toward nothing matters: a
            // wrong guess that offers an empty list is worse than the old behaviour, while a
            // wrong guess that offers values is exactly the old behaviour.
            if (chain_anchor) {
                if (const auto owner = enclosing_bracket_owner(tokens, *chain_anchor)) {
                    const auto &owner_name = tokens[*owner].lexeme;
                    const auto sym_it = sema_mod_it->second.symbols.find(owner_name);
                    const auto *type_sym = sym_it != sema_mod_it->second.symbols.end()
                        ? std::get_if<sema::TypeSymbol>(&sym_it->second)
                        : nullptr;
                    if (type_sym && type_sym->decl && !type_sym->decl->generic_params.empty()) {
                        add_type_names(sema_mod_it->second.symbols, prefix, items);
                        return items;
                    }
                }
            }

            add_keywords(prefix, items);
            add_locals_and_params(enclosing, ctx, line, result.sema_program, module_path, prefix, items);
            add_module_symbols(sema_mod_it->second.symbols, result.sema_program, module_path, prefix, items);
        }

        return items;
    }
}
