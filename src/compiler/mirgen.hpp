#pragma once

// Lowers a type-checked sema::Program to Mirage IR (mir::Module) — stage 2 of the LLVM
// replacement described in docs/backend.md.
//
// This is the counterpart of codegen.cpp, which lowers the same input to LLVM IR, and the
// two will coexist behind '--backend' until the native path is proven. Nothing here reads
// or links LLVM.
//
// COVERAGE. mirgen is being grown construct by construct rather than landed whole: a
// 7,300-line emitter ported in one step is unreviewable, and the plan's own sequencing says
// to validate by reading MIR before any backend consumes it. Anything not yet lowered is
// reported as a codegen diagnostic naming the construct — never silently skipped, and never
// a crash, so '--emit-mir' over the corpus is a truthful coverage report rather than a
// minefield. See Result::unsupported below.

#include "diagnostic_engine.hpp"
#include "mir.hpp"
#include "module_resolver.hpp"
#include "sema.hpp"

#include <set>
#include <string>

namespace mirgen {
    struct Options {
        // Byte width of a pointer on the target, mirroring sema::Options::pointer_size.
        // Everything else about the target is a backend concern; MIR is target-independent
        // apart from this one number, which decides Ty::Ptr's width and therefore every
        // 'usize' the front end already laid types out with.
        uint32_t pointer_bits = 64;
    };

    struct Result {
        mir::Module module;
        // Constructs encountered that mirgen cannot lower yet, deduplicated and sorted so
        // the set is a stable, diffable coverage report. Empty means the whole program was
        // lowered.
        //
        // Populated alongside a diagnostic for each occurrence — this is the summary, not
        // the error channel.
        std::set<std::string> unsupported;
        bool ok = false;
    };

    auto generate(const ast::Program &ast_program, const sema::Program &sema_program,
                   DiagnosticEngine &diag, const Options &options = {}) -> Result;
}
