#pragma once

#include "diagnostic_engine.hpp"
#include "module_resolver.hpp"
#include "sema.hpp"

#include <memory>

namespace llvm {
    class LLVMContext;
    class Module;
}

namespace codegen {
    struct Options {
        bool freestanding = false;
        // Suppresses generation (and, in hosted builds, the '_start' call) of the synthesized
        // '@init'-runner '_init', even if the program declares '@init' functions. Those
        // functions still compile normally and remain individually callable — only the
        // automatic invocation is skipped; the user must call each one manually if needed.
        bool noinit = false;
    };

    auto generate(
        const ast::Program &ast_program,
        const sema::Program &sema_program,
        DiagnosticEngine &diag,
        const Options &options = {}) -> std::unique_ptr<llvm::Module>;
}
