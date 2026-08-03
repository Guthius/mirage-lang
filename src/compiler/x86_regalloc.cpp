#include "x86_regalloc.hpp"

// See the header for the model. The implementation is four steps, each ordinary:
// block-local liveness (iterative dataflow over value bitsets), conservative
// interval construction from the live sets, kill-range construction from the
// instruction templates' fixed-register needs, and the linear scan itself with
// the machine verifier re-checking its output.

#include <algorithm>
#include <format>
#include <unordered_map>

namespace x86ra {
    namespace {
        using mir::Op;

        // Mirrors mir_passes' for_each_value_operand for a CONST instruction. The
        // same trap applies (TODO.md §7b): Asm's 'a' is a block index, Switch's
        // args are constants, Call's 'a' is a function index — none are values.
        template <typename F>
        void for_each_use(const mir::Inst &inst, F &&visit) {
            switch (inst.op) {
            case Op::ConstInt: case Op::ConstFloat: case Op::ConstNull:
            case Op::GlobalAddr: case Op::SlotAddr: case Op::FuncAddr:
            case Op::Unreachable:
                return;
            case Op::Load:
            case Op::PtrAddConst:
            case Op::StackAlloc:
                visit(inst.a);
                return;
            case Op::Store:
            case Op::PtrAdd:
                visit(inst.a);
                visit(inst.b);
                return;
            case Op::MemCopy: case Op::MemSet:
            case Op::Select:
                visit(inst.a);
                visit(inst.b);
                visit(inst.c);
                return;
            case Op::Call:
            case Op::Asm:
                for (const auto arg : inst.args) visit(arg);
                return;
            case Op::CallIndirect:
                visit(inst.a);
                for (const auto arg : inst.args) visit(arg);
                return;
            case Op::Jump:
                for (const auto arg : inst.args) visit(arg);
                return;
            case Op::Branch:
                visit(inst.a);
                return;
            case Op::Switch:
                visit(inst.a);
                return;
            case Op::Return:
                for (const auto arg : inst.args) visit(arg);
                return;
            default:
                visit(inst.a);
                if (inst.b != mir::NO_VALUE) visit(inst.b);
                return;
            }
        }

        // The ops whose emission is a live call (clobbering caller-saved state):
        // real calls, the libc-backed memory ops, and frem (libm fmod).
        auto is_call_like(const Op op) -> bool {
            return op == Op::Call || op == Op::CallIndirect ||
                   op == Op::MemCopy || op == Op::MemSet || op == Op::FRem;
        }

        struct Bitset {
            std::vector<uint64_t> words;
            explicit Bitset(const size_t bits) : words((bits + 63) / 64, 0) {}
            Bitset() = default;
            void set(const uint32_t i) { words[i / 64] |= uint64_t{1} << (i % 64); }
            void clear(const uint32_t i) { words[i / 64] &= ~(uint64_t{1} << (i % 64)); }
            [[nodiscard]] auto test(const uint32_t i) const -> bool {
                return (words[i / 64] >> (i % 64)) & 1;
            }
            // this |= other; returns whether anything changed.
            auto merge(const Bitset &other) -> bool {
                bool changed = false;
                for (size_t w = 0; w < words.size(); ++w) {
                    const auto merged = words[w] | other.words[w];
                    changed |= merged != words[w];
                    words[w] = merged;
                }
                return changed;
            }
            template <typename F>
            void for_each(F &&fn) const {
                for (size_t w = 0; w < words.size(); ++w) {
                    auto bits = words[w];
                    while (bits != 0) {
                        const auto bit = static_cast<uint32_t>(__builtin_ctzll(bits));
                        fn(static_cast<uint32_t>(w * 64 + bit));
                        bits &= bits - 1;
                    }
                }
            }
        };

        struct KillRange {
            uint32_t from = 0;
            uint32_t to = 0;
            bool is_call_clobber = false;
        };

        // GPR encoding indices, named for readability below.
        constexpr PhysReg RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSI = 6, RDI = 7;
        constexpr PhysReg R8 = 8, R9 = 9, R10 = 10, R11 = 11;
        constexpr PhysReg R12 = 12, R13 = 13, R14 = 14, R15 = 15;

        const PhysReg CALLER_SAVED_GPRS[] = {RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11};

        auto family_reg(const std::string &name) -> PhysReg {
            static const std::unordered_map<std::string, PhysReg> table = {
                {"rax", RAX}, {"rcx", RCX}, {"rdx", RDX}, {"rbx", RBX},
                {"rsp", 4},   {"rbp", 5},   {"rsi", RSI}, {"rdi", RDI},
                {"r8", R8},   {"r9", R9},   {"r10", R10}, {"r11", R11},
                {"r12", R12}, {"r13", R13}, {"r14", R14}, {"r15", R15},
            };
            const auto it = table.find(name);
            return it == table.end() ? NO_PHYS : it->second;
        }

        // Register preference orders. Non-crossing intervals want caller-saved
        // registers (leaving callee-saved ones free of prologue traffic); crossing
        // intervals want callee-saved first and fall back to caller-saved with
        // save-around. r10/r11 lead the caller-saved list because no template
        // needs them by name, so they conflict with the fewest kill ranges.
        const PhysReg GPR_PREFER_SCRATCHY[] = {R10, R11, RSI, RDI, R8, R9, RCX, RDX, RAX,
                                                RBX, R12, R13, R14, R15};
        const PhysReg GPR_PREFER_CALLEE[] = {RBX, R12, R13, R14, R15,
                                              R10, R11, RSI, RDI, R8, R9, RCX, RDX, RAX};
        // xmm0/xmm1 last: the float templates use them by name when operands are
        // spilled, so they carry the most kill ranges.
        const PhysReg XMM_ORDER[] = {FIRST_XMM + 2,  FIRST_XMM + 3,  FIRST_XMM + 4,
                                      FIRST_XMM + 5,  FIRST_XMM + 6,  FIRST_XMM + 7,
                                      FIRST_XMM + 8,  FIRST_XMM + 9,  FIRST_XMM + 10,
                                      FIRST_XMM + 11, FIRST_XMM + 12, FIRST_XMM + 13,
                                      FIRST_XMM + 14, FIRST_XMM + 15, FIRST_XMM + 1,
                                      FIRST_XMM + 0};

        struct Allocator {
            const mir::Module &module;
            const mir::Function &fn;
            Mode mode;
            Result result;

            // Global instruction indexing, layout order.
            struct BlockSpan { uint32_t first = 0, last = 0; };
            std::vector<BlockSpan> spans;
            uint32_t instruction_count = 0;

            std::vector<KillRange> kills[32];
            std::vector<uint32_t> call_indices;

            // Per-value interval accumulation.
            std::vector<uint32_t> start_of, end_of;

            void extend(const mir::ValueId value, const uint32_t pos) {
                start_of[value] = std::min(start_of[value], pos);
                end_of[value] = std::max(end_of[value], pos);
            }

            void kill(const PhysReg reg, const uint32_t from, const uint32_t to,
                       const bool call_clobber) {
                if (reg == NO_PHYS || reg == 4 || reg == 5) return;
                kills[reg].push_back({from, to, call_clobber});
            }

            void index_blocks() {
                uint32_t index = 0;
                spans.resize(fn.blocks.size());
                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    spans[b].first = index;
                    index += static_cast<uint32_t>(fn.blocks[b].insts.size());
                    spans[b].last = index - 1;
                }
                instruction_count = index;
            }

            // Kill ranges come straight from what each emission template needs by
            // name; keep this in lockstep with backend_x86.cpp's templates.
            void build_kills() {
                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    auto index = spans[b].first;
                    for (const auto &inst : fn.blocks[b].insts) {
                        const auto e = pos_e(index);
                        const auto d = pos_d(index);
                        const auto l = pos_l(index);
                        switch (inst.op) {
                        case Op::SDiv: case Op::UDiv: case Op::SRem: case Op::URem:
                            kill(RAX, e, d, false);
                            kill(RDX, e, d, false);
                            break;
                        case Op::Shl: case Op::LShr: case Op::AShr:
                            kill(RCX, e, d, false);
                            break;
                        case Op::StackAlloc:
                            // The template must not lease (a push/pop straddling its
                            // RSP adjustment would pop from the moved stack), so RAX
                            // is reserved for it statically.
                            kill(RAX, e, d, false);
                            break;
                        case Op::Switch:
                            // The compare loop branches away mid-template, so leased
                            // scratch (whose push/pop must stay balanced) is off the
                            // table; the template uses RAX/RCX by name instead.
                            kill(RAX, e, l, false);
                            kill(RCX, e, l, false);
                            break;
                        case Op::Call: case Op::CallIndirect:
                        case Op::MemCopy: case Op::MemSet:
                        case Op::FRem: {
                            for (const auto reg : CALLER_SAVED_GPRS) kill(reg, e, d, true);
                            for (PhysReg x = FIRST_XMM; x < FIRST_XMM + 16; ++x) kill(x, e, d, true);
                            call_indices.push_back(index);
                            break;
                        }
                        case Op::Asm: {
                            const auto &block = module.asm_blocks[inst.a];
                            for (const auto &fam : block.clobbered_families) {
                                kill(family_reg(fam), e, l, false);
                            }
                            if (!block.result_register.empty()) {
                                kill(family_reg(asm_result_family(block.result_register)), e, l, false);
                            }
                            // Belt and braces beyond sema's clobber analysis: every
                            // register the block NAMES is killed, read or written —
                            // killing a read-only one costs a little pressure, while
                            // trusting an incomplete clobber set would be a silent
                            // miscompile only the linear allocator could exhibit.
                            for (const auto &instruction : block.instructions) {
                                for (const auto &operand : instruction.operands) {
                                    if (operand.kind == mir::AsmOperand::Kind::Register) {
                                        kill(family_reg(asm_result_family(operand.reg)), e, l, false);
                                    }
                                }
                            }
                            break;
                        }
                        default:
                            break;
                        }
                        ++index;
                    }
                }
                for (auto &list : kills) {
                    std::sort(list.begin(), list.end(),
                              [](const KillRange &a, const KillRange &b) { return a.from < b.from; });
                }
            }

            // 'eax' -> 'rax' etc., so a result register maps onto its 64-bit root.
            static auto asm_result_family(const std::string &reg) -> std::string {
                static const std::unordered_map<std::string, std::string> table = {
                    {"rax", "rax"}, {"eax", "rax"}, {"ax", "rax"}, {"al", "rax"},
                    {"rcx", "rcx"}, {"ecx", "rcx"}, {"cx", "rcx"}, {"cl", "rcx"},
                    {"rdx", "rdx"}, {"edx", "rdx"}, {"dx", "rdx"}, {"dl", "rdx"},
                    {"rbx", "rbx"}, {"ebx", "rbx"}, {"bx", "rbx"}, {"bl", "rbx"},
                    {"rsi", "rsi"}, {"esi", "rsi"}, {"si", "rsi"}, {"sil", "rsi"},
                    {"rdi", "rdi"}, {"edi", "rdi"}, {"di", "rdi"}, {"dil", "rdi"},
                    {"r8", "r8"},   {"r8d", "r8"},  {"r9", "r9"},  {"r9d", "r9"},
                    {"r10", "r10"}, {"r10d", "r10"}, {"r11", "r11"}, {"r11d", "r11"},
                    {"r12", "r12"}, {"r12d", "r12"}, {"r13", "r13"}, {"r13d", "r13"},
                    {"r14", "r14"}, {"r14d", "r14"}, {"r15", "r15"}, {"r15d", "r15"},
                };
                const auto it = table.find(reg);
                return it == table.end() ? reg : it->second;
            }

            void build_intervals() {
                const auto value_count = fn.values.size();
                start_of.assign(value_count, UINT32_MAX);
                end_of.assign(value_count, 0);

                // Block-local gen (used before defined) and def sets.
                std::vector<Bitset> gen(fn.blocks.size()), def(fn.blocks.size());
                std::vector<Bitset> live_in(fn.blocks.size()), live_out(fn.blocks.size());
                std::vector<std::vector<uint32_t>> successors(fn.blocks.size());

                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    gen[b] = Bitset(value_count);
                    def[b] = Bitset(value_count);
                    live_in[b] = Bitset(value_count);
                    live_out[b] = Bitset(value_count);
                    const auto &block = fn.blocks[b];
                    for (const auto param : block.params) def[b].set(param);
                    for (const auto &inst : block.insts) {
                        for_each_use(inst, [&](const mir::ValueId v) {
                            if (!def[b].test(v)) gen[b].set(v);
                        });
                        if (inst.result != mir::NO_VALUE) def[b].set(inst.result);
                    }
                    if (block.insts.empty()) continue;
                    const auto &term = block.insts.back();
                    switch (term.op) {
                    case Op::Jump: successors[b].push_back(term.a); break;
                    case Op::Branch:
                        successors[b].push_back(term.b);
                        successors[b].push_back(term.c);
                        break;
                    case Op::Switch:
                        successors[b].push_back(term.b);
                        for (size_t i = 0; i + 1 < term.args.size(); i += 2) {
                            successors[b].push_back(term.args[i + 1]);
                        }
                        break;
                    default: break;
                    }
                }

                // live_out(b) = U live_in(s); live_in = gen U (live_out - def).
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (size_t b = fn.blocks.size(); b-- > 0;) {
                        for (const auto s : successors[b]) {
                            changed |= live_out[b].merge(live_in[s]);
                        }
                        Bitset in = gen[b];
                        for (size_t w = 0; w < in.words.size(); ++w) {
                            in.words[w] |= live_out[b].words[w] & ~def[b].words[w];
                        }
                        changed |= live_in[b].merge(in);
                    }
                }

                // Record positions.
                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    const auto &block = fn.blocks[b];
                    const auto block_start = pos_e(spans[b].first);
                    const auto block_end = pos_l(spans[b].last);
                    live_in[b].for_each([&](const uint32_t v) { extend(v, block_start); });
                    live_out[b].for_each([&](const uint32_t v) {
                        // Only a live-THROUGH value stretches back to the block
                        // start. Stretching a value defined mid-block would make it
                        // conservatively "cross" calls that precede its own def —
                        // and save-around semantics require the register to be
                        // unchanged between save and restore, which a def inside
                        // the window violates. This was a real miscompile: the
                        // restore after a call clobbered the call's own just-
                        // committed result.
                        if (!def[b].test(v)) extend(v, block_start);
                        extend(v, block_end);
                    });
                    for (const auto param : block.params) extend(param, block_start);

                    auto index = spans[b].first;
                    for (const auto &inst : block.insts) {
                        for_each_use(inst, [&](const mir::ValueId v) { extend(v, pos_u(index)); });
                        if (inst.result != mir::NO_VALUE) {
                            // Early-def for straight-line results; late-def for call
                            // and asm results, which their templates write after the
                            // clobber point.
                            const auto def_pos = is_call_like(inst.op) || inst.op == Op::Asm
                                ? pos_l(index) : pos_u(index);
                            extend(inst.result, def_pos);
                        }
                        if (inst.op == Op::Jump) {
                            // The jump writes the target's parameters (staging ->
                            // canonical) at its own late position; the parameter's
                            // register must be owned by it there.
                            for (const auto param : fn.blocks[inst.a].params) {
                                extend(param, pos_l(index));
                            }
                        }
                        ++index;
                    }
                }

                for (mir::ValueId v = 0; v < value_count; ++v) {
                    if (start_of[v] == UINT32_MAX) continue;
                    Interval interval{
                        .value = v,
                        .start = start_of[v],
                        .end = end_of[v],
                        .is_float = mir::is_float(fn.values[v].type),
                    };
                    for (const auto call : call_indices) {
                        if (interval.start < pos_u(call) && interval.end > pos_l(call)) {
                            interval.crosses_call = true;
                            break;
                        }
                    }
                    result.intervals.push_back(interval);
                }
                std::sort(result.intervals.begin(), result.intervals.end(),
                          [](const Interval &a, const Interval &b) {
                              return a.start != b.start ? a.start < b.start : a.value < b.value;
                          });
            }

            // Whether 'interval' is live strictly across the call at instruction
            // 'index' — before the argument reads AND after the result write. Only
            // then is its save area guaranteed fresh at that call, which is what
            // makes a caller-saved register tolerable there.
            [[nodiscard]] static auto crosses(const Interval &interval, const uint32_t index) -> bool {
                return interval.start < pos_u(index) && interval.end > pos_l(index);
            }

            [[nodiscard]] auto overlapping_kill_ok(const Interval &interval,
                                                    const PhysReg reg,
                                                    bool &needs_save) const -> bool {
                needs_save = false;
                for (const auto &k : kills[reg]) {
                    if (k.from > interval.end) break;
                    if (k.to < interval.start) continue;
                    // Overlap. A call clobber is tolerable when the interval is live
                    // across THAT call (the emitter parks it in its save area for
                    // exactly the calls it crosses); an interval merely USED at the
                    // call — an argument — would read a stale save area, so the
                    // register is ruled out for it.
                    if (k.is_call_clobber && !is_callee_saved(reg) && crosses(interval, k.from / 4)) {
                        needs_save = true;
                        continue;
                    }
                    return false;
                }
                return true;
            }

            void scan() {
                auto &assignments = result.values;
                assignments.assign(fn.values.size(), Assignment{});

                const auto new_area = [&] { return result.spill_area_count++; };

                if (mode == Mode::Trivial) {
                    for (const auto &interval : result.intervals) {
                        assignments[interval.value].spilled = true;
                        assignments[interval.value].spill_index = new_area();
                    }
                    return;
                }

                struct Active { uint32_t end; PhysReg reg; };
                std::vector<Active> active;

                for (const auto &interval : result.intervals) {
                    std::erase_if(active, [&](const Active &a) { return a.end < interval.start; });

                    const PhysReg *order = nullptr;
                    size_t order_count = 0;
                    if (interval.is_float) {
                        order = XMM_ORDER;
                        order_count = std::size(XMM_ORDER);
                    } else if (interval.crosses_call) {
                        order = GPR_PREFER_CALLEE;
                        order_count = std::size(GPR_PREFER_CALLEE);
                    } else {
                        order = GPR_PREFER_SCRATCHY;
                        order_count = std::size(GPR_PREFER_SCRATCHY);
                    }

                    auto &assignment = assignments[interval.value];
                    PhysReg chosen = NO_PHYS;
                    bool chosen_needs_save = false;
                    // First pass: a register with no conflicts at all. Second pass:
                    // accept a save-around candidate.
                    for (int pass = 0; pass < 2 && chosen == NO_PHYS; ++pass) {
                        for (size_t i = 0; i < order_count; ++i) {
                            const auto reg = order[i];
                            bool taken = false;
                            for (const auto &a : active) taken |= a.reg == reg;
                            if (taken) continue;
                            bool needs_save = false;
                            if (!overlapping_kill_ok(interval, reg, needs_save)) continue;
                            if (needs_save && pass == 0) continue;
                            chosen = reg;
                            chosen_needs_save = needs_save;
                            break;
                        }
                    }

                    if (chosen == NO_PHYS) {
                        assignment.spilled = true;
                        assignment.spill_index = new_area();
                        continue;
                    }
                    assignment.spilled = false;
                    assignment.reg = chosen;
                    if (chosen_needs_save) {
                        assignment.save_around_calls = true;
                        assignment.save_index = new_area();
                    }
                    if (!is_xmm(chosen) && is_callee_saved(chosen)) {
                        result.used_callee_saved |= static_cast<uint16_t>(1u << chosen);
                    }
                    active.push_back({interval.end, chosen});
                }
            }

            // The machine-level verifier (docs/backend.md validation #4): re-check
            // live-range interference against the finished assignment. Anything it
            // finds is an allocator bug; the caller turns entries into a hard
            // compile error rather than emitting wrong code.
            void verify() {
                std::vector<const Interval *> by_reg[32];
                for (const auto &interval : result.intervals) {
                    const auto &assignment = result.values[interval.value];
                    if (assignment.spilled) {
                        if (assignment.spill_index == UINT32_MAX) {
                            result.errors.push_back(std::format(
                                "value v{} spilled without a spill area in '{}'",
                                interval.value, fn.name));
                        }
                        continue;
                    }
                    by_reg[assignment.reg].push_back(&interval);
                }
                for (PhysReg reg = 0; reg < 32; ++reg) {
                    auto &list = by_reg[reg];
                    std::sort(list.begin(), list.end(),
                              [](const Interval *a, const Interval *b) { return a->start < b->start; });
                    for (size_t i = 0; i + 1 < list.size(); ++i) {
                        if (list[i]->end >= list[i + 1]->start) {
                            result.errors.push_back(std::format(
                                "register interference in '{}': v{} [{},{}] and v{} [{},{}] both in r{}",
                                fn.name, list[i]->value, list[i]->start, list[i]->end,
                                list[i + 1]->value, list[i + 1]->start, list[i + 1]->end, reg));
                        }
                    }
                    for (const auto *interval : list) {
                        const auto &assignment = result.values[interval->value];
                        for (const auto &k : kills[reg]) {
                            if (k.from > interval->end) break;
                            if (k.to < interval->start) continue;
                            const bool tolerated = k.is_call_clobber &&
                                assignment.save_around_calls && crosses(*interval, k.from / 4);
                            if (!tolerated) {
                                result.errors.push_back(std::format(
                                    "kill-range violation in '{}': v{} [{},{}] in r{} across [{},{}]",
                                    fn.name, interval->value, interval->start, interval->end,
                                    reg, k.from, k.to));
                            }
                        }
                    }
                }
            }

            void run() {
                index_blocks();
                build_kills();
                build_intervals();
                scan();
                if (mode == Mode::Linear) verify();
            }
        };
    }

    auto allocate(const mir::Module &module, const mir::Function &fn, const Mode mode) -> Result {
        Allocator allocator{.module = module, .fn = fn, .mode = mode};
        allocator.run();
        return std::move(allocator.result);
    }
}
