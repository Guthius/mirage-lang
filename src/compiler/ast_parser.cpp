#include "ast_parser.hpp"

#include "ast.hpp"
#include "diagnostic_engine.hpp"

#include <format>

namespace ast {
    namespace {
        class ParserImpl : public Parser {
            std::span<Token> tokens_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;

          public:
            ParserImpl(const std::span<Token> tokens, DiagnosticEngine &diagnostics)
                : tokens_(tokens), diagnostics_(diagnostics) {
            }

            auto diagnostics() -> DiagnosticEngine & override {
                return diagnostics_;
            }

            [[nodiscard]] auto current() const -> const Token & override {
                return tokens_[pos_];
            }

            [[nodiscard]] auto current_lexeme() const -> std::string_view override {
                return current().lexeme;
            }

            [[nodiscard]] auto current_location() const -> SourceLocation override {
                return current().location;
            }

            [[nodiscard]] auto at_end() const -> bool override {
                return current().kind == TokenKind::Eof;
            }

            [[nodiscard]] auto peek_at(const size_t offset) const -> const Token & override {
                if (pos_ + offset < tokens_.size()) {
                    return tokens_[pos_ + offset];
                }
                return tokens_.back();
            }

            [[nodiscard]] auto peek() const -> const Token & override {
                return peek_at(1);
            }

            [[nodiscard]] auto peek_next() const -> const Token & override {
                return peek_at(2);
            }

            auto advance() -> const Token & override {
                auto &tok = tokens_[pos_];
                if (!at_end()) {
                    ++pos_;
                }
                return tok;
            }

            [[nodiscard]] auto check(const TokenKind kind) const -> bool override {
                return current().kind == kind;
            }

            // Unlike peek_at, an offset past the end answers false rather than reporting the
            // Eof token's kind — "is the token at N a '='" should be no when there is no such
            // token, not accidentally yes for a query about Eof.
            [[nodiscard]] auto check_at(const size_t offset, const TokenKind kind) const -> bool override {
                return pos_ + offset < tokens_.size() && tokens_[pos_ + offset].kind == kind;
            }

            [[nodiscard]] auto check_next(const TokenKind kind) const -> bool override {
                return check_at(1, kind);
            }

            auto match(const TokenKind kind) -> bool override {
                if (check(kind)) {
                    advance();
                    return true;
                }
                return false;
            }

            auto match_identifier(const std::string_view lexeme) -> bool override {
                if (check(TokenKind::Identifier)) {
                    return advance().lexeme == lexeme;
                }
                return false;
            }

            auto expect(const TokenKind kind, std::string_view message) -> const Token override {
                if (check(kind)) {
                    return advance();
                }

                report_error(current_location(), std::format("expected {}, got '{}'", message, current_lexeme()));

                return current();
            }

            auto expect_identifier() -> const std::string & override {
                if (check(TokenKind::Identifier)) {
                    return advance().lexeme;
                }

                report_error(current_location(), std::format("expected identifier, got '{}'", current_lexeme()));

                return current().lexeme;
            }

            auto report_error(const SourceLocation location, std::string message) -> void override {
                diagnostics_.report_error(DiagnosticStage::Parser, location, std::move(message));
            }

            [[nodiscard]] auto has_reached_max_errors() const -> bool override {
                return diagnostics_.has_reached_max_errors();
            }
        };
    }

    auto parse(const std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl> {
        ParserImpl parser(tokens, diagnostics);

        std::vector<Decl> decls;

        while (true) {
            skip_semicolons(parser);
            if (parser.at_end()) {
                break;
            }

            if (auto decl = parse_decl(parser, true); decl.has_value()) {
                decls.push_back(std::move(*decl));
            }

            if (diagnostics.has_reached_max_errors()) {
                break;
            }
        }

        return decls;
    }
}
