#include "compiler/ast.hpp"
#include "compiler/codegen.hpp"
#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

namespace {
    struct Options {
        bool emit_ir;
        bool freestanding;
        std::string filename;
    };

    auto parse_options(const int argc, char *argv[]) -> Options {
        Options options{};

        for (int i = 1; i < argc; ++i) {
            const auto arg = std::string(argv[i]);
            if (arg == "--emit-ir") {
                options.emit_ir = true;
            } else if (arg == "--freestanding") {
                options.freestanding = true;
            } else if (options.filename.empty()) {
                options.filename = arg;
            } else {
                break;
            }
        }

        return options;
    }
}

auto main(const int argc, char *argv[]) -> int {
    if (argc < 2) {
        return 1;
    }

    const auto options = parse_options(argc, argv);
    if (options.filename.empty()) {
        return 1;
    }

    SourceManager source_manager;
    DiagnosticEngine diag(source_manager);

    const auto ast = ast::resolve(options.filename, source_manager, diag);
    if (!ast.ok) {
        return 1;
    }

    const auto sema = sema::check_program(ast, diag);
    if (!sema.ok) {
        return 1;
    }

    const auto llvm_module = codegen::generate(ast, sema, diag, {.freestanding = options.freestanding});
    if (!llvm_module || diag.has_errors()) {
        return 1;
    }

    if (options.emit_ir) {
        llvm_module->print(llvm::outs(), nullptr);
    }

    return 0;
}
