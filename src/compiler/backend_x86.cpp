#include "backend_x86.hpp"

#include "x86_encoder.hpp"

// The trivial-regalloc code generator (see the header). The whole discipline in one
// paragraph: every MIR value owns an 8-byte frame slot at [rbp - offset]; every MIR
// frame slot (aggregates) owns an aligned area below those; each instruction loads
// its operands from their slots into fixed scratch registers (RAX/RCX/RDX for
// integers and pointers, XMM0/XMM1 for floats), computes, and stores the result back
// to its own slot. No liveness, no allocation decisions, no cross-instruction state.
//
// Block parameters get TWO slots each: a staging slot every incoming jump writes,
// and the canonical slot the block body reads. The jump copies all its arguments
// into staging first and staging into canonical second, which makes the classic
// swap/rotation hazard ('jump header(%b, %a)' where a and b ARE the header's
// parameters) impossible by construction rather than by analysis.
//
// Calls follow System V: integer/pointer arguments in RDI RSI RDX RCX R8 R9, floats
// in XMM0-7, the rest on the stack (16-byte aligned at the call), AL = the number of
// vector registers used for a variadic callee. Mirage's own convention is
// deliberately identical at the scalar level, so one path serves both.

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>

namespace backend_x86 {
    namespace {
        using x86::Reg;
        using x86::XReg;
        using x86::Width;
        using x86::Alu;
        using x86::Cond;

        auto width_of(const mir::Ty type) -> Width {
            switch (type) {
            case mir::Ty::I1:
            case mir::Ty::I8: return Width::W8;
            case mir::Ty::I16: return Width::W16;
            case mir::Ty::I32:
            case mir::Ty::F32: return Width::W32;
            default: return Width::W64;
            }
        }

        const Reg INT_ARG_REGS[6] = {Reg::RDI, Reg::RSI, Reg::RDX, Reg::RCX, Reg::R8, Reg::R9};

        struct FunctionContext {
            const mir::Module &module;
            const mir::Function &fn;
            x86::Encoder &enc;
            std::vector<std::string> &errors;
            // Symbol index per module function/global, filled by the module walk.
            const std::vector<uint32_t> &function_symbols;
            const std::vector<uint32_t> &global_symbols;

            std::unordered_map<mir::ValueId, int32_t> value_offset;   // rbp-relative
            std::unordered_map<mir::ValueId, int32_t> staging_offset; // block params only
            std::vector<int32_t> slot_offset;                          // MIR slots
            int32_t frame_size = 0;
            std::vector<x86::Label> block_labels;

            void error(std::string message) {
                errors.push_back(std::move(message));
            }

            // ---- frame layout ----------------------------------------------------
            void layout_frame() {
                int32_t offset = 0;
                const auto place8 = [&]() {
                    offset += 8;
                    return -offset;
                };
                for (const auto &block : fn.blocks) {
                    for (const auto param : block.params) {
                        value_offset[param] = place8();
                        staging_offset[param] = place8();
                    }
                    for (const auto &inst : block.insts) {
                        if (inst.result != mir::NO_VALUE) {
                            value_offset[inst.result] = place8();
                        }
                    }
                }
                slot_offset.resize(fn.slots.size());
                for (size_t i = 0; i < fn.slots.size(); ++i) {
                    const auto align = std::max<int32_t>(1, static_cast<int32_t>(fn.slots[i].align));
                    const auto size = std::max<int32_t>(1, static_cast<int32_t>(fn.slots[i].size));
                    offset += size;
                    offset = (offset + align - 1) / align * align;
                    slot_offset[i] = -offset;
                }
                frame_size = (offset + 15) / 16 * 16;
            }

            // ---- scratch-register value movement ---------------------------------
            void load_value(const mir::ValueId value, const Reg reg) {
                const auto type = fn.values[value].type;
                const auto width = width_of(type);
                if (width == Width::W64) {
                    enc.load(Width::W64, reg, Reg::RBP, value_offset.at(value));
                } else {
                    // Narrow loads zero-extend into the full register so 64-bit scratch
                    // arithmetic on addresses and indices is never fed garbage.
                    enc.load(width, reg, Reg::RBP, value_offset.at(value));
                    if (width != Width::W32) enc.movzx(width, reg, reg);
                }
            }
            void store_result(const mir::ValueId value, const Reg reg) {
                enc.store(width_of(fn.values[value].type), Reg::RBP, value_offset.at(value), reg);
            }
            void store_at(const mir::ValueId value, const int32_t offset, const Reg reg) {
                enc.store(width_of(fn.values[value].type), Reg::RBP, offset, reg);
            }

            // ---- constants and addresses -----------------------------------------
            void materialize(const mir::Inst &inst) {
                switch (inst.op) {
                case mir::Op::ConstInt:
                    enc.mov_ri(Reg::RAX, inst.imm);
                    break;
                case mir::Op::ConstFloat:
                    enc.mov_ri(Reg::RAX, inst.imm); // the payload is already raw bits
                    break;
                case mir::Op::ConstNull:
                    enc.zero(Reg::RAX);
                    break;
                case mir::Op::GlobalAddr:
                    enc.lea_rip(Reg::RAX, global_symbols[inst.a], 0);
                    break;
                case mir::Op::FuncAddr:
                    enc.lea_rip(Reg::RAX, function_symbols[inst.a], 0);
                    break;
                case mir::Op::SlotAddr:
                    enc.lea(Reg::RAX, Reg::RBP, slot_offset[inst.a]);
                    break;
                default:
                    return;
                }
                store_result(inst.result, Reg::RAX);
            }

            // ---- calls -----------------------------------------------------------
            void emit_call(const mir::Inst &inst, const bool indirect) {
                // Classify arguments by their VALUE type: floats ride XMM registers.
                std::vector<mir::ValueId> args(inst.args.begin(), inst.args.end());
                std::vector<int> int_slot(args.size(), -1);
                std::vector<int> float_slot(args.size(), -1);
                std::vector<size_t> stack_args;
                int ints = 0;
                int floats = 0;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (mir::is_float(fn.values[args[i]].type)) {
                        if (floats < 8) float_slot[i] = floats++;
                        else stack_args.push_back(i);
                    } else {
                        if (ints < 6) int_slot[i] = ints++;
                        else stack_args.push_back(i);
                    }
                }

                // Stack arguments, pushed via an aligned reservation. The frame keeps
                // RSP 16-aligned between calls, so only the reservation itself matters.
                const auto reserve = static_cast<int32_t>((stack_args.size() + 1) / 2 * 16);
                if (reserve > 0) enc.sub_rsp(reserve);
                for (size_t i = 0; i < stack_args.size(); ++i) {
                    load_value(args[stack_args[i]], Reg::RAX);
                    enc.store(Width::W64, Reg::RSP, static_cast<int32_t>(i * 8), Reg::RAX);
                }

                // An indirect target goes to R11 BEFORE the argument registers are
                // live; R11 is caller-saved and never an argument register.
                if (indirect) load_value(inst.a, Reg::R11);

                for (size_t i = 0; i < args.size(); ++i) {
                    if (float_slot[i] >= 0) {
                        load_value(args[i], Reg::RAX);
                        enc.mov_r_x(static_cast<XReg>(float_slot[i]), Reg::RAX);
                    } else if (int_slot[i] >= 0) {
                        load_value(args[i], INT_ARG_REGS[int_slot[i]]);
                    }
                }

                const bool variadic = [&] {
                    if (indirect) {
                        return module.signatures[inst.b].is_variadic;
                    }
                    return module.signatures[module.functions[inst.a].signature].is_variadic;
                }();
                if (variadic) enc.mov_ri(Reg::RAX, floats); // AL = vector count

                if (indirect) enc.call_r(Reg::R11);
                else enc.call_sym(function_symbols[inst.a]);

                if (reserve > 0) enc.add_rsp(reserve);

                if (inst.result != mir::NO_VALUE) {
                    if (mir::is_float(fn.values[inst.result].type)) {
                        enc.mov_x_r(Reg::RAX, XReg::XMM0);
                    }
                    store_result(inst.result, Reg::RAX);
                }
            }

            // libc memcpy/memset carry the mem.copy/mem.set ops; the symbols are
            // appended to the module symbol table by the caller and passed in here.
            uint32_t memcpy_symbol = 0;
            uint32_t memset_symbol = 0;
            void emit_mem_op(const mir::Inst &inst, const bool is_set) {
                load_value(inst.a, Reg::RDI);
                load_value(inst.b, Reg::RSI);
                load_value(inst.c, Reg::RDX);
                if (is_set) {
                    // memset takes an int; the byte was zero-extended by load_value.
                    enc.call_sym(memset_symbol);
                } else {
                    enc.call_sym(memcpy_symbol);
                }
            }

            // ---- comparisons -----------------------------------------------------
            static auto icmp_cond(const mir::Op op) -> Cond {
                switch (op) {
                case mir::Op::ICmpEq: return Cond::E;
                case mir::Op::ICmpNe: return Cond::NE;
                case mir::Op::ICmpSlt: return Cond::L;
                case mir::Op::ICmpSle: return Cond::LE;
                case mir::Op::ICmpSgt: return Cond::G;
                case mir::Op::ICmpSge: return Cond::GE;
                case mir::Op::ICmpUlt: return Cond::B;
                case mir::Op::ICmpUle: return Cond::BE;
                case mir::Op::ICmpUgt: return Cond::A;
                default: return Cond::AE;
                }
            }

            void emit_icmp(const mir::Inst &inst) {
                const auto width = width_of(fn.values[inst.a].type);
                load_value(inst.a, Reg::RAX);
                load_value(inst.b, Reg::RCX);
                enc.alu_rr(Alu::Cmp, width == Width::W64 ? Width::W64 : Width::W32,
                            Reg::RAX, Reg::RCX);
                enc.setcc(icmp_cond(inst.op), Reg::RAX);
                store_result(inst.result, Reg::RAX);
            }

            void emit_fcmp(const mir::Inst &inst) {
                const bool is_double = fn.values[inst.a].type == mir::Ty::F64;
                const auto load_x = [&](const mir::ValueId v, const XReg x) {
                    load_value(v, Reg::RAX);
                    enc.mov_r_x(x, Reg::RAX);
                };
                // Olt/Ole compare through the swapped operands so the ABOVE-family
                // conditions serve all four orderings; unordered then reads as false
                // for every one of them except equality, which needs the parity fixup.
                switch (inst.op) {
                case mir::Op::FCmpOlt:
                case mir::Op::FCmpOle:
                    load_x(inst.b, XReg::XMM0);
                    load_x(inst.a, XReg::XMM1);
                    break;
                default:
                    load_x(inst.a, XReg::XMM0);
                    load_x(inst.b, XReg::XMM1);
                    break;
                }
                enc.ucomis(is_double, XReg::XMM0, XReg::XMM1);
                switch (inst.op) {
                case mir::Op::FCmpOeq:
                    enc.setcc(Cond::E, Reg::RAX);
                    enc.setcc(Cond::NP, Reg::RCX);
                    enc.alu_rr(Alu::And, Width::W8, Reg::RAX, Reg::RCX);
                    break;
                case mir::Op::FCmpOne: enc.setcc(Cond::NE, Reg::RAX); break;
                case mir::Op::FCmpOlt:
                case mir::Op::FCmpOgt: enc.setcc(Cond::A, Reg::RAX); break;
                case mir::Op::FCmpOle:
                case mir::Op::FCmpOge: enc.setcc(Cond::AE, Reg::RAX); break;
                default: enc.setcc(Cond::E, Reg::RAX); break;
                }
                store_result(inst.result, Reg::RAX);
            }

            // ---- the per-instruction walk ----------------------------------------
            void emit_inst(const mir::Inst &inst, const mir::BlockId block_id) {
                using Op = mir::Op;
                switch (inst.op) {
                case Op::ConstInt: case Op::ConstFloat: case Op::ConstNull:
                case Op::GlobalAddr: case Op::FuncAddr: case Op::SlotAddr:
                    materialize(inst);
                    return;

                case Op::Load: {
                    load_value(inst.a, Reg::RCX);
                    const auto width = width_of(inst.type);
                    enc.load(width, Reg::RAX, Reg::RCX, 0);
                    if (width != Width::W64 && width != Width::W32) enc.movzx(width, Reg::RAX, Reg::RAX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::Store: {
                    load_value(inst.a, Reg::RCX);
                    load_value(inst.b, Reg::RAX);
                    enc.store(width_of(fn.values[inst.b].type), Reg::RCX, 0, Reg::RAX);
                    return;
                }
                case Op::MemCopy: emit_mem_op(inst, false); return;
                case Op::MemSet: emit_mem_op(inst, true); return;

                case Op::PtrAddConst: {
                    load_value(inst.a, Reg::RAX);
                    enc.lea(Reg::RAX, Reg::RAX, static_cast<int32_t>(inst.imm));
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::PtrAdd: {
                    load_value(inst.a, Reg::RAX);
                    load_value(inst.b, Reg::RCX);
                    enc.alu_rr(Alu::Add, Width::W64, Reg::RAX, Reg::RCX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }

                case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: {
                    load_value(inst.a, Reg::RAX);
                    load_value(inst.b, Reg::RCX);
                    const auto alu = inst.op == Op::Add ? Alu::Add
                                   : inst.op == Op::Sub ? Alu::Sub
                                   : inst.op == Op::And ? Alu::And
                                   : inst.op == Op::Or  ? Alu::Or : Alu::Xor;
                    enc.alu_rr(alu, Width::W64, Reg::RAX, Reg::RCX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::Mul: {
                    load_value(inst.a, Reg::RAX);
                    load_value(inst.b, Reg::RCX);
                    enc.imul_rr(Reg::RAX, Reg::RCX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::SDiv: case Op::SRem: {
                    // Sign-extend the operands to 64 bits first: the narrow values in
                    // their slots were stored zero-extended.
                    load_signed(inst.a, Reg::RAX);
                    load_signed(inst.b, Reg::RCX);
                    enc.cqo();
                    enc.idiv_r(Reg::RCX);
                    store_result(inst.result, inst.op == Op::SDiv ? Reg::RAX : Reg::RDX);
                    return;
                }
                case Op::UDiv: case Op::URem: {
                    load_value(inst.a, Reg::RAX);
                    load_value(inst.b, Reg::RCX);
                    enc.zero(Reg::RDX);
                    enc.div_r(Reg::RCX);
                    store_result(inst.result, inst.op == Op::UDiv ? Reg::RAX : Reg::RDX);
                    return;
                }
                case Op::Shl: case Op::LShr: case Op::AShr: {
                    if (inst.op == Op::AShr) load_signed(inst.a, Reg::RAX);
                    else load_value(inst.a, Reg::RAX);
                    load_value(inst.b, Reg::RCX);
                    if (inst.op == Op::Shl) enc.shl_cl(Width::W64, Reg::RAX);
                    else if (inst.op == Op::LShr) enc.shr_cl(Width::W64, Reg::RAX);
                    else enc.sar_cl(Width::W64, Reg::RAX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::Not: {
                    load_value(inst.a, Reg::RAX);
                    if (inst.type == mir::Ty::I1) {
                        enc.alu_ri(Alu::Xor, Width::W8, Reg::RAX, 1);
                    } else {
                        enc.not_r(Width::W64, Reg::RAX);
                    }
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::Neg: {
                    load_value(inst.a, Reg::RAX);
                    enc.neg_r(Width::W64, Reg::RAX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }

                case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: {
                    const bool is_double = inst.type == mir::Ty::F64;
                    load_value(inst.a, Reg::RAX);
                    enc.mov_r_x(XReg::XMM0, Reg::RAX);
                    load_value(inst.b, Reg::RAX);
                    enc.mov_r_x(XReg::XMM1, Reg::RAX);
                    const uint8_t opcode = inst.op == Op::FAdd ? 0x58
                                          : inst.op == Op::FSub ? 0x5C
                                          : inst.op == Op::FMul ? 0x59 : 0x5E;
                    enc.sse_arith(opcode, is_double, XReg::XMM0, XReg::XMM1);
                    enc.mov_x_r(Reg::RAX, XReg::XMM0);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::FRem: {
                    // No hardware frem: libm fmod/fmodf via the same call machinery.
                    const bool is_double = inst.type == mir::Ty::F64;
                    load_value(inst.a, Reg::RAX);
                    enc.mov_r_x(XReg::XMM0, Reg::RAX);
                    load_value(inst.b, Reg::RAX);
                    enc.mov_r_x(XReg::XMM1, Reg::RAX);
                    enc.mov_ri(Reg::RAX, 2); // AL: two vector args (fmod is not variadic, but harmless)
                    enc.call_sym(is_double ? fmod_symbol : fmodf_symbol);
                    enc.mov_x_r(Reg::RAX, XReg::XMM0);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::FNeg: {
                    load_value(inst.a, Reg::RAX);
                    const bool is_double = inst.type == mir::Ty::F64;
                    enc.mov_ri(Reg::RCX, is_double ? static_cast<int64_t>(0x8000000000000000ULL)
                                                    : 0x80000000LL);
                    enc.alu_rr(Alu::Xor, Width::W64, Reg::RAX, Reg::RCX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }

                case Op::ICmpEq: case Op::ICmpNe:
                case Op::ICmpSlt: case Op::ICmpSle: case Op::ICmpSgt: case Op::ICmpSge:
                case Op::ICmpUlt: case Op::ICmpUle: case Op::ICmpUgt: case Op::ICmpUge:
                    emit_icmp(inst);
                    return;
                case Op::FCmpOeq: case Op::FCmpOne:
                case Op::FCmpOlt: case Op::FCmpOle: case Op::FCmpOgt: case Op::FCmpOge:
                    emit_fcmp(inst);
                    return;

                case Op::Trunc: case Op::Bitcast: case Op::PtrToInt: case Op::IntToPtr: {
                    // Pure representation moves: the slot write at the RESULT's width is
                    // the whole operation.
                    load_value(inst.a, Reg::RAX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::ZExt: {
                    load_value(inst.a, Reg::RAX); // already zero-extended by load
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::SExt: {
                    load_signed(inst.a, Reg::RAX);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::FPTrunc: case Op::FPExt: {
                    const bool to_double = inst.type == mir::Ty::F64;
                    load_value(inst.a, Reg::RAX);
                    enc.mov_r_x(XReg::XMM0, Reg::RAX);
                    enc.cvt_f2f(to_double, XReg::XMM0, XReg::XMM0);
                    enc.mov_x_r(Reg::RAX, XReg::XMM0);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::SIToFP: case Op::UIToFP: {
                    // The unsigned 64-bit edge (values >= 2^63) is knowingly emitted as
                    // the signed conversion; narrower unsigned sources were already
                    // zero-extended, which makes the signed instruction exact for them.
                    if (inst.op == Op::SIToFP) load_signed(inst.a, Reg::RAX);
                    else load_value(inst.a, Reg::RAX);
                    enc.cvt_i2f(inst.type == mir::Ty::F64, Width::W64, XReg::XMM0, Reg::RAX);
                    enc.mov_x_r(Reg::RAX, XReg::XMM0);
                    store_result(inst.result, Reg::RAX);
                    return;
                }
                case Op::FPToSI: case Op::FPToUI: {
                    const bool from_double = fn.values[inst.a].type == mir::Ty::F64;
                    load_value(inst.a, Reg::RAX);
                    enc.mov_r_x(XReg::XMM0, Reg::RAX);
                    enc.cvt_f2i(from_double, Width::W64, Reg::RAX, XReg::XMM0);
                    store_result(inst.result, Reg::RAX);
                    return;
                }

                case Op::Select: {
                    const auto skip = enc.make_label();
                    const auto done = enc.make_label();
                    load_value(inst.a, Reg::RAX);
                    enc.test_rr(Width::W8, Reg::RAX, Reg::RAX);
                    enc.jcc(Cond::E, skip);
                    load_value(inst.b, Reg::RAX);
                    enc.jmp(done);
                    enc.bind(skip);
                    load_value(inst.c, Reg::RAX);
                    enc.bind(done);
                    store_result(inst.result, Reg::RAX);
                    return;
                }

                case Op::Call: emit_call(inst, false); return;
                case Op::CallIndirect: emit_call(inst, true); return;
                case Op::Asm: emit_asm(inst); return;

                case Op::Jump: {
                    pass_block_args(inst.a, inst.args);
                    enc.jmp(block_labels[inst.a]);
                    return;
                }
                case Op::Branch: {
                    load_value(inst.a, Reg::RAX);
                    enc.test_rr(Width::W8, Reg::RAX, Reg::RAX);
                    enc.jcc(Cond::NE, block_labels[inst.b]);
                    enc.jmp(block_labels[inst.c]);
                    return;
                }
                case Op::Switch: {
                    // Case values are stored 32-bit-truncated (Builder::switch_on) and
                    // read back sign-extended. A narrow scrutinee therefore compares at
                    // 32 bits on the RAW pattern — a 64-bit compare would disagree about
                    // negative values, since load_value zero-extends the scrutinee.
                    const bool wide = width_of(fn.values[inst.a].type) == Width::W64;
                    load_value(inst.a, Reg::RAX);
                    for (size_t i = 0; i + 1 < inst.args.size(); i += 2) {
                        if (wide) {
                            enc.mov_ri(Reg::RCX,
                                        static_cast<int64_t>(static_cast<int32_t>(inst.args[i])));
                            enc.alu_rr(Alu::Cmp, Width::W64, Reg::RAX, Reg::RCX);
                        } else {
                            enc.mov_ri(Reg::RCX, inst.args[i]); // raw 32-bit pattern
                            enc.alu_rr(Alu::Cmp, Width::W32, Reg::RAX, Reg::RCX);
                        }
                        enc.jcc(Cond::E, block_labels[inst.args[i + 1]]);
                    }
                    enc.jmp(block_labels[inst.b]);
                    return;
                }
                case Op::Return: {
                    if (!inst.args.empty()) {
                        load_value(inst.args.front(), Reg::RAX);
                        if (mir::is_float(fn.values[inst.args.front()].type)) {
                            enc.mov_r_x(XReg::XMM0, Reg::RAX);
                        }
                    }
                    emit_epilogue();
                    return;
                }
                case Op::Unreachable:
                    enc.ud2();
                    return;
                }
                error(std::format("unhandled MIR op '{}' in block {}", op_name(inst.op), block_id));
            }

            uint32_t fmod_symbol = 0;
            uint32_t fmodf_symbol = 0;

            // ---- inline asm ------------------------------------------------------
            // Every operand is already resolved (sema) and every variable operand is a
            // pointer to a FRAME SLOT (mirgen pins them), so an asm instruction encodes
            // directly against [rbp - offset] with no register allocation and no
            // constraint model at all — the simplification the plan predicted from
            // owning the allocator.
            struct AsmArg {
                bool is_memory = false;
                int32_t disp = 0;   // memory: rbp-relative
                Reg reg = Reg::RAX; // register operand
                int64_t imm = 0;
                bool is_imm = false;
                Width width = Width::W64;
            };

            static auto reg_by_name(const std::string &name) -> std::optional<Reg> {
                static const std::unordered_map<std::string, Reg> table = {
                    {"rax", Reg::RAX}, {"eax", Reg::RAX}, {"ax", Reg::RAX}, {"al", Reg::RAX},
                    {"rcx", Reg::RCX}, {"ecx", Reg::RCX}, {"cx", Reg::RCX}, {"cl", Reg::RCX},
                    {"rdx", Reg::RDX}, {"edx", Reg::RDX}, {"dx", Reg::RDX}, {"dl", Reg::RDX},
                    {"rbx", Reg::RBX}, {"ebx", Reg::RBX}, {"bx", Reg::RBX}, {"bl", Reg::RBX},
                    {"rsp", Reg::RSP}, {"esp", Reg::RSP}, {"sp", Reg::RSP}, {"spl", Reg::RSP},
                    {"rbp", Reg::RBP}, {"ebp", Reg::RBP}, {"bp", Reg::RBP}, {"bpl", Reg::RBP},
                    {"rsi", Reg::RSI}, {"esi", Reg::RSI}, {"si", Reg::RSI}, {"sil", Reg::RSI},
                    {"rdi", Reg::RDI}, {"edi", Reg::RDI}, {"di", Reg::RDI}, {"dil", Reg::RDI},
                    {"r8", Reg::R8},   {"r8d", Reg::R8},  {"r8w", Reg::R8},  {"r8b", Reg::R8},
                    {"r9", Reg::R9},   {"r9d", Reg::R9},  {"r9w", Reg::R9},  {"r9b", Reg::R9},
                    {"r10", Reg::R10}, {"r10d", Reg::R10}, {"r10w", Reg::R10}, {"r10b", Reg::R10},
                    {"r11", Reg::R11}, {"r11d", Reg::R11}, {"r11w", Reg::R11}, {"r11b", Reg::R11},
                    {"r12", Reg::R12}, {"r12d", Reg::R12}, {"r12w", Reg::R12}, {"r12b", Reg::R12},
                    {"r13", Reg::R13}, {"r13d", Reg::R13}, {"r13w", Reg::R13}, {"r13b", Reg::R13},
                    {"r14", Reg::R14}, {"r14d", Reg::R14}, {"r14w", Reg::R14}, {"r14b", Reg::R14},
                    {"r15", Reg::R15}, {"r15d", Reg::R15}, {"r15w", Reg::R15}, {"r15b", Reg::R15},
                };
                const auto it = table.find(name);
                return it == table.end() ? std::optional<Reg>{} : it->second;
            }

            static auto width_from_bits(const uint32_t bits) -> Width {
                switch (bits) {
                case 8:  return Width::W8;
                case 16: return Width::W16;
                case 32: return Width::W32;
                default: return Width::W64;
                }
            }

            // The rbp offset a variable operand's pointer refers to. Only a slot address
            // qualifies -- which is all mirgen ever produces for an asm operand.
            auto slot_disp_of(const mir::ValueId value) const -> std::optional<int32_t> {
                const auto &def = fn.values[value];
                if (def.is_param || def.block >= fn.blocks.size()) return std::nullopt;
                const auto &block = fn.blocks[def.block];
                for (const auto &candidate : block.insts) {
                    if (candidate.result == value) {
                        if (candidate.op != mir::Op::SlotAddr) return std::nullopt;
                        return slot_offset[candidate.a];
                    }
                }
                return std::nullopt;
            }

            void emit_asm(const mir::Inst &inst) {
                const auto &block = module.asm_blocks[inst.a];
                for (const auto &instruction : block.instructions) {
                    std::vector<AsmArg> args;
                    bool ok = true;
                    for (const auto &operand : instruction.operands) {
                        AsmArg arg;
                        switch (operand.kind) {
                        case mir::AsmOperand::Kind::Register: {
                            const auto reg = reg_by_name(operand.reg);
                            if (!reg) {
                                error(std::format("inline asm: unsupported register '{}'", operand.reg));
                                ok = false;
                                break;
                            }
                            arg.reg = *reg;
                            arg.width = width_from_bits(operand.width_bits);
                            break;
                        }
                        case mir::AsmOperand::Kind::Immediate:
                            arg.is_imm = true;
                            arg.imm = operand.imm;
                            break;
                        case mir::AsmOperand::Kind::Variable: {
                            const auto disp = operand.arg_index < inst.args.size()
                                ? slot_disp_of(inst.args[operand.arg_index]) : std::nullopt;
                            if (!disp) {
                                error("inline asm: a variable operand did not resolve to a frame slot");
                                ok = false;
                                break;
                            }
                            arg.is_memory = true;
                            arg.disp = *disp;
                            arg.width = width_from_bits(operand.width_bits);
                            break;
                        }
                        }
                        args.push_back(arg);
                    }
                    if (!ok) return;
                    emit_asm_instruction(instruction, args);
                }

                // 'asm -> reg': that register's value AT BLOCK EXIT is the expression's
                // result. Stored immediately, with nothing emitted in between that could
                // disturb it.
                if (inst.result == mir::NO_VALUE) return;
                const auto reg = reg_by_name(block.result_register);
                if (!reg) {
                    error(std::format("inline asm: unsupported result register '{}'",
                                       block.result_register));
                    return;
                }
                store_result(inst.result, *reg);
            }

            // The operand width an instruction runs at: a register operand names it
            // explicitly ('eax' is 32), otherwise the variable's own width decides.
            static auto asm_width(const std::vector<AsmArg> &args) -> Width {
                for (const auto &arg : args) {
                    if (!arg.is_memory && !arg.is_imm) return arg.width;
                }
                for (const auto &arg : args) {
                    if (arg.is_memory) return arg.width;
                }
                return Width::W64;
            }

            void emit_asm_instruction(const mir::AsmInstruction &instruction,
                                       const std::vector<AsmArg> &args) {
                const auto &m = instruction.mnemonic;
                const auto w = asm_width(args);
                const auto two = args.size() == 2;

                static const std::unordered_map<std::string, Alu> alu_ops = {
                    {"add", Alu::Add}, {"sub", Alu::Sub}, {"and", Alu::And},
                    {"or", Alu::Or}, {"xor", Alu::Xor}, {"cmp", Alu::Cmp},
                };
                // not/neg/inc/dec share the /digit unary encodings.
                static const std::unordered_map<std::string, uint8_t> unary_slots = {
                    {"inc", 0}, {"dec", 1}, {"not", 2}, {"neg", 3},
                };

                if (m == "nop") { enc.nop(); return; }
                if (m == "syscall") { enc.syscall(); return; }
                if (m == "cpuid") { enc.cpuid(); return; }
                if (m == "ret") { enc.ret(); return; }

                if (m == "mov" && two) {
                    const auto &dst = args[0];
                    const auto &src = args[1];
                    if (!dst.is_memory && src.is_imm) { enc.mov_ri(dst.reg, src.imm); return; }
                    if (!dst.is_memory && !src.is_memory && !src.is_imm) { enc.mov_rr(w, dst.reg, src.reg); return; }
                    if (!dst.is_memory && src.is_memory) { enc.load(w, dst.reg, Reg::RBP, src.disp); return; }
                    if (dst.is_memory && !src.is_memory && !src.is_imm) { enc.store(w, Reg::RBP, dst.disp, src.reg); return; }
                    if (dst.is_memory && src.is_imm) {
                        enc.mov_mi(w, Reg::RBP, dst.disp, static_cast<int32_t>(src.imm));
                        return;
                    }
                }
                if (m == "movzx" && two && !args[0].is_memory) {
                    if (args[1].is_memory) { enc.movzx_m(args[1].width, args[0].reg, Reg::RBP, args[1].disp); return; }
                    if (!args[1].is_imm) { enc.movzx(args[1].width, args[0].reg, args[1].reg); return; }
                }
                if (m == "lea" && two && !args[0].is_memory && args[1].is_memory) {
                    enc.lea(args[0].reg, Reg::RBP, args[1].disp);
                    return;
                }
                if (const auto alu = alu_ops.find(m); alu != alu_ops.end() && two) {
                    const auto &dst = args[0];
                    const auto &src = args[1];
                    if (!dst.is_memory && src.is_imm) { enc.alu_ri(alu->second, w, dst.reg, static_cast<int32_t>(src.imm)); return; }
                    if (!dst.is_memory && !src.is_memory) { enc.alu_rr(alu->second, w, dst.reg, src.reg); return; }
                    if (!dst.is_memory && src.is_memory) { enc.alu_rm(alu->second, w, dst.reg, Reg::RBP, src.disp); return; }
                    if (dst.is_memory && src.is_imm) { enc.alu_mi(alu->second, w, Reg::RBP, dst.disp, static_cast<int32_t>(src.imm)); return; }
                    if (dst.is_memory && !src.is_memory) { enc.alu_mr(alu->second, w, Reg::RBP, dst.disp, src.reg); return; }
                }
                if (m == "test" && two && !args[0].is_memory && !args[1].is_memory && !args[1].is_imm) {
                    enc.test_rr(w, args[0].reg, args[1].reg);
                    return;
                }
                if (const auto unary = unary_slots.find(m);
                    unary != unary_slots.end() && args.size() == 1) {
                    if (args[0].is_memory) enc.unary_m(unary->second, w, Reg::RBP, args[0].disp);
                    else enc.unary_r(unary->second, w, args[0].reg);
                    return;
                }
                if (m == "push" && args.size() == 1) {
                    if (args[0].is_memory) enc.push_m(Reg::RBP, args[0].disp);
                    else enc.push_r(args[0].reg);
                    return;
                }
                if (m == "pop" && args.size() == 1) {
                    if (args[0].is_memory) enc.pop_m(Reg::RBP, args[0].disp);
                    else enc.pop_r(args[0].reg);
                    return;
                }

                // Loud refusal, naming the instruction: an asm block the encoder cannot
                // render must never be silently dropped from the output.
                error(std::format("inline asm: cannot encode '{}' with {} operand(s) in this form "
                                   "(line {})", m, args.size(), instruction.line));
            }

            void load_signed(const mir::ValueId value, const Reg reg) {
                const auto width = width_of(fn.values[value].type);
                if (width == Width::W64) {
                    enc.load(Width::W64, reg, Reg::RBP, value_offset.at(value));
                } else {
                    enc.load(width, reg, Reg::RBP, value_offset.at(value));
                    enc.movsx(width, reg, reg);
                }
            }

            void pass_block_args(const mir::BlockId target, const std::vector<uint32_t> &args) {
                const auto &params = fn.blocks[target].params;
                // Two phases through the staging slots: rotation-safe by construction.
                for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
                    load_value(args[i], Reg::RAX);
                    store_at(params[i], staging_offset.at(params[i]), Reg::RAX);
                }
                for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
                    enc.load(Width::W64, Reg::RAX, Reg::RBP, staging_offset.at(params[i]));
                    enc.store(Width::W64, Reg::RBP, value_offset.at(params[i]), Reg::RAX);
                }
            }

            void emit_epilogue() {
                enc.mov_rr(Width::W64, Reg::RSP, Reg::RBP);
                enc.pop_r(Reg::RBP);
                enc.ret();
            }

            void emit() {
                layout_frame();
                for (size_t i = 0; i < fn.blocks.size(); ++i) {
                    block_labels.push_back(enc.make_label());
                }

                enc.push_r(Reg::RBP);
                enc.mov_rr(Width::W64, Reg::RBP, Reg::RSP);
                if (frame_size > 0) enc.sub_rsp(frame_size);

                // Incoming arguments (the entry block's parameters) spill from their
                // System V registers; stack-passed extras sit above the return address.
                int ints = 0;
                int floats = 0;
                int32_t caller_stack = 16; // saved RBP + return address
                if (!fn.blocks.empty()) {
                    for (const auto param : fn.blocks.front().params) {
                        if (mir::is_float(fn.values[param].type)) {
                            if (floats < 8) {
                                enc.mov_x_r(Reg::RAX, static_cast<XReg>(floats++));
                                store_result(param, Reg::RAX);
                                continue;
                            }
                        } else if (ints < 6) {
                            store_result(param, INT_ARG_REGS[ints++]);
                            continue;
                        }
                        enc.load(Width::W64, Reg::RAX, Reg::RBP, caller_stack);
                        caller_stack += 8;
                        store_result(param, Reg::RAX);
                    }
                }

                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    enc.bind(block_labels[b]);
                    for (const auto &inst : fn.blocks[b].insts) {
                        emit_inst(inst, static_cast<mir::BlockId>(b));
                    }
                }
            }
        };
    }

    auto generate(const mir::Module &module) -> Result {
        Result result;
        elf::Object &object = result.object;

        // ---- globals ---------------------------------------------------------
        // Placement: initialized constants in .rodata, initialized mutables in
        // .data, zero-initialized in .bss. Initializer relocations become
        // R_X86_64_64 entries against the target's symbol.
        std::vector<uint32_t> global_symbols(module.globals.size());
        std::vector<uint32_t> function_symbols(module.functions.size());
        struct PlacedGlobal { elf::Section section; uint64_t offset; };
        std::vector<PlacedGlobal> placed(module.globals.size());

        const auto place_bytes = [](std::vector<uint8_t> &into, const std::vector<uint8_t> &bytes,
                                     const uint32_t align) {
            while (align > 1 && into.size() % align != 0) into.push_back(0);
            const auto at = into.size();
            into.insert(into.end(), bytes.begin(), bytes.end());
            return static_cast<uint64_t>(at);
        };

        for (size_t i = 0; i < module.globals.size(); ++i) {
            const auto &global = module.globals[i];
            elf::Section section;
            uint64_t offset;
            if (global.init.empty()) {
                section = elf::Section::Bss;
                const auto align = std::max<uint64_t>(1, global.align);
                object.bss_size = (object.bss_size + align - 1) / align * align;
                offset = object.bss_size;
                object.bss_size += std::max<uint64_t>(1, global.size);
                object.bss_align = std::max<uint64_t>(object.bss_align, align);
            } else if (global.is_constant) {
                section = elf::Section::Rodata;
                offset = place_bytes(object.rodata, global.init, global.align);
                object.rodata_align = std::max<uint64_t>(object.rodata_align, global.align);
            } else {
                section = elf::Section::Data;
                offset = place_bytes(object.data, global.init, global.align);
                object.data_align = std::max<uint64_t>(object.data_align, global.align);
            }
            placed[i] = {section, offset};
            global_symbols[i] = static_cast<uint32_t>(object.symbols.size());
            object.symbols.push_back({
                .name = global.name,
                .section = section,
                .value = offset,
                .size = global.size,
                .is_global = global.linkage == mir::Linkage::External,
                .is_function = false,
            });
        }
        // Initializer relocations, now that every global has a home.
        for (size_t i = 0; i < module.globals.size(); ++i) {
            for (const auto &reloc : module.globals[i].relocations) {
                object.relocations.push_back({
                    .in = placed[i].section,
                    .offset = placed[i].offset + reloc.offset,
                    .symbol = reloc.kind == mir::Relocation::Kind::FunctionAddr
                        ? function_symbols[reloc.target] // patched below once functions exist
                        : global_symbols[reloc.target],
                    .type = elf::R_X86_64_64,
                    .addend = reloc.addend,
                });
            }
        }

        // ---- function symbols (declared before bodies so calls can reference) --
        const auto first_function_symbol = static_cast<uint32_t>(object.symbols.size());
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            function_symbols[i] = static_cast<uint32_t>(object.symbols.size());
            object.symbols.push_back({
                .name = fn.name,
                .section = fn.has_body ? elf::Section::Text : elf::Section::Undefined,
                .value = 0, // patched after emission
                .size = 0,
                .is_global = !fn.has_body || fn.linkage == mir::Linkage::External,
                .is_function = true,
            });
        }
        // Function-address initializer relocations recorded above used
        // 'function_symbols' before it was filled; fix them up.
        {
            size_t reloc_index = 0;
            for (size_t i = 0; i < module.globals.size(); ++i) {
                for (const auto &reloc : module.globals[i].relocations) {
                    if (reloc.kind == mir::Relocation::Kind::FunctionAddr) {
                        object.relocations[reloc_index].symbol = function_symbols[reloc.target];
                    }
                    ++reloc_index;
                }
            }
        }

        const auto runtime_symbol = [&](const char *name) {
            for (uint32_t s = 0; s < object.symbols.size(); ++s) {
                if (object.symbols[s].name == name) return s;
            }
            object.symbols.push_back({.name = name, .section = elf::Section::Undefined,
                                       .is_global = true, .is_function = true});
            return static_cast<uint32_t>(object.symbols.size() - 1);
        };
        const auto memcpy_symbol = runtime_symbol("memcpy");
        const auto memset_symbol = runtime_symbol("memset");
        const auto fmod_symbol = runtime_symbol("fmod");
        const auto fmodf_symbol = runtime_symbol("fmodf");

        // ---- bodies ----------------------------------------------------------
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body) continue;

            // 16-align each function's entry.
            while (object.text.size() % 16 != 0) object.text.push_back(0x90);
            const auto start = object.text.size();

            x86::Encoder enc;
            FunctionContext ctx{
                .module = module,
                .fn = fn,
                .enc = enc,
                .errors = result.errors,
                .function_symbols = function_symbols,
                .global_symbols = global_symbols,
            };
            ctx.memcpy_symbol = memcpy_symbol;
            ctx.memset_symbol = memset_symbol;
            ctx.fmod_symbol = fmod_symbol;
            ctx.fmodf_symbol = fmodf_symbol;
            ctx.emit();
            enc.resolve_labels();

            object.symbols[first_function_symbol + i].value = start;
            object.symbols[first_function_symbol + i].size = enc.code.size();
            object.text.insert(object.text.end(), enc.code.begin(), enc.code.end());

            for (const auto &reloc : enc.relocations) {
                object.relocations.push_back({
                    .in = elf::Section::Text,
                    .offset = start + reloc.offset,
                    .symbol = reloc.symbol,
                    .type = reloc.kind == x86::Relocation::Kind::Call32
                        ? elf::R_X86_64_PLT32 : elf::R_X86_64_PC32,
                    .addend = reloc.addend + (reloc.kind == x86::Relocation::Kind::Rip32 ? -4 : 0),
                });
            }
        }

        // ---- entry glue ------------------------------------------------------
        // The hosted link passes '-nostartfiles' (the LLVM path synthesizes its own
        // '_start', and both backends must agree), so a defined 'main' needs the same
        // glue here: call '_init' when the module defines one, call 'main', then hand
        // its result to libc 'exit' — which also flushes stdio, keeping printf output
        // byte-identical with the LLVM build.
        int32_t main_index = -1;
        int32_t init_index = -1;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (!module.functions[i].has_body) continue;
            if (module.functions[i].name == "main") main_index = static_cast<int32_t>(i);
            if (module.functions[i].name == "_init") init_index = static_cast<int32_t>(i);
        }
        if (main_index >= 0) {
            const auto exit_symbol = runtime_symbol("exit");
            while (object.text.size() % 16 != 0) object.text.push_back(0x90);
            const auto start = object.text.size();
            x86::Encoder enc;
            // At process entry RSP is 16-aligned with argc on top; the AND keeps the
            // alignment guarantee even when a loader bends the rule, and the call
            // then puts the callee at the standard rsp%16==8.
            enc.alu_ri(Alu::And, Width::W64, Reg::RSP, -16);
            if (init_index >= 0) enc.call_sym(function_symbols[init_index]);
            // Three validated 'main' shapes (validate_hosted_main), with the same exit
            // codes the LLVM _start produces: '-> i32' exits with the value, void exits
            // 0, '-> error(...)' exits with (tag != 0) — the error travels through the
            // sret blob this glue owns.
            const auto &main_sig = module.signatures[module.functions[main_index].signature];
            const bool sret_main = main_sig.result == mir::Ty::Void && !main_sig.params.empty();
            if (sret_main) {
                enc.sub_rsp(128);
                enc.mov_rr(Width::W64, Reg::RDI, Reg::RSP);
                enc.call_sym(function_symbols[main_index]);
                enc.load(Width::W32, Reg::RAX, Reg::RSP, 0);
                enc.test_rr(Width::W32, Reg::RAX, Reg::RAX);
                enc.setcc(Cond::NE, Reg::RAX);
                enc.movzx(Width::W8, Reg::RAX, Reg::RAX);
                enc.mov_rr(Width::W32, Reg::RDI, Reg::RAX);
            } else if (main_sig.result == mir::Ty::Void) {
                enc.call_sym(function_symbols[main_index]);
                enc.zero(Reg::RDI);
            } else {
                enc.call_sym(function_symbols[main_index]);
                enc.mov_rr(Width::W32, Reg::RDI, Reg::RAX);
            }
            enc.call_sym(exit_symbol);
            enc.ud2();
            enc.resolve_labels();
            object.symbols.push_back({
                .name = "_start",
                .section = elf::Section::Text,
                .value = start,
                .size = enc.code.size(),
                .is_global = true,
                .is_function = true,
            });
            object.text.insert(object.text.end(), enc.code.begin(), enc.code.end());
            for (const auto &reloc : enc.relocations) {
                object.relocations.push_back({
                    .in = elf::Section::Text,
                    .offset = start + reloc.offset,
                    .symbol = reloc.symbol,
                    .type = elf::R_X86_64_PLT32,
                    .addend = reloc.addend,
                });
            }
        }

        result.ok = result.errors.empty();
        return result;
    }
}
