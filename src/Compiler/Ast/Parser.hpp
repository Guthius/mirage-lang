#pragma once

#include <Compiler/Token.hpp>

namespace Ast {
    class Parser {
      public:
        virtual ~Parser() = default;

        virtual auto Current() const -> const Token & = 0;
        virtual auto CurrentLexeme() const -> std::string_view = 0;
        virtual auto CurrentLocation() const -> SourceLocation = 0;
        virtual auto AtEnd() const -> bool = 0;
        virtual auto Peek() const -> const Token & = 0;
        virtual auto Advance() -> const Token & = 0;
        virtual auto Check(TokenKind kind) const -> bool = 0;
        virtual auto Match(TokenKind kind) -> bool = 0;
        virtual auto Expect(TokenKind kind, std::string_view message) -> const Token = 0;
        virtual auto ExpectIdentifier(std::string_view identifier) -> const Token & = 0;
        virtual auto ReportError(SourceLocation location, std::string message) -> void = 0;
    };
}
