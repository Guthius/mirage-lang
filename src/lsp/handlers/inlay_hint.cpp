#include "inlay_hint.hpp"

#include "../type_printer.hpp"
#include "ast_walker.hpp"
#include "common.hpp"

#include "compiler/lexer.hpp"
#include "compiler/sema.hpp"

#include <type_traits>

namespace lsp::handlers {
    namespace {
        using json = nlohmann::json;

        // LSP InlayHintKind protocol constants.
        enum class HintKind : int { Type = 1, Parameter = 2 };

        auto position_json(const size_t line, const size_t column) -> json {
            return {{"line", line == 0 ? 0 : line - 1}, {"character", column == 0 ? 0 : column - 1}};
        }

        auto hint_json(const size_t line, const size_t column, std::string label, const HintKind kind,
                       const bool pad_left, const bool pad_right) -> json {
            return {
                {"position", position_json(line, column)},
                {"label", std::move(label)},
                {"kind", static_cast<int>(kind)},
                {"paddingLeft", pad_left},
                {"paddingRight", pad_right},
            };
        }

        // Locates the identifier token immediately following the 'const'/'mut' keyword token at
        // `keyword_location` - i.e. the variable's own name token - so a type hint can be placed
        // right after it rather than after the keyword (VarDeclStmt::location is the keyword's
        // own position, not the name's - see parse_var_decl_stmt in ast.cpp).
        auto name_token_after(const std::vector<Token> &tokens, const SourceLocation &keyword_location) -> const Token * {
            const auto idx = token_at(tokens, keyword_location.line, keyword_location.column);
            if (!idx || *idx + 1 >= tokens.size()) return nullptr;
            return &tokens[*idx + 1];
        }

        // Locates each name's own token in a 'const a, b, _ := f()' group declaration, in
        // source order - VarDeclGroupStmt::location is the 'const'/'mut' keyword's own position
        // (same convention as VarDeclStmt), so each name is found by scanning forward for
        // Identifier tokens (skipping the ',' separators) up to the ':='.
        auto group_decl_name_tokens(const std::vector<Token> &tokens, const SourceLocation &keyword_location) -> std::vector<const Token *> {
            std::vector<const Token *> names;
            const auto idx = token_at(tokens, keyword_location.line, keyword_location.column);
            if (!idx) return names;
            for (size_t i = *idx + 1; i < tokens.size() && tokens[i].kind != TokenKind::ColonEqual; ++i) {
                if (tokens[i].kind == TokenKind::Identifier) names.push_back(&tokens[i]);
            }
            return names;
        }

        // True start-of-argument token indices for a call, given its own opening '(' token
        // index and matching close (from build_bracket_index) - splits on top-level (this
        // call's own nesting level) commas. Needed because expression nodes in this AST store
        // one DEFINING token location per node - e.g. a nested CallExpr's is its own '('
        // position, a MemberExpr's is its '.' position (see parse_postfix's per-iteration
        // 'location' capture, ast.cpp) - never a true span start, so
        // sema::get_expr_location(arg) is unreliable as "where does this argument begin" for
        // anything but a single-token argument (a bare literal/identifier). Reconstructing the
        // boundary from the token stream instead sidesteps that entirely.
        auto call_arg_start_tokens(const std::vector<Token> &tokens, const size_t open_idx, const size_t close_idx) -> std::vector<size_t> {
            std::vector<size_t> starts;
            if (open_idx + 1 >= close_idx) return starts;
            starts.push_back(open_idx + 1);

            int depth = 0;
            for (size_t i = open_idx + 1; i < close_idx; ++i) {
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
                case TokenKind::Comma:
                    if (depth == 0 && i + 1 < close_idx) starts.push_back(i + 1);
                    break;
                default:
                    break;
                }
            }
            return starts;
        }

        // Suppresses a parameter-name hint when the argument already spells the parameter's own
        // name (e.g. 'fn(width: width)') - standard noise-reduction convention other editors'
        // inlay hints use, since the hint would be pure repetition.
        auto is_redundant_arg_name(const ast::Expr &arg, const std::string &param_name) -> bool {
            const auto *ident = std::get_if<ast::IdentExpr>(&arg);
            return ident && ident->name == param_name;
        }

        // Param names for a resolved callee, in call-argument order - already self-excluded for
        // a method (sema::MethodInfo::decl->params/ast::ImplDecl::Function::params never
        // include 'self'). Empty if `res` isn't a function/method/ext-fn/macro with a known
        // declaration (e.g. a call through a function-pointer-typed variable, or an unresolved
        // symbol) - callers treat that as "nothing to hint," not an error.
        auto callee_param_names(const Resolution &res) -> std::vector<std::string> {
            if (res.kind == Resolution::Kind::Method) {
                std::vector<std::string> names;
                if (res.method && res.method->decl) {
                    for (const auto &p : res.method->decl->params) names.push_back(p.name);
                }
                return names;
            }
            if (res.kind != Resolution::Kind::Symbol || !res.symbol) return {};

            return std::visit(
                [&]<typename T>(const T &sym) -> std::vector<std::string> {
                    using S = std::decay_t<T>;
                    std::vector<std::string> names;
                    if constexpr (std::is_same_v<S, sema::FunctionSymbol> || std::is_same_v<S, sema::ExtFunctionSymbol> ||
                                  std::is_same_v<S, sema::MacroSymbol>) {
                        if (sym.decl) {
                            for (const auto &p : sym.decl->params) names.push_back(p.name);
                        }
                    }
                    return names;
                },
                *res.symbol);
        }
    }

    auto handle_inlay_hint(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                          const size_t start_line, const size_t end_line) -> nlohmann::json {
        json hints = json::array();

        DiagnosticEngine throwaway_diag(*result.source_manager);
        const auto source_file = result.source_manager->load(path, throwaway_diag);
        if (source_file.text.empty()) return hints;
        const auto tokens = lexer::tokenize(source_file.text, source_file.filename, throwaway_diag);
        const auto bracket_index = build_bracket_index(tokens);

        const auto mod_it = result.ast_program.modules.find(module_path);
        const auto sema_mod_it = result.sema_program.modules.find(module_path);
        if (mod_it == result.ast_program.modules.end() || sema_mod_it == result.sema_program.modules.end()) {
            return hints;
        }

        const LocalLookupContext ctx{
            .sema_module = sema_mod_it->second,
            .sema_program = result.sema_program,
            .module_path = module_path,
            .diag = throwaway_diag,
            .tokens = &tokens,
            .program_result = &result,
        };

        const auto in_range = [&](const size_t line) { return line >= start_line && line <= end_line; };

        AstVisitor visitor;

        // Type hints: after a ':='-inferred VarDeclStmt's name, and after each name in a
        // multi-return VarDeclGroupStmt ('const a, b := f()') - the latter's per-name type
        // isn't recorded anywhere by sema (only computed into a local, per-statement, by
        // check_group_call_returns), so resolve_group_decl_name_type() re-resolves it by
        // re-resolving the initializer call's callee (see common.cpp).
        visitor.on_stmt = [&](const ast::Stmt &stmt) {
            if (const auto *var = std::get_if<ast::VarDeclStmt>(&stmt)) {
                if (var->type || !var->init || !in_range(var->location.line)) return;

                const auto *name_tok = name_token_after(tokens, var->location);
                if (!name_tok) return;

                const auto type = resolve_var_decl_type(*var, ctx);
                if (type.kind == sema::TypeKind::Invalid) return;

                const auto hint_column = name_tok->location.column + name_tok->lexeme.size();
                hints.push_back(hint_json(name_tok->location.line, hint_column,
                                          ": " + type_to_string(type, result.sema_program, module_path),
                                          HintKind::Type, false, false));
                return;
            }

            if (const auto *group = std::get_if<ast::VarDeclGroupStmt>(&stmt)) {
                if (!in_range(group->location.line)) return;

                const auto name_tokens = group_decl_name_tokens(tokens, group->location);
                for (size_t i = 0; i < group->names.size() && i < name_tokens.size(); ++i) {
                    if (group->names[i].empty() || group->names[i] == "_") continue;

                    const auto type = resolve_group_decl_name_type(*group, i, ctx);
                    if (type.kind == sema::TypeKind::Void) continue; // resolution failed - see its own doc comment

                    const auto *name_tok = name_tokens[i];
                    const auto hint_column = name_tok->location.column + name_tok->lexeme.size();
                    hints.push_back(hint_json(name_tok->location.line, hint_column,
                                              ": " + type_to_string(type, result.sema_program, module_path),
                                              HintKind::Type, false, false));
                }
            }
        };

        // Parameter-name hints: resolve the callee (reusing resolve_at_tokens, which already
        // knows how to walk a plain call, a method call, and a module-qualified free-function
        // call uniformly - see common.cpp's chain/Container machinery) at its own name token's
        // position, then zip its declared parameter names against the call's positional
        // arguments' TOKEN-derived start positions (not their ast::Expr's own location - see
        // call_arg_start_tokens's doc comment for why that would misplace the hint for any
        // compound argument).
        visitor.on_expr = [&](const ast::Expr &expr) {
            const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&expr);
            if (!call || !in_range((*call)->location.line)) return;

            const auto callee_loc = callee_name_location((*call)->callee, tokens);
            if (!callee_loc) return;

            const auto callee_idx = token_at(tokens, callee_loc->line, callee_loc->column);
            if (!callee_idx || *callee_idx + 1 >= tokens.size() || tokens[*callee_idx + 1].kind != TokenKind::LParen) return;
            const auto open_idx = *callee_idx + 1;
            const auto close_it = bracket_index.find(open_idx);
            if (close_it == bracket_index.end()) return;

            const auto callee_res = resolve_at_tokens(result, module_path, tokens, callee_loc->line, callee_loc->column);
            const auto param_names = callee_param_names(callee_res);
            if (param_names.empty()) return;

            const auto arg_starts = call_arg_start_tokens(tokens, open_idx, close_it->second);
            const auto &args = (*call)->args;
            for (size_t i = 0; i < args.size() && i < param_names.size() && i < arg_starts.size(); ++i) {
                if (param_names[i].empty() || is_redundant_arg_name(args[i], param_names[i])) continue;
                const auto &arg_loc = tokens[arg_starts[i]].location;
                if (!in_range(arg_loc.line)) continue;
                hints.push_back(hint_json(arg_loc.line, arg_loc.column, param_names[i] + ":", HintKind::Parameter, false, true));
            }
        };

        walk_module_bodies(mod_it->second, visitor);

        return hints;
    }
}
