#include "Compiler/ast.hpp"
#include "Compiler/lexer.hpp"
#include "Compiler/module_resolver.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
    auto ReadFile(const std::string &path) -> std::string {
        std::ifstream ifs(path);
        if (!ifs) {
            std::cerr << "cannot open file '" << path << "'\n";
            std::exit(1);
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }
}

auto main(const int argc, char *argv[]) -> int {
    if (argc != 2) {
        return 1;
    }

    const auto filename = std::string(argv[1]);

    DiagnosticEngine diagnostics;

    const auto program = ast::resolve(filename, diagnostics);
    if (!program.ok) {
        return 1;
    }

    return 0;
}
