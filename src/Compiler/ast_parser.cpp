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

            [[nodiscard]] auto peek() const -> const Token & override {
                if (pos_ + 1 < tokens_.size()) {
                    return tokens_[pos_ + 1];
                }
                return tokens_.back();
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

            auto match(const TokenKind kind) -> bool override {
                if (check(kind)) {
                    advance();
                    return true;
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

            auto expect_identifier(std::string_view identifier) -> const Token & override {
                if (check(TokenKind::Identifier) || current_lexeme() == identifier) {
                    return advance();
                }

                report_error(current_location(), std::format("expected '{}', got '{}'", identifier, current_lexeme()));

                return current();
            }

            auto report_error(const SourceLocation location, std::string message) -> void override {
                diagnostics_.report_error(DiagnosticStage::Parser, location, std::move(message));
            }
        };
    }

    auto parse(const std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl> {
        ParserImpl parser(tokens, diagnostics);

        std::vector<Decl> decls;

        while (!parser.at_end()) {
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
