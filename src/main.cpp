#include "Compiler/ast.hpp"
#include "Compiler/lexer.hpp"
#include "Compiler/module_resolver.hpp"
#include "Compiler/source_manager.hpp"

#include <string>

auto main(const int argc, char *argv[]) -> int {
    if (argc != 2) {
        return 1;
    }

    const auto filename = std::string(argv[1]);

    SourceManager source_manager;
    DiagnosticEngine diagnostics(source_manager);

    const auto program = ast::resolve(filename, source_manager, diagnostics);
    if (!program.ok) {
        return 1;
    }

    return 0;
}
