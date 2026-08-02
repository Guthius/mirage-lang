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
        // Target triple and DataLayout string, computed by the driver BEFORE generation and
        // installed on the module up front — so every implicit alignment/constant fold made
        // while BUILDING the IR uses the real target's layout, not LLVM's default. Empty
        // means "leave the module's defaults" (unit tests, --dump-ast paths).
        std::string target_triple;
        std::string data_layout;
        // Under 'mirage test': the canonical directory the reserved 'core/testing' module
        // resolved to. Codegen reaches that module's declarations by (module path,
        // declaration name) -- the same mangled reference every cross-module symbol already
        // uses -- rather than through identifier resolution, which cannot see a forced
        // module by design. Empty in every other action, which is also what turns the whole
        // test-mode code path off.
        std::string testing_module_path;
    };

    // The generated module together with the LLVMContext that owns every type and
    // constant in it. The context member is declared BEFORE the module so member
    // destruction (reverse order) tears the module down first — a module must never
    // outlive its context. Replaces an internal retain-forever list that parked every
    // context for the life of the process.
    struct GeneratedModule {
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
    };

    auto generate(
        const ast::Program &ast_program,
        const sema::Program &sema_program,
        DiagnosticEngine &diag,
        const Options &options = {}) -> GeneratedModule;
}
