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
    };

    auto generate(
        const ast::Program &ast_program,
        const sema::Program &sema_program,
        DiagnosticEngine &diag,
        const Options &options = {}) -> std::unique_ptr<llvm::Module>;
}
