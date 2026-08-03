#pragma once

// x86-64 code generation from MIR (stage 4, docs/backend.md): the TRIVIAL-regalloc
// pipeline. Every MIR value lives in a frame slot; every instruction loads its
// operands into fixed scratch registers, computes, and spills the result. Slow and
// enormous by design — and almost impossible to get wrong, which is the point: it
// validates ISel, frame layout, the encoder and the ELF writer end to end before a
// real allocator exists, and it stays forever as the triage tool
// ("--regalloc=trivial": if a bug reproduces here, it is not the allocator").

#include "elf_writer.hpp"
#include "mir.hpp"

#include <string>
#include <vector>

namespace backend_x86 {
    struct Result {
        bool ok = false;
        std::vector<std::string> errors;
        elf::Object object;
    };

    [[nodiscard]] auto generate(const mir::Module &module) -> Result;
}
