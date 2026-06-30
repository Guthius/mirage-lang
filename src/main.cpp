#include "compiler/ast.hpp"
#include "compiler/lexer.hpp"
#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"

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

    auto res = sema::check_program(program, diagnostics);

    return 0;
}
