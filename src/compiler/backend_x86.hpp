#pragma once

// x86-64 code generation from MIR (stages 4-6, docs/backend.md). One emission
// engine reads every operand from wherever the register allocator put it; the
// allocator is selected per call:
//
//  - RegAlloc::Linear (the default): the linear-scan allocator in
//    x86_regalloc.cpp, with its machine-level interference verifier.
//  - RegAlloc::Trivial: every value spilled to a frame area — the stage-4/5
//    discipline, kept forever as the standing triage tool ("if it also
//    misbehaves under trivial, the bug is not in the allocator").

#include "elf_writer.hpp"
#include "mir.hpp"

#include <string>
#include <vector>

namespace backend_x86 {
    enum class RegAlloc : uint8_t { Trivial, Linear };

    struct Result {
        bool ok = false;
        std::vector<std::string> errors;
        elf::Object object;
    };

    // 'test_info'/'test_runner' (both UINT32_MAX for an ordinary build) select the
    // 'mirage test' entry: the glue calls '_run_tests(&__mirage_test_info)' instead of
    // 'main', which is compiled like any other function and never invoked.
    [[nodiscard]] auto generate(const mir::Module &module,
                                 uint32_t test_info = UINT32_MAX,
                                 uint32_t test_runner = UINT32_MAX,
                                 RegAlloc regalloc = RegAlloc::Linear) -> Result;
}
