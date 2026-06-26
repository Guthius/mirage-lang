#pragma once

#include "diagnostic_engine.hpp"
#include "token.hpp"

#include <vector>

namespace lexer {
    auto Tokenize(std::string_view source, std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token>;
}
