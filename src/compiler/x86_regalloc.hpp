#pragma once

// Linear-scan register allocation over MIR values (stage 6, docs/backend.md).
//
// There is no separate machine IR: MIR is already SSA-shaped (every value has one
// defining instruction, block parameters instead of phi), so MIR values ARE the
// virtual registers, and this pass assigns each one either a physical register or
// an 8-byte frame area for its whole lifetime. The emission templates in
// backend_x86.cpp read operands from wherever the assignment put them; the trivial
// allocator is the degenerate assignment in which everything is spilled, which is
// what keeps '--regalloc=trivial' alive as the standing triage tool with the SAME
// emission engine ("if it also misbehaves under trivial, the bug is not in the
// allocator").
//
// Fixed-register constraints are modelled as KILL RANGES: positions at which a
// physical register is owned by an instruction template (div/idiv write rdx:rax,
// variable shifts need cl, a call clobbers every caller-saved register and all
// XMMs, an asm block clobbers what sema said it does). An interval may not be
// assigned a register whose kill range it overlaps — with one deliberate
// exception: an interval that is live ACROSS calls may take a caller-saved
// register with 'save_around_calls', and the emitter then stores it to its save
// area before each call it crosses and reloads it after. That is this allocator's
// form of interval splitting around calls: the value lives in a register between
// calls and in memory across them.
//
// Intervals are conservative single ranges [start, end] over the union of a
// value's live positions. Holes are not modelled; the cost is register pressure,
// never correctness (a register "occupied" through a hole is simply not handed
// out to anyone else, and templates only write registers they own at that point).

#include "mir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace x86ra {
    // One register namespace across both classes: 0-15 are the GPRs in hardware
    // encoding order (matching x86::Reg), 16-31 are XMM0-15. RSP(4)/RBP(5) are
    // reserved and never allocated; everything else — 14 GPRs and 16 XMMs — is.
    using PhysReg = uint8_t;
    inline constexpr PhysReg FIRST_XMM = 16;
    inline constexpr PhysReg NO_PHYS = 0xFF;

    [[nodiscard]] inline auto is_xmm(const PhysReg reg) -> bool { return reg >= FIRST_XMM; }
    // Callee-saved GPRs under System V: rbx, r12-r15. Every XMM is caller-saved.
    [[nodiscard]] inline auto is_callee_saved(const PhysReg reg) -> bool {
        return reg == 3 || (reg >= 12 && reg <= 15);
    }

    enum class Mode : uint8_t { Trivial, Linear };

    // Where a value lives for its whole lifetime. Spilled values own an 8-byte
    // area in the frame's spill region (spill_index); save_around_calls values
    // additionally own the area their register is parked in across each call
    // they are live over (save_index). Both index the same dense area table.
    struct Assignment {
        bool spilled = true;
        PhysReg reg = NO_PHYS;
        bool save_around_calls = false;
        uint32_t spill_index = UINT32_MAX;
        uint32_t save_index = UINT32_MAX;
    };

    // Instruction 'index' (global order across the function's blocks, in layout
    // order) owns positions [4i, 4i+3]:
    //   E = 4i     before anything; kill ranges start here
    //   U = 4i+1   operand reads; results of straight-line ops ALSO define here
    //              (the early-def rule: a result never shares a register with an
    //              operand, which is what makes two-address templates safe)
    //   D = 4i+2   straight-line kill ranges end here; call clobbers live here
    //   L = 4i+3   late writes: block-argument stores at a jump, call results
    inline constexpr auto pos_e(const uint32_t index) -> uint32_t { return index * 4; }
    inline constexpr auto pos_u(const uint32_t index) -> uint32_t { return index * 4 + 1; }
    inline constexpr auto pos_d(const uint32_t index) -> uint32_t { return index * 4 + 2; }
    inline constexpr auto pos_l(const uint32_t index) -> uint32_t { return index * 4 + 3; }

    // Conservative single-range interval, positions inclusive.
    struct Interval {
        mir::ValueId value = mir::NO_VALUE;
        uint32_t start = 0;
        uint32_t end = 0;
        bool is_float = false;
        bool crosses_call = false;
    };

    struct Result {
        std::vector<Assignment> values;   // indexed by ValueId
        std::vector<Interval> intervals;  // sorted by start; emission sweeps these
        uint32_t spill_area_count = 0;    // 8-byte areas the frame must provide
        uint16_t used_callee_saved = 0;   // GPR bitmask for prologue saves
        // Machine-verifier findings (docs/backend.md validation #4): after
        // assignment, every same-register interval pair is re-checked for
        // overlap and every interval against its register's kill ranges. Any
        // entry here is an allocator bug and must abort the compile.
        std::vector<std::string> errors;
    };

    [[nodiscard]] auto allocate(const mir::Module &module, const mir::Function &fn,
                                 Mode mode) -> Result;
}
