#pragma once

// Standalone wasm code generation from MIR (stage 7, docs/backend.md): emits the
// FINAL .wasm module directly — whole-program compilation means there is nothing
// to link, so no relocatable object format, no linking section, no wasm-ld. The
// relocatable shape emscripten needs is stage 8, deliberately built second.
//
// Shape of the output:
//  - Control flow is the DISPATCH LOOP: each function body is
//    'loop { block×N { br_table $state } ... }', one arm per MIR block, every
//    branch assigning $state. Works for any CFG including irreducible ones, and
//    keeps CFG structuring out of the picture while the codegen itself is new —
//    the Relooper is stage 9, an optimization.
//  - Every MIR value is a typed wasm local; aggregates live in linear memory on a
//    SHADOW STACK (a mutable '__stack_pointer' global), because wasm's operand
//    stack is not addressable.
//  - A function "pointer" is a funcref TABLE INDEX (slot i+1 for function i; 0 is
//    reserved so a null pointer traps on call_indirect). Global-initializer
//    relocations resolve at layout time — function targets to table indices,
//    global targets to absolute memory addresses.
//  - Imports come from '@import' bindings on 'ext fn' (default module "env",
//    decision D4); every External-linkage definition is exported ('@export');
//    the entry glue is an exported "main" wrapping the user's main (or the test
//    runner under 'mirage test').

#include "mir.hpp"

#include <string>
#include <vector>

namespace backend_wasm {
    struct Result {
        bool ok = false;
        std::vector<std::string> errors;
        std::vector<uint8_t> bytes; // the serialized .wasm module
    };

    // Same contract as backend_x86::generate: 'test_info'/'test_runner' (both
    // UINT32_MAX for an ordinary build) select the 'mirage test' entry.
    [[nodiscard]] auto generate(const mir::Module &module,
                                 uint32_t test_info = UINT32_MAX,
                                 uint32_t test_runner = UINT32_MAX) -> Result;
}
