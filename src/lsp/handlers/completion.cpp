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
            "usize", "isize", "f32", "f64", "anyptr", "type",
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
                // NOTE: this arm is currently UNREACHABLE from handle_completion, and adding
                // it did not change any observable behaviour. Getting here needs a receiver
                // expression whose type is a bitset, and no such expression exists -- a flag
                // is only ever written contextually, as a bare '.Name' taking its meaning
                // from the expected type. That form has no receiver chain, so the handler
                // takes the no-receiver path and never consults a type at all. Reaching this
                // needs contextual '.' completion, which is a separate feature; kept because
                // it is correct and is what that feature will call. Pinned as unimplemented
                // in lsp_smoke_test.py so wiring it up is a visible change.
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

    // Three completion contexts, decided from the token stream alone (there is no parse --
    // the buffer is usually mid-edit and often unparseable):
    //
    //   'receiver.<cursor>'    member access, via chain_prefix + add_type_members
    //   'Generic[<cursor>]'    type arguments, via enclosing_bracket_owner + add_type_names
    //   anything else          keywords, locals/params, module symbols
    //
    // Still missing: the contextual '.<cursor>' form, where a bare '.Name' takes its meaning
    // from the expected type (an enum field, a tagged-union variant, a bitset flag). It has
    // no receiver, so it lands in the third case and is offered value completions. Reaching
    // add_type_members' Enum/Bitset arms from there needs the expected type at the cursor,
    // which nothing here currently computes.
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

        const LocalLookupContext ctx{
            .sema_module = sema_mod_it->second,
            .sema_program = result.sema_program,
            .module_path = module_path,
            .diag = throwaway_diag,
            .tokens = &tokens,
            .program_result = &result,
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
            const auto start = t.location.column;
            const auto end = start + t.lexeme.size();
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
        if (chain_anchor) {
            chain_prefix_names = chain_prefix(tokens, *chain_anchor);
            if (!chain_prefix_names.empty()) {
                dot_triggered = true;
            } else if (!identifier_index && *chain_anchor >= 1 && tokens[*chain_anchor - 1].kind == TokenKind::Dot) {
                // Bare "receiver." with receiver itself unresolvable as a chain (e.g. after a
                // call/index) - chain_prefix can't reconstruct these; nothing to offer.
                return json::array();
            }
        }

        const auto enclosing = find_enclosing_function(mod_it->second, sema_mod_it->second, result.sema_program, module_path, throwaway_diag, tokens, line);

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
                return Resolution{.kind = Resolution::Kind::Symbol, .name = name, .module_path = module_path, .symbol = &sym_it->second};
            }
            return std::nullopt;
        };

        std::vector<json> items;

        if (dot_triggered) {
            const auto base = resolve_base_name(chain_prefix_names[0]);
            if (!base) return json::array();

            Container container = base->kind == Resolution::Kind::Symbol
                                       ? symbol_to_container(*base->symbol)
                                       : Container{.kind = Container::Kind::Type, .module_path = "", .type = base->type};

            for (size_t i = 1; i < chain_prefix_names.size() && container.kind != Container::Kind::None; ++i) {
                auto [res, next] = step(container, chain_prefix_names[i], result.sema_program);
                container = next;
            }

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
