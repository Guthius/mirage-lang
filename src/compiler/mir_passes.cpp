#include "mir.hpp"

// promote_slots — stage 3's mem2reg-lite (docs/backend.md).
//
// The front end puts EVERY local in a stack slot and reaches it through load/store
// (mirgen deliberately emits memory form). This pass turns a slot back into a plain
// value wherever that is provably safe: the slot's address never escapes, and every
// access is a full-width load or store of one scalar type. Because that describes
// almost every scalar local mirgen emits, this single pass recovers most of what
// LLVM's pipeline was giving the old backend for free.
//
// SSA construction is the simple lazy algorithm (Braun et al.) adapted to this IR's
// BLOCK PARAMETERS: a merge point's value becomes a new parameter on the join block,
// and every predecessor passes its own reaching value as a jump argument. Two
// deliberate simplifications, both safe:
//
//   - No trivial-parameter elimination. A join whose incoming values all agree keeps
//     its (redundant) parameter. Correct, just occasionally one register copy larger;
//     a cleanup can fold these later without touching this algorithm.
//   - A load with no reaching store reads a zero constant. Mirage zero-values every
//     local without an initializer ('undefined' is the explicit opt-out, and reading
//     one is unspecified anyway — zero is a valid instance of unspecified).
//
// One structural wrinkle is this IR's own rule: BRANCH AND SWITCH TARGETS CANNOT TAKE
// BLOCK ARGUMENTS (mir::verify enforces it — only Jump passes them). When a join block
// gains a parameter and a predecessor reaches it through a branch or switch, the edge
// is split: a fresh block holding exactly 'jump join(args...)' takes the edge over.
// One trampoline serves every edge between the same (pred, join) pair, since all of
// them carry the same reaching values.

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace mir {
    namespace {
        // Byte width of a scalar as stored in memory. I1 is stored as one byte, which
        // is the same answer the emitters assume.
        auto store_size(const Ty type, const uint32_t pointer_bits) -> uint32_t {
            if (type == Ty::I1) return 1;
            return type_bits(type, pointer_bits) / 8;
        }

        struct PerFunction {
            Module &module;
            Function &fn;

            // slot index -> the promoted variable's scalar type; absent = not promoted.
            std::unordered_map<uint32_t, Ty> vars;
            // value id of a 'slot.addr' -> slot index (for promoted slots only).
            std::unordered_map<ValueId, uint32_t> addr_values;

            std::vector<std::vector<BlockId>> preds;

            // Lazy SSA state, keyed (slot, block).
            std::map<std::pair<uint32_t, BlockId>, ValueId> entry_def;
            std::map<std::pair<uint32_t, BlockId>, ValueId> exit_def_memo;
            // The LAST store's value per (block, slot), from a single pre-scan.
            std::map<std::pair<uint32_t, BlockId>, ValueId> last_store;
            // Zero constants materialized at the entry block's top, one per variable
            // that needed one.
            std::unordered_map<uint32_t, ValueId> zero_consts;
            std::vector<Inst> entry_prelude;

            // Parameters added to a join block, in creation order, with their slots.
            std::map<BlockId, std::vector<uint32_t>> added_params;
            // (pred, join) -> args to pass, parallel to added_params[join].
            std::map<std::pair<BlockId, BlockId>, std::vector<ValueId>> edge_args;

            // load result -> reaching value (applied transitively at rewrite time).
            std::unordered_map<ValueId, ValueId> remap;

            auto new_value(const Ty type, const BlockId block, const uint32_t index,
                            const bool is_param) -> ValueId {
                fn.values.push_back(ValueDef{.block = block, .index = index,
                                              .type = type, .is_param = is_param});
                return static_cast<ValueId>(fn.values.size() - 1);
            }

            auto resolve(ValueId value) const -> ValueId {
                while (true) {
                    const auto it = remap.find(value);
                    if (it == remap.end()) return value;
                    value = it->second;
                }
            }

            auto zero_for(const uint32_t slot, const Ty type) -> ValueId {
                if (const auto it = zero_consts.find(slot); it != zero_consts.end()) {
                    return it->second;
                }
                Inst inst;
                inst.op = type == Ty::Ptr ? Op::ConstNull
                        : is_float(type)  ? Op::ConstFloat
                                           : Op::ConstInt;
                inst.type = type;
                inst.imm = 0;
                // The prelude is spliced in front of the entry block at rewrite time;
                // the recorded index is provisional and fixed up then.
                inst.result = new_value(type, 0, static_cast<uint32_t>(entry_prelude.size()), false);
                entry_prelude.push_back(inst);
                zero_consts[slot] = inst.result;
                return inst.result;
            }

            auto get_entry(const uint32_t slot, const BlockId block) -> ValueId {
                const auto key = std::pair{slot, block};
                if (const auto it = entry_def.find(key); it != entry_def.end()) {
                    return it->second;
                }
                const auto type = vars.at(slot);
                const auto &incoming = preds[block];
                if (incoming.empty()) {
                    const auto value = zero_for(slot, type);
                    entry_def[key] = value;
                    return value;
                }
                if (incoming.size() == 1) {
                    // Memoized AFTER the recursion: a single-predecessor chain cannot
                    // reach back to this block without passing a multi-predecessor
                    // block first (which memoizes eagerly and breaks the cycle).
                    const auto value = get_exit(slot, incoming.front());
                    entry_def[key] = value;
                    return value;
                }
                // A join: the value becomes a new block parameter. Memoize BEFORE
                // visiting predecessors — a loop header's back edge reads this very
                // parameter as its own reaching value.
                const auto param = new_value(type, block,
                    static_cast<uint32_t>(fn.blocks[block].params.size()), true);
                fn.blocks[block].params.push_back(param);
                added_params[block].push_back(slot);
                entry_def[key] = param;
                for (const auto pred : incoming) {
                    edge_args[{pred, block}].push_back(get_exit(slot, pred));
                }
                return param;
            }

            auto get_exit(const uint32_t slot, const BlockId block) -> ValueId {
                const auto key = std::pair{slot, block};
                if (const auto it = exit_def_memo.find(key); it != exit_def_memo.end()) {
                    return it->second;
                }
                ValueId value;
                if (const auto it = last_store.find(key); it != last_store.end()) {
                    value = it->second;
                } else {
                    value = get_entry(slot, block);
                }
                exit_def_memo[key] = value;
                return value;
            }
        };

        // Calls 'visit' with a mutable reference to every VALUE operand of 'inst' --
        // the per-op operand map verify() also encodes, in one reusable place.
        template <typename F>
        void for_each_value_operand(Inst &inst, F &&visit) {
            switch (inst.op) {
            case Op::ConstInt: case Op::ConstFloat: case Op::ConstNull:
            case Op::GlobalAddr: case Op::SlotAddr: case Op::FuncAddr:
            case Op::Unreachable:
                return;
            case Op::Load:
            case Op::PtrAddConst:
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
                for (auto &arg : inst.args) visit(arg);
                return;
            case Op::CallIndirect:
                visit(inst.a);
                for (auto &arg : inst.args) visit(arg);
                return;
            case Op::Jump:
                for (auto &arg : inst.args) visit(arg);
                return;
            case Op::Branch:
                visit(inst.a);
                return;
            case Op::Switch:
                visit(inst.a);
                return; // args are (value, block) pairs of CONSTANTS and labels, not values
            case Op::Return:
                for (auto &arg : inst.args) visit(arg);
                return;
            default:
                // Every arithmetic/comparison/conversion op: a, and b when present.
                visit(inst.a);
                if (inst.b != NO_VALUE) visit(inst.b);
                return;
            }
        }

        void promote_function(Module &module, Function &fn, PromoteStats &stats) {
            if (!fn.has_body || fn.blocks.empty()) return;
            PerFunction p{.module = module, .fn = fn};

            // ---- candidacy -------------------------------------------------------
            // Start from every non-escaping slot, then disqualify on any access that
            // is not a full-width same-type load or store THROUGH a slot.addr value.
            std::unordered_map<uint32_t, std::optional<Ty>> candidate;
            for (uint32_t s = 0; s < fn.slots.size(); ++s) {
                if (!fn.slots[s].address_escapes) candidate[s] = std::nullopt;
            }
            std::unordered_map<ValueId, uint32_t> addr_to_slot;
            for (const auto &block : fn.blocks) {
                for (const auto &inst : block.insts) {
                    if (inst.op == Op::SlotAddr && candidate.contains(inst.a)) {
                        addr_to_slot[inst.result] = inst.a;
                    }
                }
            }
            const auto disqualify = [&](const ValueId addr) {
                if (const auto it = addr_to_slot.find(addr); it != addr_to_slot.end()) {
                    candidate.erase(it->second);
                }
            };
            for (auto &block : fn.blocks) {
                for (auto &inst : block.insts) {
                    // A load/store through a tracked address is the accepted shape;
                    // the address appearing ANYWHERE else disqualifies its slot.
                    if (inst.op == Op::Load && addr_to_slot.contains(inst.a)) {
                        const auto slot = addr_to_slot.at(inst.a);
                        if (const auto it = candidate.find(slot); it != candidate.end()) {
                            if (!it->second) it->second = inst.type;
                            if (*it->second != inst.type ||
                                store_size(inst.type, module.pointer_bits) != fn.slots[slot].size) {
                                candidate.erase(slot);
                            }
                        }
                        continue;
                    }
                    if (inst.op == Op::Store && addr_to_slot.contains(inst.a)) {
                        const auto slot = addr_to_slot.at(inst.a);
                        disqualify(inst.b); // storing an ADDRESS escapes its slot
                        if (const auto it = candidate.find(slot); it != candidate.end()) {
                            const auto stored = fn.values[inst.b].type;
                            if (!it->second) it->second = stored;
                            if (*it->second != stored ||
                                store_size(stored, module.pointer_bits) != fn.slots[slot].size) {
                                candidate.erase(slot);
                            }
                        }
                        continue;
                    }
                    for_each_value_operand(inst, [&](ValueId &operand) { disqualify(operand); });
                }
            }
            for (const auto &[slot, type] : candidate) {
                // A slot with a type was loaded or stored as that scalar; one without
                // any access at all is simply dead and can go too -- but only typed
                // ones become SSA variables.
                if (type) p.vars[slot] = *type;
            }
            std::unordered_map<uint32_t, bool> dead_slots; // untyped candidates
            for (const auto &[slot, type] : candidate) {
                if (!type) dead_slots[slot] = true;
            }
            if (p.vars.empty() && dead_slots.empty()) return;
            for (const auto &[value, slot] : addr_to_slot) {
                if (p.vars.contains(slot) || dead_slots.contains(slot)) {
                    p.addr_values[value] = slot;
                }
            }

            // ---- predecessors and last stores ------------------------------------
            p.preds.assign(fn.blocks.size(), {});
            for (BlockId b = 0; b < fn.blocks.size(); ++b) {
                const auto &insts = fn.blocks[b].insts;
                if (insts.empty()) continue;
                const auto &term = insts.back();
                const auto add_edge = [&](const BlockId target) {
                    if (target < fn.blocks.size()) p.preds[target].push_back(b);
                };
                switch (term.op) {
                case Op::Jump: add_edge(term.a); break;
                case Op::Branch: add_edge(term.b); add_edge(term.c); break;
                case Op::Switch:
                    add_edge(term.b);
                    for (size_t i = 1; i < term.args.size(); i += 2) add_edge(term.args[i]);
                    break;
                default: break;
                }
            }
            // Deduplicate multi-edges (a switch with several cases to one block): the
            // SSA algorithm wants each PREDECESSOR once, since one jump passes one
            // argument list.
            for (auto &list : p.preds) {
                std::ranges::sort(list);
                list.erase(std::ranges::unique(list).begin(), list.end());
            }
            for (BlockId b = 0; b < fn.blocks.size(); ++b) {
                for (const auto &inst : fn.blocks[b].insts) {
                    if (inst.op == Op::Store) {
                        if (const auto it = p.addr_values.find(inst.a);
                            it != p.addr_values.end() && p.vars.contains(it->second)) {
                            p.last_store[{it->second, b}] = inst.b;
                        }
                    }
                }
            }

            // ---- rewrite ---------------------------------------------------------
            // Walk each block in order, tracking the current value per variable;
            // loads remap to it (or to the lazily-computed entry value), stores update
            // it, and both die along with the slot.addr instructions.
            std::vector<std::vector<char>> dead(fn.blocks.size());
            for (BlockId b = 0; b < fn.blocks.size(); ++b) {
                dead[b].assign(fn.blocks[b].insts.size(), 0);
                std::unordered_map<uint32_t, ValueId> current;
                for (size_t i = 0; i < fn.blocks[b].insts.size(); ++i) {
                    auto &inst = fn.blocks[b].insts[i];
                    if (inst.op == Op::SlotAddr && p.addr_values.contains(inst.result)) {
                        dead[b][i] = 1;
                        continue;
                    }
                    if (inst.op == Op::Load) {
                        if (const auto it = p.addr_values.find(inst.a); it != p.addr_values.end()) {
                            const auto slot = it->second;
                            const auto reaching = current.contains(slot)
                                ? current.at(slot) : p.get_entry(slot, b);
                            p.remap[inst.result] = reaching;
                            dead[b][i] = 1;
                            continue;
                        }
                    }
                    if (inst.op == Op::Store) {
                        if (const auto it = p.addr_values.find(inst.a); it != p.addr_values.end()) {
                            if (p.vars.contains(it->second)) {
                                current[it->second] = p.resolve(inst.b);
                            }
                            dead[b][i] = 1;
                            continue;
                        }
                    }
                }
                // Fix the memoized last-store values through the remap chain, so a
                // stored load-result forwards to its true reaching value.
                for (auto &[key, value] : p.last_store) {
                    if (key.second == b) value = p.resolve(value);
                }
            }
            // Now that every load is resolved, rewrite all remaining operands.
            for (auto &block : fn.blocks) {
                for (auto &inst : block.insts) {
                    for_each_value_operand(inst, [&](ValueId &operand) {
                        if (operand != NO_VALUE) operand = p.resolve(operand);
                    });
                }
            }
            // Resolve edge args through the remap too (they were captured mid-flight).
            for (auto &[edge, args] : p.edge_args) {
                for (auto &arg : args) arg = p.resolve(arg);
            }

            // ---- edge arguments (with critical-edge splitting) -------------------
            std::map<std::pair<BlockId, BlockId>, BlockId> trampolines;
            for (const auto &[edge, args] : p.edge_args) {
                const auto [pred, succ] = edge;
                auto &term = fn.blocks[pred].insts.back();
                if (term.op == Op::Jump) {
                    for (const auto arg : args) term.args.push_back(arg);
                    continue;
                }
                // Branch/switch targets cannot carry arguments (the verifier's rule),
                // so the edge is split with a jump-only trampoline.
                auto tramp_it = trampolines.find(edge);
                if (tramp_it == trampolines.end()) {
                    Block trampoline;
                    trampoline.label = fn.blocks[succ].label + ".edge";
                    Inst jump;
                    jump.op = Op::Jump;
                    jump.a = succ;
                    jump.args.assign(args.begin(), args.end());
                    trampoline.insts.push_back(std::move(jump));
                    fn.blocks.push_back(std::move(trampoline));
                    tramp_it = trampolines.emplace(edge,
                        static_cast<BlockId>(fn.blocks.size() - 1)).first;
                }
                const auto tramp = tramp_it->second;
                auto &pred_term = fn.blocks[pred].insts.back();
                if (pred_term.op == Op::Branch) {
                    if (pred_term.b == succ) pred_term.b = tramp;
                    if (pred_term.c == succ) pred_term.c = tramp;
                } else if (pred_term.op == Op::Switch) {
                    if (pred_term.b == succ) pred_term.b = tramp;
                    for (size_t i = 1; i < pred_term.args.size(); i += 2) {
                        if (pred_term.args[i] == succ) pred_term.args[i] = tramp;
                    }
                }
            }

            // ---- dead-access removal, prelude, slot compaction -------------------
            for (BlockId b = 0; b < dead.size(); ++b) {
                auto &insts = fn.blocks[b].insts;
                std::vector<Inst> kept_insts;
                kept_insts.reserve(insts.size());
                for (size_t i = 0; i < insts.size(); ++i) {
                    if (!dead[b][i]) kept_insts.push_back(std::move(insts[i]));
                }
                insts = std::move(kept_insts);
            }
            if (!p.entry_prelude.empty()) {
                auto &entry = fn.blocks.front().insts;
                entry.insert(entry.begin(), p.entry_prelude.begin(), p.entry_prelude.end());
            }
            // ValueDef.index fields are stale after insert/erase; verify() and the
            // printer derive positions by walking blocks, but keep them coherent for
            // any future consumer that trusts them.
            for (BlockId b = 0; b < fn.blocks.size(); ++b) {
                for (uint32_t i = 0; i < fn.blocks[b].insts.size(); ++i) {
                    const auto &inst = fn.blocks[b].insts[i];
                    if (inst.result != NO_VALUE && inst.result < fn.values.size()) {
                        fn.values[inst.result].block = b;
                        fn.values[inst.result].index = i;
                    }
                }
            }

            std::unordered_map<uint32_t, uint32_t> slot_remap;
            std::vector<Slot> kept;
            for (uint32_t s = 0; s < fn.slots.size(); ++s) {
                if (p.vars.contains(s) || dead_slots.contains(s)) continue;
                slot_remap[s] = static_cast<uint32_t>(kept.size());
                kept.push_back(fn.slots[s]);
            }
            for (auto &block : fn.blocks) {
                for (auto &inst : block.insts) {
                    if (inst.op == Op::SlotAddr) inst.a = slot_remap.at(inst.a);
                }
            }
            fn.slots = std::move(kept);

            stats.slots_promoted += p.vars.size() + dead_slots.size();
            for (const auto &params : p.added_params | std::views::values) {
                stats.params_added += params.size();
            }
        }
    }

    auto promote_slots(Module &module) -> PromoteStats {
        PromoteStats stats;
        for (auto &fn : module.functions) {
            promote_function(module, fn, stats);
        }
        return stats;
    }
}
