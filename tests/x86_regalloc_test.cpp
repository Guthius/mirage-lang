// Unit tests for the linear-scan register allocator (stage 6, docs/backend.md).
// These pin the PROPERTIES the emission templates in backend_x86.cpp rely on —
// fixed-register kill ranges respected, call-crossing values kept out of
// unprotected caller-saved registers, the trivial mode spilling everything —
// rather than exact register choices, which are a preference order away from
// changing. The machine verifier runs inside allocate() for every linear case, so
// each check here is also a check that the verifier found nothing.

#include "compiler/mir.hpp"
#include "compiler/x86_regalloc.hpp"

#include <cstdio>
#include <initializer_list>

namespace {
    int failures = 0;

    void expect(const bool ok, const char *what) {
        if (ok) {
            std::printf("ok: %s\n", what);
        } else {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    }

    constexpr x86ra::PhysReg RAX = 0, RCX = 1, RDX = 2;

    auto assignment_of(const x86ra::Result &result, const mir::ValueId value)
        -> const x86ra::Assignment & {
        return result.values[value];
    }

    auto in_any_reg(const x86ra::Result &result, const mir::ValueId value,
                     std::initializer_list<x86ra::PhysReg> regs) -> bool {
        const auto &assignment = assignment_of(result, value);
        if (assignment.spilled) return false;
        for (const auto reg : regs) {
            if (assignment.reg == reg) return true;
        }
        return false;
    }
}

int main() {
    using mir::Ty;
    using mir::Op;

    // ---- straight-line arithmetic: everything fits in registers -------------
    {
        mir::Module module;
        module.functions.push_back({});
        auto &fn = module.functions[0];
        fn.name = "straight";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::I64, Ty::I64}, .result = Ty::I64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::I64);
        const auto b = builder.add_block_param(entry, Ty::I64);
        builder.set_insert_point(entry);
        const auto sum = builder.binary(Op::Add, Ty::I64, a, b);
        const auto twice = builder.binary(Op::Add, Ty::I64, sum, sum);
        builder.ret(twice);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "straight-line: verifier clean");
        expect(!assignment_of(result, sum).spilled && !assignment_of(result, twice).spilled,
               "straight-line: results in registers");
        expect(result.spill_area_count == 0, "straight-line: nothing spilled");

        const auto trivial = x86ra::allocate(module, fn, x86ra::Mode::Trivial);
        bool all_spilled = true;
        for (const auto &interval : trivial.intervals) {
            all_spilled &= assignment_of(trivial, interval.value).spilled;
        }
        expect(all_spilled, "trivial mode: every value spilled");
    }

    // ---- division: the divisor and the result avoid RAX/RDX -----------------
    {
        mir::Module module;
        module.functions.push_back({});
        auto &fn = module.functions[0];
        fn.name = "divide";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::I64, Ty::I64}, .result = Ty::I64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::I64);
        const auto b = builder.add_block_param(entry, Ty::I64);
        builder.set_insert_point(entry);
        const auto quotient = builder.binary(Op::UDiv, Ty::I64, a, b);
        // Keep both operands alive PAST the div so their intervals overlap its
        // kill range rather than ending at it.
        const auto keep = builder.binary(Op::Add, Ty::I64, a, b);
        const auto merged = builder.binary(Op::Add, Ty::I64, quotient, keep);
        builder.ret(merged);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "division: verifier clean");
        expect(!in_any_reg(result, a, {RAX, RDX}) && !in_any_reg(result, b, {RAX, RDX}),
               "division: operands kept out of RAX/RDX");
        expect(!in_any_reg(result, quotient, {RAX, RDX}),
               "division: result kept out of RAX/RDX");
    }

    // ---- shifts: the operands avoid RCX -------------------------------------
    {
        mir::Module module;
        module.functions.push_back({});
        auto &fn = module.functions[0];
        fn.name = "shift";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::I64, Ty::I64}, .result = Ty::I64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::I64);
        const auto b = builder.add_block_param(entry, Ty::I64);
        builder.set_insert_point(entry);
        const auto shifted = builder.binary(Op::Shl, Ty::I64, a, b);
        const auto keep = builder.binary(Op::Add, Ty::I64, a, b);
        const auto merged = builder.binary(Op::Add, Ty::I64, shifted, keep);
        builder.ret(merged);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "shift: verifier clean");
        expect(!in_any_reg(result, a, {RCX}) && !in_any_reg(result, b, {RCX}) &&
               !in_any_reg(result, shifted, {RCX}),
               "shift: operands and result kept out of RCX");
    }

    // ---- call-crossing values: callee-saved, save-around, or spilled --------
    {
        mir::Module module;
        module.functions.push_back({});   // the caller under test
        module.functions.push_back({});   // a callee to cross
        auto &callee = module.functions[1];
        callee.name = "callee";
        callee.signature = module.intern_signature({.result = Ty::Void});

        auto &fn = module.functions[0];
        fn.name = "crossing";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::I64}, .result = Ty::I64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::I64);
        builder.set_insert_point(entry);
        const auto live = builder.binary(Op::Add, Ty::I64, a, a);
        builder.call(1, Ty::Void, {});
        const auto after = builder.binary(Op::Add, Ty::I64, live, live);
        builder.ret(after);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "crossing: verifier clean");
        const auto &assignment = assignment_of(result, live);
        const bool protected_somehow = assignment.spilled ||
            x86ra::is_callee_saved(assignment.reg) || assignment.save_around_calls;
        expect(protected_somehow,
               "crossing: a value live across a call is callee-saved, saved around, or spilled");
        if (assignment.save_around_calls) {
            expect(assignment.save_index != UINT32_MAX, "crossing: save area allocated");
        }
    }

    // ---- pressure: more call-crossing values than callee-saved registers ----
    {
        mir::Module module;
        module.functions.push_back({});
        module.functions.push_back({});
        auto &callee = module.functions[1];
        callee.name = "callee";
        callee.signature = module.intern_signature({.result = Ty::Void});

        auto &fn = module.functions[0];
        fn.name = "pressure";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::I64}, .result = Ty::I64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::I64);
        builder.set_insert_point(entry);
        std::vector<mir::ValueId> across;
        for (int i = 0; i < 12; ++i) {
            across.push_back(builder.binary(Op::Add, Ty::I64, a, a));
        }
        builder.call(1, Ty::Void, {});
        auto merged = across[0];
        for (size_t i = 1; i < across.size(); ++i) {
            merged = builder.binary(Op::Add, Ty::I64, merged, across[i]);
        }
        builder.ret(merged);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "pressure: verifier clean");
        for (const auto value : across) {
            const auto &assignment = assignment_of(result, value);
            if (!(assignment.spilled || x86ra::is_callee_saved(assignment.reg) ||
                  assignment.save_around_calls)) {
                expect(false, "pressure: an unprotected caller-saved crossing value");
                break;
            }
        }
        expect(true, "pressure: every crossing value protected");
    }

    // ---- floats get the XMM class -------------------------------------------
    {
        mir::Module module;
        module.functions.push_back({});
        auto &fn = module.functions[0];
        fn.name = "floats";
        fn.has_body = true;
        fn.signature = module.intern_signature({.params = {Ty::F64, Ty::F64}, .result = Ty::F64});

        mir::Builder builder(module, 0);
        const auto entry = builder.create_block("entry");
        const auto a = builder.add_block_param(entry, Ty::F64);
        const auto b = builder.add_block_param(entry, Ty::F64);
        builder.set_insert_point(entry);
        const auto sum = builder.binary(Op::FAdd, Ty::F64, a, b);
        builder.ret(sum);

        const auto result = x86ra::allocate(module, fn, x86ra::Mode::Linear);
        expect(result.errors.empty(), "floats: verifier clean");
        const auto &assignment = assignment_of(result, sum);
        expect(!assignment.spilled && x86ra::is_xmm(assignment.reg),
               "floats: result assigned an XMM register");
    }

    if (failures != 0) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("\nall x86 regalloc tests passed\n");
    return 0;
}
