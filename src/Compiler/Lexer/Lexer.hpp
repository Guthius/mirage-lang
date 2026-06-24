#pragma once

#include <Compiler/Diagnostics/DiagnosticEngine.hpp>
#include <Compiler/Token.hpp>

#include <vector>

namespace Lexer {
    auto Tokenize(std::string_view source, std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token>;
}
