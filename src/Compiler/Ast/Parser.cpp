#include "../Ast.hpp"

#include <format>

namespace Ast {
    namespace {
        class ParserImpl : public Parser {
            std::span<Token> tokens_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;

          public:
            ParserImpl(const std::span<Token> tokens, DiagnosticEngine &diagnostics)
                : tokens_(tokens), diagnostics_(diagnostics) {
            }

            auto Current() const -> const Token & override {
                return tokens_[pos_];
            }

            auto CurrentLexeme() const -> std::string_view override {
                return Current().Lexeme;
            }

            auto CurrentLocation() const -> SourceLocation override {
                return Current().Location;
            }

            auto AtEnd() const -> bool override {
                return Current().Kind == TokenKind::eof;
            }

            auto Peek() const -> const Token & override {
                if (pos_ + 1 < tokens_.size()) {
                    return tokens_[pos_ + 1];
                }
                return tokens_.back();
            }

            auto Advance() -> const Token & override {
                auto &tok = tokens_[pos_];
                if (!AtEnd()) {
                    ++pos_;
                }
                return tok;
            }

            auto Check(const TokenKind kind) const -> bool override {
                return Current().Kind == kind;
            }

            auto Match(const TokenKind kind) -> bool override {
                if (Check(kind)) {
                    Advance();
                    return true;
                }
                return false;
            }

            auto Expect(const TokenKind kind, std::string_view message) -> const Token override {
                if (Check(kind)) {
                    return Advance();
                }

                ReportError(CurrentLocation(), std::format("expected {}, got '{}'", message, CurrentLexeme()));

                return Current();
            }

            auto ExpectIdentifier(std::string_view identifier) -> const Token & override {
                if (Check(TokenKind::identifier) || CurrentLexeme() == identifier) {
                    return Advance();
                }

                ReportError(CurrentLocation(), std::format("expected '{}', got '{}'", identifier, CurrentLexeme()));

                return Current();
            }

            auto ReportError(SourceLocation location, std::string message) -> void override {
                diagnostics_.ReportError(DiagnosticStage::Parser, location, std::move(message));
            }
        };
    }

    auto Parse(std::span<Token> tokens, DiagnosticEngine &diagnostics) -> std::vector<Decl> {
        ParserImpl parser(tokens, diagnostics);

        std::vector<Decl> decls;

        while (!parser.AtEnd()) {
            if (auto decl = ParseDecl(parser, true); decl.has_value()) {
                decls.push_back(std::move(*decl));
            }

            if (diagnostics.HasReachedMaxErrors()) {
                break;
            }
        }

        return decls;
    }
}
