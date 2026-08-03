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
        // '--noinit': skip synthesizing the '@init'-runner '_init', mirroring the
        // driver flag codegen honors.
        bool noinit = false;
        // Run the hosted entry-point validation (validate_hosted_main's rules).
        // Set by the driver for real BUILDS — not for '--emit-mir' (which
        // inspects arbitrary modules) and not under '--freestanding'.
        bool validate_entry = false;
        // '--freestanding': no libc. The runtime panic path must then reach the
        // kernel through raw syscalls rather than write()/exit(), or the
        // program fails to link against -nostdlib.
        bool freestanding = false;
        // Set under 'mirage test': synthesize the test-runner entry instead of the
        // ordinary one. Empty means the normal build; otherwise it is the resolved
        // 'core/testing' module path, whose '_run_tests' the entry calls.
        std::string testing_module_path;
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
        // Under 'mirage test' (Options::testing_module_path set): the '__mirage_test_info'
        // descriptor and 'core/testing''s '_run_tests', which the backend's entry glue
        // calls instead of 'main'. UINT32_MAX when this is an ordinary build.
        uint32_t test_info_global = UINT32_MAX;
        uint32_t test_runner_function = UINT32_MAX;
        bool ok = false;
    };

    auto generate(const ast::Program &ast_program, const sema::Program &sema_program,
                   DiagnosticEngine &diag, const Options &options = {}) -> Result;
}
