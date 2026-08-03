#include "backend_x86.hpp"

#include "x86_encoder.hpp"
#include "x86_regalloc.hpp"

// The x86-64 code generator, stage 6 shape: one emission engine that reads every
// operand from wherever the register allocator put it — a physical register or an
// 8-byte frame area — and writes results the same way. '--regalloc=trivial' is the
// degenerate assignment (everything spilled), which reproduces the stage-4/5
// discipline through the SAME templates and stays forever as the triage tool;
// '--regalloc=linear' is the real allocator in x86_regalloc.cpp.
//
// Invariants the templates rely on:
//
//  - CANONICAL FORM. An integer value in a register or spill area is always
//    zero-extended to 64 bits; templates that need sign re-extend explicitly
//    (movsx), and every result is re-canonicalized at its type's width before it
//    is committed. This is the register-world restatement of the trivial
//    allocator's narrow-store/zero-extending-load convention, so both modes
//    compute identical bit patterns.
//
//  - EARLY DEFS. The allocator defines straight-line results at the same position
//    their operands are read (x86_regalloc.hpp), so a result register never
//    aliases an operand register and two-address templates ('mov dst, a; op dst,
//    b') are safe by construction. Call and asm results are defined late, after
//    the clobber point.
//
//  - KILL RANGES. Templates that need registers by name (div/idiv: rdx:rax,
//    shifts: cl, stackalloc: rax, calls: every caller-saved register and all
//    XMMs, asm: its clobber set) had those registers reserved by the allocator at
//    those positions. Everything else acquires scratch dynamically from registers
//    that are dead at the current instruction, falling back to push/pop around
//    the one instruction in the (rare) case none is free.
//
//  - BLOCK ARGUMENTS go through per-parameter staging slots in memory: every jump
//    writes all its arguments into staging first and staging into the parameters'
//    homes second, which makes the classic swap/rotation hazard impossible by
//    construction rather than by analysis — the same reasoning the trivial
//    backend used, kept because it is proof, not inertia.
//
// Calls follow System V: integer/pointer arguments in RDI RSI RDX RCX R8 R9,
// floats in XMM0-7, the rest on the stack (16-byte aligned at the call), AL = the
// number of vector registers used for a variadic callee. Values the allocator
// parked with save_around_calls are stored to their save areas before the call
// sequence begins and reloaded after it ends — the interval-splitting-around-
// calls form docs/backend.md stage 6 requires.

#include <algorithm>
#include <cstdlib>
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

        auto greg(const x86ra::PhysReg reg) -> Reg { return static_cast<Reg>(reg); }
        auto xreg(const x86ra::PhysReg reg) -> XReg {
            return static_cast<XReg>(reg - x86ra::FIRST_XMM);
        }
        auto bit(const Reg reg) -> uint32_t { return 1u << static_cast<uint32_t>(reg); }
        auto bit(const XReg reg) -> uint32_t { return 1u << (16 + static_cast<uint32_t>(reg)); }

        const Reg INT_ARG_REGS[6] = {Reg::RDI, Reg::RSI, Reg::RDX, Reg::RCX, Reg::R8, Reg::R9};

        struct FunctionContext {
            const mir::Module &module;
            const mir::Function &fn;
            x86::Encoder &enc;
            std::vector<std::string> &errors;
            const std::vector<uint32_t> &function_symbols;
            const std::vector<uint32_t> &global_symbols;
            const x86ra::Result &ra;

            // ---- frame ----------------------------------------------------------
            std::vector<int32_t> slot_offset;                       // MIR slots
            std::vector<int32_t> area_offset;                       // spill/save areas
            std::unordered_map<mir::ValueId, int32_t> staging_offset; // block params
            int32_t callee_save_offset[16] = {};
            int32_t frame_size = 0;
            std::vector<x86::Label> block_labels;
            // C-ABI support areas (mir.hpp Signature metadata): a stash for a
            // call's two-word-return out-pointer, this function's own two-word
            // return blob, and the sret pointer kept for the RAX-on-return rule.
            int32_t cret_stash_offset = 0;
            int32_t cret_blob_offset = 0;
            int32_t sret_stash_offset = 0;

            uint32_t memcpy_symbol = 0;
            uint32_t memset_symbol = 0;
            uint32_t fmod_symbol = 0;
            uint32_t fmodf_symbol = 0;

            // ---- emission sweep (dynamic scratch + save-around) -----------------
            struct ActiveInterval {
                uint32_t start = 0;
                uint32_t end = 0;
                mir::ValueId value = mir::NO_VALUE;
                x86ra::PhysReg reg = x86ra::NO_PHYS;
                bool save_around = false;
            };
            std::vector<ActiveInterval> active;
            size_t next_interval = 0;
            uint32_t current_index = 0;
            uint32_t lease_mask = 0; // one bit per PhysReg currently leased

            void error(std::string message) { errors.push_back(std::move(message)); }

            // ---- assignment access ----------------------------------------------
            [[nodiscard]] auto assignment(const mir::ValueId v) const -> const x86ra::Assignment & {
                return ra.values[v];
            }
            [[nodiscard]] auto in_reg(const mir::ValueId v) const -> bool {
                return !assignment(v).spilled;
            }
            [[nodiscard]] auto val_gpr(const mir::ValueId v) const -> Reg {
                return greg(assignment(v).reg);
            }
            [[nodiscard]] auto val_xmm(const mir::ValueId v) const -> XReg {
                return xreg(assignment(v).reg);
            }
            [[nodiscard]] auto val_off(const mir::ValueId v) const -> int32_t {
                return area_offset[assignment(v).spill_index];
            }
            [[nodiscard]] auto is_double(const mir::ValueId v) const -> bool {
                return fn.values[v].type == mir::Ty::F64;
            }

            // ---- frame layout ----------------------------------------------------
            void layout_frame() {
                int32_t offset = 0;
                const auto place8 = [&] {
                    offset += 8;
                    return -offset;
                };
                for (Reg r : {Reg::RBX, Reg::R12, Reg::R13, Reg::R14, Reg::R15}) {
                    if (ra.used_callee_saved & bit(r)) {
                        callee_save_offset[static_cast<int>(r)] = place8();
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
                area_offset.resize(ra.spill_area_count);
                for (uint32_t i = 0; i < ra.spill_area_count; ++i) area_offset[i] = place8();
                for (const auto &block : fn.blocks) {
                    for (const auto param : block.params) staging_offset[param] = place8();
                }

                bool calls_two_word_ret = false;
                for (const auto &block : fn.blocks) {
                    for (const auto &inst : block.insts) {
                        if (inst.op != mir::Op::Call && inst.op != mir::Op::CallIndirect) continue;
                        const auto signature = inst.op == mir::Op::CallIndirect
                            ? inst.b : module.functions[inst.a].signature;
                        if (module.signatures[signature].c_ret_words == 2) calls_two_word_ret = true;
                    }
                }
                if (calls_two_word_ret) cret_stash_offset = place8();
                const auto &own_sig = module.signatures[fn.signature];
                if (own_sig.c_ret_words == 2) {
                    place8();
                    cret_blob_offset = place8(); // 16 contiguous bytes
                }
                if (own_sig.c_sret) sret_stash_offset = place8();

                frame_size = (offset + 15) / 16 * 16;
            }

            // ---- sweep ----------------------------------------------------------
            void advance_to(const uint32_t index) {
                current_index = index;
                const auto low = x86ra::pos_e(index);
                std::erase_if(active, [&](const ActiveInterval &a) { return a.end < low; });
                const auto limit = x86ra::pos_l(index);
                while (next_interval < ra.intervals.size() &&
                       ra.intervals[next_interval].start <= limit) {
                    const auto &interval = ra.intervals[next_interval++];
                    const auto &as = ra.values[interval.value];
                    if (!as.spilled) {
                        active.push_back({interval.start, interval.end, interval.value,
                                          as.reg, as.save_around_calls});
                    }
                }
            }

            [[nodiscard]] auto occupied(const x86ra::PhysReg reg) const -> bool {
                if (lease_mask & (1u << reg)) return true;
                for (const auto &a : active) {
                    if (a.reg == reg) return true;
                }
                return false;
            }

            // ---- dynamic scratch -------------------------------------------------
            // A leased register is dead at the current instruction, or failing that
            // a pushed victim restored on release. Templates that lease and also
            // move RSP are forbidden (StackAlloc uses its statically killed RAX).
            struct Lease {
                Reg reg = Reg::RAX;
                bool pushed = false;
                bool held = false;
            };
            struct XLease {
                XReg reg = XReg::XMM0;
                bool saved = false;
                bool held = false;
            };

            auto lease_gpr(const uint32_t exclude) -> Lease {
                static const Reg CANDIDATES[] = {Reg::R10, Reg::R11, Reg::RAX, Reg::RCX,
                                                  Reg::RDX, Reg::RSI, Reg::RDI, Reg::R8,
                                                  Reg::R9,  Reg::RBX, Reg::R12, Reg::R13,
                                                  Reg::R14, Reg::R15};
                for (const auto reg : CANDIDATES) {
                    if ((exclude & bit(reg)) || occupied(static_cast<x86ra::PhysReg>(reg))) continue;
                    lease_mask |= bit(reg);
                    return {reg, false, true};
                }
                for (const auto reg : CANDIDATES) {
                    if (exclude & bit(reg)) continue;
                    enc.push_r(reg);
                    lease_mask |= bit(reg);
                    return {reg, true, true};
                }
                error("internal error: no scratch register available");
                return {Reg::RAX, false, false};
            }
            void release(Lease &lease) {
                if (!lease.held) return;
                lease_mask &= ~bit(lease.reg);
                if (lease.pushed) enc.pop_r(lease.reg);
                lease.held = false;
            }

            auto lease_xmm(const uint32_t exclude) -> XLease {
                for (int i = 15; i >= 0; --i) {
                    const auto reg = static_cast<XReg>(i);
                    if ((exclude & bit(reg)) ||
                        occupied(static_cast<x86ra::PhysReg>(x86ra::FIRST_XMM + i))) continue;
                    lease_mask |= bit(reg);
                    return {reg, false, true};
                }
                // Park a victim in a fresh stack cell; no call intervenes before
                // release, so the momentary RSP adjustment is invisible.
                for (int i = 15; i >= 0; --i) {
                    const auto reg = static_cast<XReg>(i);
                    if (exclude & bit(reg)) continue;
                    enc.sub_rsp(16);
                    enc.movsd_store(Reg::RSP, 0, reg);
                    lease_mask |= bit(reg);
                    return {reg, true, true};
                }
                error("internal error: no scratch XMM register available");
                return {XReg::XMM0, false, false};
            }
            void release(XLease &lease) {
                if (!lease.held) return;
                lease_mask &= ~bit(lease.reg);
                if (lease.saved) {
                    enc.movsd_load(lease.reg, Reg::RSP, 0);
                    enc.add_rsp(16);
                }
                lease.held = false;
            }

            // Exclusion mask of the registers the named values are assigned to.
            [[nodiscard]] auto excl(const std::initializer_list<mir::ValueId> values) const -> uint32_t {
                uint32_t mask = 0;
                for (const auto v : values) {
                    if (v != mir::NO_VALUE && in_reg(v)) mask |= 1u << assignment(v).reg;
                }
                return mask;
            }

            // ---- value movement --------------------------------------------------
            // Copy an integer-class value into a specific register, canonical form.
            void gpr_into(const mir::ValueId v, const Reg dst) {
                if (in_reg(v)) {
                    if (val_gpr(v) != dst) enc.mov_rr(Width::W64, dst, val_gpr(v));
                } else {
                    enc.load(Width::W64, dst, Reg::RBP, val_off(v));
                }
            }
            // Sign-extended read at the value's own width (trivial's load_signed).
            void gpr_into_signed(const mir::ValueId v, const Reg dst) {
                const auto width = width_of(fn.values[v].type);
                gpr_into(v, dst);
                if (width != Width::W64) enc.movsx(width, dst, dst);
            }
            // Returns a register currently holding v; loads into 'lease' if spilled.
            auto use_gpr(const mir::ValueId v, const uint32_t extra_exclude) -> std::pair<Reg, Lease> {
                if (in_reg(v)) return {val_gpr(v), Lease{}};
                auto lease = lease_gpr(extra_exclude);
                enc.load(Width::W64, lease.reg, Reg::RBP, val_off(v));
                return {lease.reg, lease};
            }
            void xmm_into(const mir::ValueId v, const XReg dst) {
                if (in_reg(v)) {
                    if (val_xmm(v) != dst) enc.movaps(dst, val_xmm(v));
                } else if (is_double(v)) {
                    enc.movsd_load(dst, Reg::RBP, val_off(v));
                } else {
                    enc.movss_load(dst, Reg::RBP, val_off(v));
                }
            }
            auto use_xmm(const mir::ValueId v, const uint32_t extra_exclude) -> std::pair<XReg, XLease> {
                if (in_reg(v)) return {val_xmm(v), XLease{}};
                auto lease = lease_xmm(extra_exclude);
                xmm_into(v, lease.reg);
                return {lease.reg, lease};
            }

            // Target register for an integer result: the assigned one, or a scratch
            // to compute in before commit_gpr stores it.
            auto def_gpr(const mir::ValueId v, const uint32_t exclude) -> std::pair<Reg, Lease> {
                if (in_reg(v)) return {val_gpr(v), Lease{}};
                auto lease = lease_gpr(exclude);
                return {lease.reg, lease};
            }
            auto def_xmm(const mir::ValueId v, const uint32_t exclude) -> std::pair<XReg, XLease> {
                if (in_reg(v)) return {val_xmm(v), XLease{}};
                auto lease = lease_xmm(exclude);
                return {lease.reg, lease};
            }

            // Re-establish canonical (zero-extended) form at the type's width.
            void canonicalize(const Reg reg, const mir::Ty type) {
                switch (width_of(type)) {
                case Width::W8:
                case Width::W16:
                    enc.movzx(width_of(type), reg, reg);
                    break;
                case Width::W32:
                    enc.mov_rr(Width::W32, reg, reg); // mov r32, r32 zero-extends
                    break;
                case Width::W64:
                    break;
                }
            }

            void commit_gpr(const mir::ValueId v, const Reg computed) {
                if (in_reg(v)) {
                    if (val_gpr(v) != computed) enc.mov_rr(Width::W64, val_gpr(v), computed);
                } else {
                    enc.store(Width::W64, Reg::RBP, val_off(v), computed);
                }
            }
            void commit_xmm(const mir::ValueId v, const XReg computed) {
                if (in_reg(v)) {
                    if (val_xmm(v) != computed) enc.movaps(val_xmm(v), computed);
                } else if (is_double(v)) {
                    enc.movsd_store(Reg::RBP, val_off(v), computed);
                } else {
                    enc.movss_store(Reg::RBP, val_off(v), computed);
                }
            }

            // ---- constants and addresses -----------------------------------------
            void materialize(const mir::Inst &inst) {
                if (inst.op == mir::Op::ConstFloat) {
                    // Raw bits ride a GPR into either an XMM home or the spill area.
                    auto lease = lease_gpr(0);
                    enc.mov_ri(lease.reg, inst.imm);
                    if (in_reg(inst.result)) {
                        enc.mov_r_x(val_xmm(inst.result), lease.reg);
                    } else if (inst.type == mir::Ty::F64) {
                        enc.store(Width::W64, Reg::RBP, val_off(inst.result), lease.reg);
                    } else {
                        enc.store(Width::W32, Reg::RBP, val_off(inst.result), lease.reg);
                    }
                    release(lease);
                    return;
                }
                auto [dst, lease] = def_gpr(inst.result, 0);
                switch (inst.op) {
                case mir::Op::ConstInt: {
                    // Canonical form is decided here, at materialization: the
                    // immediate is masked to the type's width, zero-extended.
                    const auto bits = mir::type_bits(inst.type, module.pointer_bits);
                    auto value = static_cast<uint64_t>(inst.imm);
                    if (bits != 0 && bits < 64) value &= (uint64_t{1} << bits) - 1;
                    enc.mov_ri(dst, static_cast<int64_t>(value));
                    break;
                }
                case mir::Op::ConstNull:
                    enc.zero(dst);
                    break;
                case mir::Op::GlobalAddr:
                    enc.lea_rip(dst, global_symbols[inst.a], 0);
                    break;
                case mir::Op::FuncAddr:
                    enc.lea_rip(dst, function_symbols[inst.a], 0);
                    break;
                case mir::Op::SlotAddr:
                    enc.lea(dst, Reg::RBP, slot_offset[inst.a]);
                    break;
                default:
                    break;
                }
                commit_gpr(inst.result, dst);
                release(lease);
            }

            // ---- calls -----------------------------------------------------------
            [[nodiscard]] auto crosses_current_call(const ActiveInterval &a) const -> bool {
                return a.start < x86ra::pos_u(current_index) &&
                       a.end > x86ra::pos_l(current_index);
            }

            void call_saves() {
                for (const auto &a : active) {
                    if (!a.save_around || !crosses_current_call(a)) continue;
                    const auto off = area_offset[ra.values[a.value].save_index];
                    if (x86ra::is_xmm(a.reg)) enc.movsd_store(Reg::RBP, off, xreg(a.reg));
                    else enc.store(Width::W64, Reg::RBP, off, greg(a.reg));
                }
            }
            void call_restores() {
                for (const auto &a : active) {
                    if (!a.save_around || !crosses_current_call(a)) continue;
                    const auto off = area_offset[ra.values[a.value].save_index];
                    if (x86ra::is_xmm(a.reg)) enc.movsd_load(xreg(a.reg), Reg::RBP, off);
                    else enc.load(Width::W64, greg(a.reg), Reg::RBP, off);
                }
            }

            // Where to read a value from inside a call template: its callee-saved
            // register, or a frame offset (its spill area, or — for a caller-saved
            // save-around value whose register the template may already have
            // clobbered — its save area, written by call_saves() above).
            struct CallOperand {
                bool mem = false;
                x86ra::PhysReg reg = x86ra::NO_PHYS;
                int32_t off = 0;
            };
            [[nodiscard]] auto call_operand(const mir::ValueId v) const -> CallOperand {
                const auto &as = assignment(v);
                if (as.spilled) return {.mem = true, .off = area_offset[as.spill_index]};
                if (as.save_around_calls) {
                    for (const auto &a : active) {
                        if (a.value == v && crosses_current_call(a)) {
                            return {.mem = true, .off = area_offset[as.save_index]};
                        }
                    }
                }
                return {.mem = false, .reg = as.reg};
            }
            void call_arg_into_gpr(const mir::ValueId v, const Reg dst) {
                const auto operand = call_operand(v);
                if (operand.mem) enc.load(Width::W64, dst, Reg::RBP, operand.off);
                else if (greg(operand.reg) != dst) enc.mov_rr(Width::W64, dst, greg(operand.reg));
            }
            void call_arg_into_xmm(const mir::ValueId v, const XReg dst) {
                const auto operand = call_operand(v);
                if (!operand.mem) {
                    if (xreg(operand.reg) != dst) enc.movaps(dst, xreg(operand.reg));
                } else if (is_double(v)) {
                    enc.movsd_load(dst, Reg::RBP, operand.off);
                } else {
                    enc.movss_load(dst, Reg::RBP, operand.off);
                }
            }

            void emit_call(const mir::Inst &inst, const bool indirect) {
                const auto signature = indirect ? inst.b : module.functions[inst.a].signature;
                const auto &sig = module.signatures[signature];
                call_saves();

                std::vector<mir::ValueId> args(inst.args.begin(), inst.args.end());

                // A two-eightbyte C return: the trailing MIR argument is the
                // out-pointer, never passed physically. Stash it now, while its
                // location is still valid, and store the return registers through
                // it after the call.
                const bool two_word_ret = sig.c_ret_words == 2;
                if (two_word_ret && !args.empty()) {
                    call_arg_into_gpr(args.back(), Reg::RAX);
                    enc.store(Width::W64, Reg::RBP, cret_stash_offset, Reg::RAX);
                    args.pop_back();
                }

                const auto byval_of = [&](const size_t i) -> uint32_t {
                    return i < sig.byval_sizes.size() ? sig.byval_sizes[i] : 0;
                };

                // Classify: SysV MEMORY-class (byval) arguments always go to the
                // stack, sized and aligned as the aggregate; everything else takes
                // the next register of its class or an 8-byte stack cell.
                std::vector<int> int_slot(args.size(), -1);
                std::vector<int> float_slot(args.size(), -1);
                std::vector<int32_t> stack_off(args.size(), -1);
                int ints = 0;
                int floats = 0;
                int32_t stack_bytes = 0;
                for (size_t i = 0; i < args.size(); ++i) {
                    if (const auto byval = byval_of(i)) {
                        const auto align = std::max<int32_t>(
                            8, i < sig.byval_aligns.size()
                                   ? static_cast<int32_t>(sig.byval_aligns[i]) : 8);
                        stack_bytes = (stack_bytes + align - 1) / align * align;
                        stack_off[i] = stack_bytes;
                        stack_bytes += static_cast<int32_t>((byval + 7) / 8 * 8);
                    } else if (mir::is_float(fn.values[args[i]].type)) {
                        if (floats < 8) float_slot[i] = floats++;
                        else { stack_off[i] = stack_bytes; stack_bytes += 8; }
                    } else {
                        if (ints < 6) int_slot[i] = ints++;
                        else { stack_off[i] = stack_bytes; stack_bytes += 8; }
                    }
                }

                // Stack arguments through an aligned reservation, staged via RAX
                // (statically killed at every call); byval aggregates copy in
                // 8-byte chunks through R11, whose only later use (the indirect
                // callee) comes after this loop.
                const auto reserve = (stack_bytes + 15) / 16 * 16;
                if (reserve > 0) enc.sub_rsp(reserve);
                for (size_t i = 0; i < args.size(); ++i) {
                    if (stack_off[i] < 0) continue;
                    if (const auto byval = byval_of(i)) {
                        call_arg_into_gpr(args[i], Reg::R11);
                        const auto rounded = static_cast<int32_t>((byval + 7) / 8 * 8);
                        for (int32_t chunk = 0; chunk < rounded; chunk += 8) {
                            enc.load(Width::W64, Reg::RAX, Reg::R11, chunk);
                            enc.store(Width::W64, Reg::RSP, stack_off[i] + chunk, Reg::RAX);
                        }
                    } else {
                        call_arg_into_gpr(args[i], Reg::RAX);
                        enc.store(Width::W64, Reg::RSP, stack_off[i], Reg::RAX);
                    }
                }

                // An indirect target goes to R11 BEFORE the argument registers are
                // live; R11 is caller-saved and never an argument register.
                if (indirect) call_arg_into_gpr(inst.a, Reg::R11);

                for (size_t i = 0; i < args.size(); ++i) {
                    if (float_slot[i] >= 0) {
                        call_arg_into_xmm(args[i], static_cast<XReg>(float_slot[i]));
                    } else if (int_slot[i] >= 0) {
                        call_arg_into_gpr(args[i], INT_ARG_REGS[int_slot[i]]);
                    }
                }

                if (sig.is_variadic) enc.mov_ri(Reg::RAX, floats); // AL = vector count

                if (indirect) enc.call_r(Reg::R11);
                else enc.call_sym(function_symbols[inst.a]);

                if (reserve > 0) enc.add_rsp(reserve);

                if (inst.result != mir::NO_VALUE) {
                    if (mir::is_float(fn.values[inst.result].type)) {
                        commit_xmm(inst.result, XReg::XMM0);
                    } else {
                        canonicalize(Reg::RAX, inst.type);
                        commit_gpr(inst.result, Reg::RAX);
                    }
                }
                if (two_word_ret) {
                    enc.load(Width::W64, Reg::R11, Reg::RBP, cret_stash_offset);
                    if (sig.c_ret_sse[0]) enc.movsd_store(Reg::R11, 0, XReg::XMM0);
                    else enc.store(Width::W64, Reg::R11, 0, Reg::RAX);
                    if (sig.c_ret_sse[1]) enc.movsd_store(Reg::R11, 8, XReg::XMM1);
                    else enc.store(Width::W64, Reg::R11, 8, Reg::RDX);
                }
                call_restores();
            }

            // libc memcpy/memset carry the mem.copy/mem.set ops. These are calls:
            // saves/restores and the caller-saved argument registers apply exactly
            // as for a user call.
            void emit_mem_op(const mir::Inst &inst, const bool is_set) {
                call_saves();
                call_arg_into_gpr(inst.a, Reg::RDI);
                call_arg_into_gpr(inst.b, Reg::RSI);
                call_arg_into_gpr(inst.c, Reg::RDX);
                enc.call_sym(is_set ? memset_symbol : memcpy_symbol);
                call_restores();
            }

            void emit_frem(const mir::Inst &inst) {
                const bool dbl = inst.type == mir::Ty::F64;
                call_saves();
                call_arg_into_xmm(inst.a, XReg::XMM0);
                call_arg_into_xmm(inst.b, XReg::XMM1);
                enc.mov_ri(Reg::RAX, 2);
                enc.call_sym(dbl ? fmod_symbol : fmodf_symbol);
                commit_xmm(inst.result, XReg::XMM0);
                call_restores();
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
                const auto width = width_of(fn.values[inst.a].type) == Width::W64
                    ? Width::W64 : Width::W32;
                auto [ar, alease] = use_gpr(inst.a, excl({inst.b, inst.result}));
                if (in_reg(inst.b)) {
                    enc.alu_rr(Alu::Cmp, width, ar, val_gpr(inst.b));
                } else {
                    enc.alu_rm(Alu::Cmp, width, ar, Reg::RBP, val_off(inst.b));
                }
                release(alease);
                auto [dst, dlease] = def_gpr(inst.result, excl({inst.a, inst.b}));
                enc.setcc(icmp_cond(inst.op), dst);
                enc.movzx(Width::W8, dst, dst);
                commit_gpr(inst.result, dst);
                release(dlease);
            }

            void emit_fcmp(const mir::Inst &inst) {
                const bool dbl = fn.values[inst.a].type == mir::Ty::F64;
                // Olt/Ole compare through swapped operands so the ABOVE-family
                // conditions serve all four orderings; unordered then reads false
                // for every one except equality, which needs the parity fixup.
                const bool swap = inst.op == mir::Op::FCmpOlt || inst.op == mir::Op::FCmpOle;
                const auto first = swap ? inst.b : inst.a;
                const auto second = swap ? inst.a : inst.b;

                auto [fx, flease] = use_xmm(first, excl({second}));
                if (in_reg(second)) {
                    enc.ucomis(dbl, fx, val_xmm(second));
                } else {
                    enc.ucomis_m(dbl, fx, Reg::RBP, val_off(second));
                }
                release(flease);

                auto [dst, dlease] = def_gpr(inst.result, 0);
                switch (inst.op) {
                case mir::Op::FCmpOeq: {
                    auto parity = lease_gpr(bit(dst));
                    enc.setcc(Cond::E, dst);
                    enc.setcc(Cond::NP, parity.reg);
                    enc.alu_rr(Alu::And, Width::W8, dst, parity.reg);
                    release(parity);
                    break;
                }
                case mir::Op::FCmpOne: enc.setcc(Cond::NE, dst); break;
                case mir::Op::FCmpOlt:
                case mir::Op::FCmpOgt: enc.setcc(Cond::A, dst); break;
                case mir::Op::FCmpOle:
                case mir::Op::FCmpOge: enc.setcc(Cond::AE, dst); break;
                default: enc.setcc(Cond::E, dst); break;
                }
                enc.movzx(Width::W8, dst, dst);
                commit_gpr(inst.result, dst);
                release(dlease);
            }

            // ---- inline asm ------------------------------------------------------
            // Unchanged in substance from stage 5: every variable operand is a
            // pointer to a pinned FRAME SLOT, so an asm instruction encodes against
            // [rbp - offset] with no constraint model. The allocator reserved every
            // register the block names, so nothing live is sitting in them.
            struct AsmArg {
                bool is_memory = false;
                int32_t disp = 0;
                Reg reg = Reg::RAX;
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

            // The rbp offset a variable operand's pointer refers to. Only a slot
            // address qualifies — which is all mirgen ever produces for one.
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

                // 'asm -> reg': that register's value AT BLOCK EXIT is the result.
                if (inst.result == mir::NO_VALUE) return;
                const auto reg = reg_by_name(block.result_register);
                if (!reg) {
                    error(std::format("inline asm: unsupported result register '{}'",
                                       block.result_register));
                    return;
                }
                canonicalize(*reg, inst.type);
                commit_gpr(inst.result, *reg);
            }

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

                // Loud refusal, naming the instruction: an asm block the encoder
                // cannot render must never be silently dropped from the output.
                error(std::format("inline asm: cannot encode '{}' with {} operand(s) in this form "
                                   "(line {})", m, args.size(), instruction.line));
            }

            // ---- block arguments -------------------------------------------------
            void pass_block_args(const mir::BlockId target, const std::vector<uint32_t> &args) {
                const auto &params = fn.blocks[target].params;
                // Phase 1: every argument into its parameter's staging slot.
                for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
                    const auto arg = args[i];
                    const auto staging = staging_offset.at(params[i]);
                    if (in_reg(arg) && mir::is_float(fn.values[arg].type)) {
                        enc.movsd_store(Reg::RBP, staging, val_xmm(arg));
                    } else if (in_reg(arg)) {
                        enc.store(Width::W64, Reg::RBP, staging, val_gpr(arg));
                    } else {
                        auto lease = lease_gpr(0);
                        enc.load(Width::W64, lease.reg, Reg::RBP, val_off(arg));
                        enc.store(Width::W64, Reg::RBP, staging, lease.reg);
                        release(lease);
                    }
                }
                // Phase 2: staging into the parameters' homes.
                for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
                    load_param_from_staging(params[i]);
                }
            }

            void load_param_from_staging(const mir::ValueId param) {
                const auto staging = staging_offset.at(param);
                if (in_reg(param)) {
                    if (mir::is_float(fn.values[param].type)) {
                        if (is_double(param)) enc.movsd_load(val_xmm(param), Reg::RBP, staging);
                        else enc.movss_load(val_xmm(param), Reg::RBP, staging);
                    } else {
                        enc.load(Width::W64, val_gpr(param), Reg::RBP, staging);
                    }
                } else {
                    auto lease = lease_gpr(0);
                    enc.load(Width::W64, lease.reg, Reg::RBP, staging);
                    enc.store(Width::W64, Reg::RBP, val_off(param), lease.reg);
                    release(lease);
                }
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
                    if (mir::is_float(inst.type)) {
                        auto [addr, alease] = use_gpr(inst.a, excl({inst.result}));
                        auto [dst, dlease] = def_xmm(inst.result, 0);
                        if (inst.type == mir::Ty::F64) enc.movsd_load(dst, addr, 0);
                        else enc.movss_load(dst, addr, 0);
                        commit_xmm(inst.result, dst);
                        release(dlease);
                        release(alease);
                        return;
                    }
                    auto [addr, alease] = use_gpr(inst.a, excl({inst.result}));
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}) | bit(addr));
                    const auto width = width_of(inst.type);
                    enc.load(width, dst, addr, 0);
                    if (width == Width::W8 || width == Width::W16) enc.movzx(width, dst, dst);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    release(alease);
                    return;
                }
                case Op::Store: {
                    if (mir::is_float(fn.values[inst.b].type)) {
                        auto [addr, alease] = use_gpr(inst.a, excl({inst.b}));
                        auto [src, slease] = use_xmm(inst.b, 0);
                        if (is_double(inst.b)) enc.movsd_store(addr, 0, src);
                        else enc.movss_store(addr, 0, src);
                        release(slease);
                        release(alease);
                        return;
                    }
                    auto [addr, alease] = use_gpr(inst.a, excl({inst.b}));
                    auto [src, slease] = use_gpr(inst.b, excl({inst.a}) | bit(addr));
                    enc.store(width_of(fn.values[inst.b].type), addr, 0, src);
                    release(slease);
                    release(alease);
                    return;
                }
                case Op::MemCopy: emit_mem_op(inst, false); return;
                case Op::MemSet: emit_mem_op(inst, true); return;

                case Op::StackAlloc: {
                    // Extend the frame by a 16-aligned amount and hand back the new
                    // top. RAX is statically killed here — a dynamic lease could
                    // push/pop, which must never straddle an RSP adjustment.
                    gpr_into(inst.a, Reg::RAX);
                    enc.alu_ri(Alu::Add, Width::W64, Reg::RAX, 15);
                    enc.alu_ri(Alu::And, Width::W64, Reg::RAX, -16);
                    enc.alu_rr(Alu::Sub, Width::W64, Reg::RSP, Reg::RAX);
                    enc.alu_ri(Alu::And, Width::W64, Reg::RSP, -16);
                    enc.mov_rr(Width::W64, Reg::RAX, Reg::RSP);
                    commit_gpr(inst.result, Reg::RAX);
                    return;
                }

                case Op::PtrAddConst: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    if (in_reg(inst.a)) {
                        enc.lea(dst, val_gpr(inst.a), static_cast<int32_t>(inst.imm));
                    } else {
                        enc.load(Width::W64, dst, Reg::RBP, val_off(inst.a));
                        enc.lea(dst, dst, static_cast<int32_t>(inst.imm));
                    }
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::PtrAdd: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a, inst.b}));
                    gpr_into(inst.a, dst);
                    if (in_reg(inst.b)) enc.alu_rr(Alu::Add, Width::W64, dst, val_gpr(inst.b));
                    else enc.alu_rm(Alu::Add, Width::W64, dst, Reg::RBP, val_off(inst.b));
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }

                case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: {
                    const auto alu = inst.op == Op::Add ? Alu::Add
                                   : inst.op == Op::Sub ? Alu::Sub
                                   : inst.op == Op::And ? Alu::And
                                   : inst.op == Op::Or  ? Alu::Or : Alu::Xor;
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a, inst.b}));
                    gpr_into(inst.a, dst);
                    if (in_reg(inst.b)) enc.alu_rr(alu, Width::W64, dst, val_gpr(inst.b));
                    else enc.alu_rm(alu, Width::W64, dst, Reg::RBP, val_off(inst.b));
                    // And/Or/Xor of canonical operands stay canonical; Add/Sub can
                    // carry into the bits above a narrow type's width.
                    if (inst.op == Op::Add || inst.op == Op::Sub) canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::Mul: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a, inst.b}));
                    gpr_into(inst.a, dst);
                    if (in_reg(inst.b)) enc.imul_rr(dst, val_gpr(inst.b));
                    else enc.imul_rm(dst, Reg::RBP, val_off(inst.b));
                    canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::SDiv: case Op::SRem: {
                    // RAX/RDX are statically killed. Operands are sign-extended
                    // from their width — the canonical form is zero-extended, and a
                    // spilled slot holds that zero-extended form, so idiv_m's
                    // 64-bit memory read would be wrong for a narrow operand.
                    gpr_into_signed(inst.a, Reg::RAX);
                    enc.cqo();
                    const auto b_width = width_of(fn.values[inst.b].type);
                    if (in_reg(inst.b) && b_width == Width::W64) {
                        enc.idiv_r(val_gpr(inst.b));
                    } else if (!in_reg(inst.b) && b_width == Width::W64) {
                        enc.idiv_m(Reg::RBP, val_off(inst.b));
                    } else {
                        auto lease = lease_gpr(excl({inst.a, inst.b, inst.result}) |
                                                bit(Reg::RAX) | bit(Reg::RDX));
                        gpr_into_signed(inst.b, lease.reg);
                        enc.idiv_r(lease.reg);
                        release(lease);
                    }
                    const auto out = inst.op == Op::SDiv ? Reg::RAX : Reg::RDX;
                    canonicalize(out, inst.type);
                    commit_gpr(inst.result, out);
                    return;
                }
                case Op::UDiv: case Op::URem: {
                    gpr_into(inst.a, Reg::RAX);
                    enc.zero(Reg::RDX);
                    if (in_reg(inst.b)) enc.div_r(val_gpr(inst.b));
                    else enc.div_m(Reg::RBP, val_off(inst.b));
                    commit_gpr(inst.result, inst.op == Op::UDiv ? Reg::RAX : Reg::RDX);
                    return;
                }
                case Op::Shl: case Op::LShr: case Op::AShr: {
                    // RCX is statically killed for the count.
                    gpr_into(inst.b, Reg::RCX);
                    auto [dst, dlease] = def_gpr(inst.result,
                                                  excl({inst.a, inst.b}) | bit(Reg::RCX));
                    if (inst.op == Op::AShr) gpr_into_signed(inst.a, dst);
                    else gpr_into(inst.a, dst);
                    if (inst.op == Op::Shl) enc.shl_cl(Width::W64, dst);
                    else if (inst.op == Op::LShr) enc.shr_cl(Width::W64, dst);
                    else enc.sar_cl(Width::W64, dst);
                    if (inst.op != Op::LShr) canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::Not: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    gpr_into(inst.a, dst);
                    if (inst.type == mir::Ty::I1) {
                        enc.alu_ri(Alu::Xor, Width::W8, dst, 1);
                    } else {
                        enc.not_r(Width::W64, dst);
                        canonicalize(dst, inst.type);
                    }
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::Neg: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    gpr_into(inst.a, dst);
                    enc.neg_r(Width::W64, dst);
                    canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }

                case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: {
                    const bool dbl = inst.type == mir::Ty::F64;
                    const uint8_t opcode = inst.op == Op::FAdd ? 0x58
                                          : inst.op == Op::FSub ? 0x5C
                                          : inst.op == Op::FMul ? 0x59 : 0x5E;
                    auto [dst, dlease] = def_xmm(inst.result, excl({inst.a, inst.b}));
                    xmm_into(inst.a, dst);
                    if (in_reg(inst.b)) enc.sse_arith(opcode, dbl, dst, val_xmm(inst.b));
                    else enc.sse_arith_m(opcode, dbl, dst, Reg::RBP, val_off(inst.b));
                    commit_xmm(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::FRem: emit_frem(inst); return;
                case Op::FNeg: {
                    // Flip the sign bit through a GPR: cheap, and works identically
                    // for register-resident and spilled operands.
                    const bool dbl = inst.type == mir::Ty::F64;
                    auto lease = lease_gpr(0);
                    if (in_reg(inst.a)) {
                        enc.mov_x_r(lease.reg, val_xmm(inst.a));
                    } else {
                        enc.load(width_of(fn.values[inst.a].type), lease.reg, Reg::RBP,
                                  val_off(inst.a));
                    }
                    auto mask = lease_gpr(bit(lease.reg));
                    enc.mov_ri(mask.reg, dbl ? static_cast<int64_t>(0x8000000000000000ULL)
                                              : 0x80000000LL);
                    enc.alu_rr(Alu::Xor, Width::W64, lease.reg, mask.reg);
                    release(mask);
                    if (in_reg(inst.result)) {
                        enc.mov_r_x(val_xmm(inst.result), lease.reg);
                    } else {
                        enc.store(dbl ? Width::W64 : Width::W32, Reg::RBP,
                                   val_off(inst.result), lease.reg);
                    }
                    release(lease);
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

                case Op::Trunc: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    gpr_into(inst.a, dst);
                    canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::ZExt: case Op::PtrToInt: case Op::IntToPtr: {
                    // The operand is already canonical (zero-extended); the move is
                    // the whole operation.
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    gpr_into(inst.a, dst);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::SExt: {
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    gpr_into_signed(inst.a, dst);
                    canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::Bitcast: {
                    // Same-width reinterpretation, possibly across classes. Route
                    // the raw bits through a GPR; the source width picks the load.
                    const bool src_float = mir::is_float(fn.values[inst.a].type);
                    const bool dst_float = mir::is_float(inst.type);
                    auto lease = lease_gpr(excl({inst.a, inst.result}));
                    if (src_float && in_reg(inst.a)) {
                        enc.mov_x_r(lease.reg, val_xmm(inst.a));
                        if (fn.values[inst.a].type == mir::Ty::F32) canonicalize(lease.reg, mir::Ty::I32);
                    } else {
                        gpr_into(inst.a, lease.reg);
                        if (!in_reg(inst.a) && src_float &&
                            fn.values[inst.a].type == mir::Ty::F32) {
                            canonicalize(lease.reg, mir::Ty::I32);
                        }
                    }
                    if (dst_float) {
                        if (in_reg(inst.result)) {
                            enc.mov_r_x(val_xmm(inst.result), lease.reg);
                        } else {
                            enc.store(inst.type == mir::Ty::F64 ? Width::W64 : Width::W32,
                                       Reg::RBP, val_off(inst.result), lease.reg);
                        }
                    } else {
                        commit_gpr(inst.result, lease.reg);
                    }
                    release(lease);
                    return;
                }
                case Op::FPTrunc: case Op::FPExt: {
                    const bool to_double = inst.type == mir::Ty::F64;
                    auto [dst, dlease] = def_xmm(inst.result, excl({inst.a}));
                    xmm_into(inst.a, dst);
                    enc.cvt_f2f(to_double, dst, dst);
                    commit_xmm(inst.result, dst);
                    release(dlease);
                    return;
                }
                case Op::SIToFP: case Op::UIToFP: {
                    // The unsigned 64-bit edge (values >= 2^63) is knowingly emitted
                    // as the signed conversion; narrower unsigned sources are already
                    // zero-extended, which makes the signed instruction exact.
                    auto lease = lease_gpr(excl({inst.a, inst.result}));
                    if (inst.op == Op::SIToFP) gpr_into_signed(inst.a, lease.reg);
                    else gpr_into(inst.a, lease.reg);
                    auto [dst, dlease] = def_xmm(inst.result, 0);
                    enc.cvt_i2f(inst.type == mir::Ty::F64, Width::W64, dst, lease.reg);
                    commit_xmm(inst.result, dst);
                    release(dlease);
                    release(lease);
                    return;
                }
                case Op::FPToSI: case Op::FPToUI: {
                    const bool from_double = fn.values[inst.a].type == mir::Ty::F64;
                    auto [src, slease] = use_xmm(inst.a, 0);
                    auto [dst, dlease] = def_gpr(inst.result, excl({inst.a}));
                    enc.cvt_f2i(from_double, Width::W64, dst, src);
                    canonicalize(dst, inst.type);
                    commit_gpr(inst.result, dst);
                    release(dlease);
                    release(slease);
                    return;
                }

                case Op::Select: {
                    // Branchy on purpose: both classes share one shape, and the
                    // untaken side's operand is simply never read.
                    auto [cond, clease] = use_gpr(inst.a, excl({inst.b, inst.c, inst.result}));
                    const auto skip = enc.make_label();
                    const auto done = enc.make_label();
                    enc.test_rr(Width::W8, cond, cond);
                    release(clease);
                    if (mir::is_float(inst.type)) {
                        auto [dst, dlease] = def_xmm(inst.result, excl({inst.b, inst.c}));
                        enc.jcc(Cond::E, skip);
                        xmm_into(inst.b, dst);
                        enc.jmp(done);
                        enc.bind(skip);
                        xmm_into(inst.c, dst);
                        enc.bind(done);
                        commit_xmm(inst.result, dst);
                        release(dlease);
                    } else {
                        auto [dst, dlease] = def_gpr(inst.result,
                                                      excl({inst.a, inst.b, inst.c}));
                        enc.jcc(Cond::E, skip);
                        gpr_into(inst.b, dst);
                        enc.jmp(done);
                        enc.bind(skip);
                        gpr_into(inst.c, dst);
                        enc.bind(done);
                        commit_gpr(inst.result, dst);
                        release(dlease);
                    }
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
                    auto [cond, clease] = use_gpr(inst.a, 0);
                    enc.test_rr(Width::W8, cond, cond);
                    release(clease);
                    enc.jcc(Cond::NE, block_labels[inst.b]);
                    enc.jmp(block_labels[inst.c]);
                    return;
                }
                case Op::Switch: {
                    // Case values are stored 32-bit-truncated (Builder::switch_on)
                    // and read back sign-extended. A narrow scrutinee compares at 32
                    // bits on the RAW pattern — a 64-bit compare would disagree
                    // about negative values, since the canonical form zero-extends.
                    // RAX/RCX are statically killed here: the compare loop branches
                    // away mid-template, so leased scratch (whose push/pop must stay
                    // balanced) cannot be used.
                    const bool wide = width_of(fn.values[inst.a].type) == Width::W64;
                    const auto scrutinee = in_reg(inst.a) ? val_gpr(inst.a)
                        : (gpr_into(inst.a, Reg::RAX), Reg::RAX);
                    for (size_t i = 0; i + 1 < inst.args.size(); i += 2) {
                        if (wide) {
                            enc.mov_ri(Reg::RCX,
                                        static_cast<int64_t>(static_cast<int32_t>(inst.args[i])));
                            enc.alu_rr(Alu::Cmp, Width::W64, scrutinee, Reg::RCX);
                        } else {
                            enc.mov_ri(Reg::RCX, inst.args[i]); // raw 32-bit pattern
                            enc.alu_rr(Alu::Cmp, Width::W32, scrutinee, Reg::RCX);
                        }
                        enc.jcc(Cond::E, block_labels[inst.args[i + 1]]);
                    }
                    enc.jmp(block_labels[inst.b]);
                    return;
                }
                case Op::Return: {
                    if (!inst.args.empty()) {
                        const auto value = inst.args.front();
                        if (mir::is_float(fn.values[value].type)) {
                            xmm_into(value, XReg::XMM0);
                        } else {
                            gpr_into(value, Reg::RAX);
                        }
                    }
                    // C-ABI return shapes for this function itself: a two-word
                    // return loads rax/rdx (or xmm0/xmm1) from the blob its
                    // synthetic out-param pointed at; the sret convention hands
                    // the hidden pointer back in RAX.
                    const auto &own_sig = module.signatures[fn.signature];
                    if (own_sig.c_ret_words == 2) {
                        if (own_sig.c_ret_sse[0]) enc.movsd_load(XReg::XMM0, Reg::RBP, cret_blob_offset);
                        else enc.load(Width::W64, Reg::RAX, Reg::RBP, cret_blob_offset);
                        if (own_sig.c_ret_sse[1]) enc.movsd_load(XReg::XMM1, Reg::RBP, cret_blob_offset + 8);
                        else enc.load(Width::W64, Reg::RDX, Reg::RBP, cret_blob_offset + 8);
                    } else if (own_sig.c_sret) {
                        enc.load(Width::W64, Reg::RAX, Reg::RBP, sret_stash_offset);
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

            void emit_epilogue() {
                for (Reg r : {Reg::RBX, Reg::R12, Reg::R13, Reg::R14, Reg::R15}) {
                    if (ra.used_callee_saved & bit(r)) {
                        enc.load(Width::W64, r, Reg::RBP, callee_save_offset[static_cast<int>(r)]);
                    }
                }
                enc.mov_rr(Width::W64, Reg::RSP, Reg::RBP);
                enc.pop_r(Reg::RBP);
                enc.ret();
            }

            void emit() {
                layout_frame();
                for (size_t i = 0; i < fn.blocks.size(); ++i) {
                    block_labels.push_back(enc.make_label());
                }

                // '@naked': the body IS the function — no prologue, no parameter
                // spills, no frame. Its asm ends however the author ended it; a
                // Return op degrades to a bare 'ret'.
                if (fn.is_naked) {
                    uint32_t index = 0;
                    for (size_t b = 0; b < fn.blocks.size(); ++b) {
                        enc.bind(block_labels[b]);
                        for (const auto &inst : fn.blocks[b].insts) {
                            advance_to(index);
                            if (inst.op == mir::Op::Return) {
                                enc.ret();
                            } else {
                                emit_inst(inst, static_cast<mir::BlockId>(b));
                            }
                            ++index;
                        }
                    }
                    return;
                }

                enc.push_r(Reg::RBP);
                enc.mov_rr(Width::W64, Reg::RBP, Reg::RSP);
                if (frame_size > 0) enc.sub_rsp(frame_size);
                for (Reg r : {Reg::RBX, Reg::R12, Reg::R13, Reg::R14, Reg::R15}) {
                    if (ra.used_callee_saved & bit(r)) {
                        enc.store(Width::W64, Reg::RBP, callee_save_offset[static_cast<int>(r)], r);
                    }
                }

                // Incoming arguments. Phase 1 parks every register-passed argument
                // in memory (the parameter's spill area, or its staging slot when
                // it lives in a register), phase 2 stages stack-passed extras
                // through RAX, and phase 3 loads register-homed parameters — an
                // ordering that never reads an argument register after another
                // parameter's home register was written.
                int ints = 0;
                int floats = 0;
                int32_t caller_stack = 16; // saved RBP + return address
                const auto &own_sig = module.signatures[fn.signature];
                if (!fn.blocks.empty()) {
                    const auto &params = fn.blocks.front().params;
                    std::vector<mir::ValueId> reg_homed;
                    for (size_t i = 0; i < params.size(); ++i) {
                        const auto param = params[i];
                        const auto home = in_reg(param) ? staging_offset.at(param) : val_off(param);
                        if (in_reg(param)) reg_homed.push_back(param);
                        // C-ABI shapes first (mir.hpp Signature metadata): a byval
                        // parameter IS a pointer into the caller's stack area; the
                        // synthetic two-word-return out-pointer aims at this
                        // frame's own blob; the sret pointer is also stashed for
                        // the RAX-on-return rule.
                        const auto byval = i < own_sig.byval_sizes.size()
                            ? own_sig.byval_sizes[i] : 0;
                        if (byval != 0) {
                            const auto align = std::max<int32_t>(
                                8, i < own_sig.byval_aligns.size()
                                       ? static_cast<int32_t>(own_sig.byval_aligns[i]) : 8);
                            caller_stack = (caller_stack + align - 1) / align * align;
                            enc.lea(Reg::RAX, Reg::RBP, caller_stack);
                            caller_stack += static_cast<int32_t>((byval + 7) / 8 * 8);
                            enc.store(Width::W64, Reg::RBP, home, Reg::RAX);
                            continue;
                        }
                        if (own_sig.c_ret_words == 2 && i + 1 == params.size()) {
                            enc.lea(Reg::RAX, Reg::RBP, cret_blob_offset);
                            enc.store(Width::W64, Reg::RBP, home, Reg::RAX);
                            continue;
                        }
                        if (mir::is_float(fn.values[param].type)) {
                            if (floats < 8) {
                                enc.movsd_store(Reg::RBP, home, static_cast<XReg>(floats++));
                                continue;
                            }
                        } else if (ints < 6) {
                            if (own_sig.c_sret && i == 0) {
                                enc.store(Width::W64, Reg::RBP, sret_stash_offset,
                                           INT_ARG_REGS[0]);
                            }
                            enc.store(Width::W64, Reg::RBP, home, INT_ARG_REGS[ints++]);
                            continue;
                        }
                        enc.load(Width::W64, Reg::RAX, Reg::RBP, caller_stack);
                        caller_stack += 8;
                        enc.store(Width::W64, Reg::RBP, home, Reg::RAX);
                    }
                    for (const auto param : reg_homed) {
                        load_param_from_staging(param);
                    }
                }

                uint32_t index = 0;
                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    enc.bind(block_labels[b]);
                    for (const auto &inst : fn.blocks[b].insts) {
                        advance_to(index);
                        emit_inst(inst, static_cast<mir::BlockId>(b));
                        ++index;
                    }
                }
            }
        };
    }

    auto generate(const mir::Module &module, const uint32_t test_info,
                   const uint32_t test_runner, const RegAlloc regalloc) -> Result {
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
        const auto mode = regalloc == RegAlloc::Trivial ? x86ra::Mode::Trivial
                                                         : x86ra::Mode::Linear;
        // Debug bisection hook: functions at index >= MIRAGE_RA_LINEAR_LIMIT fall
        // back to the trivial allocator, which binary-searches a linear-scan
        // miscompile down to one function. Unset in normal operation.
        size_t linear_limit = SIZE_MAX;
        if (const char *env = std::getenv("MIRAGE_RA_LINEAR_LIMIT")) {
            linear_limit = static_cast<size_t>(std::strtoull(env, nullptr, 10));
        }
        size_t body_index = 0;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body) continue;

            const auto fn_mode = body_index++ < linear_limit ? mode : x86ra::Mode::Trivial;
            const auto allocation = x86ra::allocate(module, fn, fn_mode);
            if (!allocation.errors.empty()) {
                // The machine verifier found interference: an allocator bug. Never
                // emit from a failed allocation — wrong code is the one outcome
                // this backend must not produce.
                for (const auto &finding : allocation.errors) {
                    result.errors.push_back("register allocator: " + finding);
                }
                break;
            }

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
                .ra = allocation,
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
        const bool test_mode = test_info != UINT32_MAX && test_runner != UINT32_MAX;
        if ((main_index >= 0 || test_mode) && result.errors.empty()) {
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
            if (test_mode) {
                // 'mirage test': run the discovered tests and exit 0. 'main' is never
                // called even when the root module declares one.
                enc.lea_rip(Reg::RDI, global_symbols[test_info], 0);
                enc.call_sym(function_symbols[test_runner]);
                enc.zero(Reg::RDI);
                enc.call_sym(exit_symbol);
                enc.ud2();
                enc.resolve_labels();
                object.symbols.push_back({
                    .name = "_start", .section = elf::Section::Text, .value = start,
                    .size = enc.code.size(), .is_global = true, .is_function = true,
                });
                object.text.insert(object.text.end(), enc.code.begin(), enc.code.end());
                for (const auto &reloc : enc.relocations) {
                    object.relocations.push_back({
                        .in = elf::Section::Text, .offset = start + reloc.offset,
                        .symbol = reloc.symbol,
                        .type = reloc.kind == x86::Relocation::Kind::Call32
                            ? elf::R_X86_64_PLT32 : elf::R_X86_64_PC32,
                        .addend = reloc.addend + (reloc.kind == x86::Relocation::Kind::Rip32 ? -4 : 0),
                    });
                }
                result.ok = result.errors.empty();
                return result;
            }
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
