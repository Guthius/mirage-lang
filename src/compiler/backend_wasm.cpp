#include "backend_wasm.hpp"

#include "wasm_encoder.hpp"

// MIR → standalone wasm (see the header for the shape). The translation keeps the
// x86 backend's CANONICAL FORM invariant — an integer value in a local is
// zero-extended to its type's width — so both native backends compute identical
// bit patterns and the x86↔wasm differential harness compares like with like.
// Where wasm's own semantics diverge from x86's (shift counts mod 32 vs mod 64,
// i32.div_s trapping on INT32_MIN/-1, f64.ne being unordered-ne, trapping float→
// int), the narrow operation is widened to i64 or recomposed from ordered
// primitives so the observable behavior matches the x86 backend on every input
// the corpus can express.

#include <algorithm>
#include <bit>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace backend_wasm {
    namespace {
        using wasm::ValType;
        namespace op = wasm::op;

        constexpr uint32_t DATA_BASE = 1024;        // below is the null guard region
        constexpr uint32_t STACK_SIZE = 1 << 20;    // 1 MiB shadow stack
        constexpr uint32_t PAGE = 65536;

        auto val_type(const mir::Ty type) -> ValType {
            switch (type) {
            case mir::Ty::I64: return ValType::I64;
            case mir::Ty::F32: return ValType::F32;
            case mir::Ty::F64: return ValType::F64;
            default: return ValType::I32; // I1/I8/I16/I32/Ptr on wasm32
            }
        }

        auto is_i64_class(const mir::Ty type) -> bool { return type == mir::Ty::I64; }

        // The zero-extension mask that re-establishes canonical form after an
        // operation that can overflow a narrow width. Mirrors width_of on x86:
        // I1 canonicalizes at 8 bits, matching the byte-store discipline there.
        auto narrow_mask(const mir::Ty type) -> int32_t {
            switch (type) {
            case mir::Ty::I1:
            case mir::Ty::I8: return 0xFF;
            case mir::Ty::I16: return 0xFFFF;
            default: return 0; // I32/Ptr/I64: none
            }
        }

        auto func_type_of(const mir::Module &module, const uint32_t signature) -> wasm::FuncType {
            const auto &sig = module.signatures[signature];
            wasm::FuncType type;
            for (const auto param : sig.params) type.params.push_back(val_type(param));
            // A C-variadic IMPORT takes its tail through a shadow-stack buffer
            // passed as ONE trailing pointer — emscripten's own convention, so the
            // stage-8 relocatable objects will link against real emscripten libc
            // unchanged, and the stage-7 host shim reads the same layout.
            if (sig.is_variadic) type.params.push_back(ValType::I32);
            if (sig.result != mir::Ty::Void) type.results.push_back(val_type(sig.result));
            return type;
        }

        // The wasm type of a DEFINED function comes from its entry-block
        // parameters, not its signature: a Mirage-native variadic ('fn f(a, ...)')
        // records a variadic signature whose param list understates the real
        // parameter row — mirgen collects the tail into a slice at every call
        // site, so caller and body agree with each other but not with the
        // signature. The body is the ground truth wasm must type against.
        auto defined_func_type(const mir::Module &module, const mir::Function &fn) -> wasm::FuncType {
            wasm::FuncType type;
            for (const auto param : fn.params) {
                type.params.push_back(val_type(fn.values[param].type));
            }
            const auto &sig = module.signatures[fn.signature];
            if (sig.result != mir::Ty::Void) type.results.push_back(val_type(sig.result));
            return type;
        }

        struct FunctionContext {
            const mir::Module &module;
            const mir::Function &fn;
            std::vector<std::string> &errors;
            const std::vector<uint32_t> &wasm_index;
            const std::vector<uint32_t> &type_of_sig;
            const std::vector<uint32_t> &global_address;
            const std::vector<uint32_t> &table_slot; // per mir function; 0 = not taken
            uint32_t sp_global = 0;
            int64_t fmod_index = -1;
            int64_t fmodf_index = -1;
            // Object mode (stage 8): addresses and indices become relocations
            // against these symbol indices instead of resolved constants.
            bool object_mode = false;
            const std::vector<uint32_t> *fn_symbol = nullptr;     // per mir function
            const std::vector<uint32_t> *data_symbol = nullptr;   // per mir global
            uint32_t sp_symbol = 0;
            int64_t fmod_symbol = -1;
            int64_t fmodf_symbol = -1;

            wasm::Code code;
            std::vector<ValType> locals;             // beyond the parameters
            std::vector<uint32_t> value_local;       // ValueId → local index
            std::unordered_map<mir::ValueId, uint32_t> staging_local;
            std::vector<uint32_t> slot_offset;
            uint32_t frame_size = 0;
            uint32_t saved_sp_local = UINT32_MAX;
            uint32_t frame_local = UINT32_MAX;
            uint32_t state_local = UINT32_MAX;
            uint32_t va_saved_local = UINT32_MAX;
            uint32_t va_base_local = UINT32_MAX;
            bool needs_frame = false;
            bool use_dispatch = false;
            uint32_t current_block = 0;

            void error(std::string message) { errors.push_back(std::move(message)); }

            // br depth from MIR block 'current_block' code to the dispatch loop,
            // plus 'extra' for any if-nesting at the emission point.
            [[nodiscard]] auto loop_depth(const uint32_t extra = 0) const -> uint32_t {
                return static_cast<uint32_t>(fn.blocks.size()) - 1 - current_block + extra;
            }

            void get(const mir::ValueId v) { code.local_get(value_local[v]); }
            void set(const mir::ValueId v) { code.local_set(value_local[v]); }

            // Shadow-stack pointer access: a module-local global in a standalone
            // build, the imported (relocated) 'env.__stack_pointer' in an object.
            void sp_get() {
                if (object_mode) code.global_get_reloc(0, sp_symbol);
                else code.global_get(sp_global);
            }
            void sp_set() {
                if (object_mode) code.global_set_reloc(0, sp_symbol);
                else code.global_set(sp_global);
            }

            [[nodiscard]] auto ty(const mir::ValueId v) const -> mir::Ty {
                return fn.values[v].type;
            }

            void mask_narrow(const mir::Ty type) {
                const auto mask = narrow_mask(type);
                if (mask != 0) {
                    code.i32_const(mask);
                    code.op(op::i32_and);
                }
            }

            // Push a value sign-extended to i64 from its own width.
            void push_signed64(const mir::ValueId v) {
                get(v);
                switch (ty(v)) {
                case mir::Ty::I1:
                case mir::Ty::I8:
                    code.op(op::i32_extend8_s);
                    code.op(op::i64_extend_i32_s);
                    break;
                case mir::Ty::I16:
                    code.op(op::i32_extend16_s);
                    code.op(op::i64_extend_i32_s);
                    break;
                case mir::Ty::I64:
                    break;
                default: // I32/Ptr
                    code.op(op::i64_extend_i32_s);
                    break;
                }
            }
            // Push a value zero-extended to i64 (canonical form makes this cheap).
            void push_unsigned64(const mir::ValueId v) {
                get(v);
                if (ty(v) != mir::Ty::I64) code.op(op::i64_extend_i32_u);
            }
            // Push a value as an i32 address/byte-count. mirgen mostly builds these
            // as usize (I32 on wasm32), but a pointer offset computed through a u64
            // arrives as I64 and wraps — the address space is 32 bits.
            void push_address(const mir::ValueId v) {
                get(v);
                if (ty(v) == mir::Ty::I64) code.op(op::i32_wrap_i64);
            }

            void layout_frame() {
                uint32_t offset = 0;
                slot_offset.resize(fn.slots.size());
                for (size_t i = 0; i < fn.slots.size(); ++i) {
                    const auto align = std::max<uint32_t>(1, fn.slots[i].align);
                    offset = (offset + align - 1) / align * align;
                    slot_offset[i] = offset;
                    offset += std::max<uint32_t>(1, fn.slots[i].size);
                }
                frame_size = (offset + 15) / 16 * 16;
            }

            void assign_locals() {
                const auto param_count = fn.params.size();
                value_local.assign(fn.values.size(), UINT32_MAX);
                for (size_t i = 0; i < param_count; ++i) {
                    value_local[fn.params[i]] = static_cast<uint32_t>(i);
                }
                auto next = static_cast<uint32_t>(param_count);
                const auto add_local = [&](const ValType type) {
                    locals.push_back(type);
                    return next++;
                };

                bool has_stackalloc = false;
                bool has_variadic_call = false;
                for (const auto &block : fn.blocks) {
                    for (const auto &inst : block.insts) {
                        if (inst.op == mir::Op::StackAlloc) has_stackalloc = true;
                        if (inst.op == mir::Op::Call || inst.op == mir::Op::CallIndirect) {
                            const bool indirect = inst.op == mir::Op::CallIndirect;
                            const auto signature = indirect
                                ? inst.b : module.functions[inst.a].signature;
                            if (module.signatures[signature].is_variadic &&
                                (indirect || !module.functions[inst.a].has_body)) {
                                has_variadic_call = true;
                            }
                        }
                    }
                }
                needs_frame = frame_size > 0 || has_stackalloc;
                if (needs_frame) {
                    saved_sp_local = add_local(ValType::I32);
                    frame_local = add_local(ValType::I32);
                }
                if (has_variadic_call) {
                    va_saved_local = add_local(ValType::I32);
                    va_base_local = add_local(ValType::I32);
                }

                use_dispatch = fn.blocks.size() > 1;
                if (!use_dispatch && !fn.blocks.empty() && !fn.blocks[0].insts.empty()) {
                    const auto terminator = fn.blocks[0].insts.back().op;
                    use_dispatch = terminator == mir::Op::Jump ||
                                   terminator == mir::Op::Branch ||
                                   terminator == mir::Op::Switch;
                }
                if (use_dispatch) state_local = add_local(ValType::I32);

                for (mir::ValueId v = 0; v < fn.values.size(); ++v) {
                    if (value_local[v] != UINT32_MAX) continue; // a parameter
                    value_local[v] = add_local(val_type(fn.values[v].type));
                }
                for (size_t b = 0; b < fn.blocks.size(); ++b) {
                    if (b == 0) continue; // entry params ARE the wasm params
                    for (const auto param : fn.blocks[b].params) {
                        staging_local[param] = add_local(val_type(fn.values[param].type));
                    }
                }
            }

            // ---- shared op fragments -----------------------------------------
            void emit_icmp(const mir::Inst &inst) {
                const bool wide = is_i64_class(ty(inst.a));
                get(inst.a);
                get(inst.b);
                uint8_t opcode = 0;
                switch (inst.op) {
                case mir::Op::ICmpEq: opcode = wide ? op::i64_eq : op::i32_eq; break;
                case mir::Op::ICmpNe: opcode = wide ? op::i64_ne : op::i32_ne; break;
                case mir::Op::ICmpSlt: opcode = wide ? op::i64_lt_s : op::i32_lt_s; break;
                case mir::Op::ICmpSle: opcode = wide ? op::i64_le_s : op::i32_le_s; break;
                case mir::Op::ICmpSgt: opcode = wide ? op::i64_gt_s : op::i32_gt_s; break;
                case mir::Op::ICmpSge: opcode = wide ? op::i64_ge_s : op::i32_ge_s; break;
                case mir::Op::ICmpUlt: opcode = wide ? op::i64_lt_u : op::i32_lt_u; break;
                case mir::Op::ICmpUle: opcode = wide ? op::i64_le_u : op::i32_le_u; break;
                case mir::Op::ICmpUgt: opcode = wide ? op::i64_gt_u : op::i32_gt_u; break;
                default: opcode = wide ? op::i64_ge_u : op::i32_ge_u; break;
                }
                code.op(opcode);
                set(inst.result);
            }

            void emit_fcmp(const mir::Inst &inst) {
                const bool dbl = ty(inst.a) == mir::Ty::F64;
                const auto pick = [&](const uint8_t f32_op, const uint8_t f64_op) {
                    return dbl ? f64_op : f32_op;
                };
                if (inst.op == mir::Op::FCmpOne) {
                    // wasm's f.ne is UNORDERED-ne (true on NaN); ordered-ne is
                    // (a < b) | (a > b), false on NaN like the x86 backend.
                    get(inst.a);
                    get(inst.b);
                    code.op(pick(op::f32_lt, op::f64_lt));
                    get(inst.a);
                    get(inst.b);
                    code.op(pick(op::f32_gt, op::f64_gt));
                    code.op(op::i32_or);
                    set(inst.result);
                    return;
                }
                get(inst.a);
                get(inst.b);
                switch (inst.op) {
                case mir::Op::FCmpOeq: code.op(pick(op::f32_eq, op::f64_eq)); break;
                case mir::Op::FCmpOlt: code.op(pick(op::f32_lt, op::f64_lt)); break;
                case mir::Op::FCmpOle: code.op(pick(op::f32_le, op::f64_le)); break;
                case mir::Op::FCmpOgt: code.op(pick(op::f32_gt, op::f64_gt)); break;
                default: code.op(pick(op::f32_ge, op::f64_ge)); break;
                }
                set(inst.result);
            }

            void emit_call(const mir::Inst &inst, const bool indirect) {
                const auto signature = indirect ? inst.b : module.functions[inst.a].signature;
                const auto &sig = module.signatures[signature];
                // The C buffer convention applies to variadic IMPORTS (and, for
                // completeness, indirect variadic calls). A defined Mirage
                // variadic already received its tail as an ordinary slice —
                // mirgen collected it at the call site — so its args pass
                // verbatim against the body-derived type.
                const bool c_variadic = sig.is_variadic &&
                    (indirect || !module.functions[inst.a].has_body);
                const auto fixed = c_variadic ? sig.params.size() : inst.args.size();

                if (c_variadic) {
                    // Spill the variadic tail to a shadow-stack buffer, ILP32
                    // C-promotion layout: 4-byte cells for i32-class arguments,
                    // 8-byte-aligned 8-byte cells for i64/f64, f32 promoted to
                    // f64. The buffer pointer becomes the one extra argument.
                    uint32_t size = 0;
                    std::vector<uint32_t> offsets;
                    for (size_t i = fixed; i < inst.args.size(); ++i) {
                        const auto type = ty(inst.args[i]);
                        const bool wide = type == mir::Ty::I64 || type == mir::Ty::F64 ||
                                          type == mir::Ty::F32;
                        if (wide) size = (size + 7) & ~7u;
                        offsets.push_back(size);
                        size += wide ? 8 : 4;
                    }
                    size = (size + 15) & ~15u;

                    sp_get();
                    code.local_set(va_saved_local);
                    sp_get();
                    code.i32_const(static_cast<int32_t>(size));
                    code.op(op::i32_sub);
                    code.local_tee(va_base_local);
                    sp_set();
                    for (size_t i = fixed; i < inst.args.size(); ++i) {
                        const auto arg = inst.args[i];
                        const auto type = ty(arg);
                        const auto offset = offsets[i - fixed];
                        code.local_get(va_base_local);
                        get(arg);
                        if (type == mir::Ty::F32) {
                            code.op(op::f64_promote_f32);
                            code.store(ValType::F64, 64, 0, offset);
                        } else if (type == mir::Ty::F64) {
                            code.store(ValType::F64, 64, 0, offset);
                        } else if (type == mir::Ty::I64) {
                            code.store(ValType::I64, 64, 0, offset);
                        } else {
                            code.store(ValType::I32, 32, 0, offset);
                        }
                    }
                }

                const auto declared_param = [&](const size_t i) -> mir::Ty {
                    if (!indirect && module.functions[inst.a].has_body) {
                        const auto &callee = module.functions[inst.a];
                        if (i < callee.params.size()) {
                            return module.functions[inst.a].values.empty()
                                ? ty(inst.args[i])
                                : callee.values[callee.params[i]].type;
                        }
                        return ty(inst.args[i]);
                    }
                    return i < sig.params.size() ? sig.params[i] : ty(inst.args[i]);
                };
                for (size_t i = 0; i < fixed && i < inst.args.size(); ++i) {
                    get(inst.args[i]);
                    // Width-coerce when the value's class disagrees with the
                    // declared parameter (e.g. a usize-shaped I32 into a
                    // declared-I64 slot). The x86 backend's 64-bit registers
                    // absorb this silently; wasm's type system does not.
                    const bool arg64 = is_i64_class(ty(inst.args[i]));
                    const bool want64 = is_i64_class(declared_param(i));
                    if (arg64 && !want64) code.op(op::i32_wrap_i64);
                    else if (!arg64 && want64) code.op(op::i64_extend_i32_u);
                }
                if (c_variadic) code.local_get(va_base_local);
                if (indirect) {
                    get(inst.a); // the table index
                    if (object_mode) code.call_indirect_reloc(type_of_sig[signature]);
                    else code.call_indirect(type_of_sig[signature]);
                } else if (object_mode) {
                    code.call_reloc(wasm_index[inst.a], (*fn_symbol)[inst.a]);
                } else {
                    code.call(wasm_index[inst.a]);
                }
                if (inst.result != mir::NO_VALUE) {
                    const bool got64 = is_i64_class(sig.result);
                    const bool want64 = is_i64_class(inst.type);
                    if (got64 && !want64) code.op(op::i32_wrap_i64);
                    else if (!got64 && want64) code.op(op::i64_extend_i32_u);
                    // Re-canonicalize a narrow integer result, as the x86 backend
                    // does after every call.
                    mask_narrow(inst.type);
                    set(inst.result);
                } else if (sig.result != mir::Ty::Void) {
                    code.drop();
                }
                if (c_variadic) {
                    // Rebalance immediately (not at function exit): a variadic
                    // call in a loop must not creep the shadow stack downward.
                    code.local_get(va_saved_local);
                    sp_set();
                }
            }

            void emit_inst(const mir::Inst &inst) {
                using Op = mir::Op;
                switch (inst.op) {
                case Op::ConstInt: {
                    if (is_i64_class(inst.type)) {
                        code.i64_const(inst.imm);
                    } else {
                        const auto bits = mir::type_bits(inst.type, module.pointer_bits);
                        auto value = static_cast<uint64_t>(inst.imm);
                        if (bits != 0 && bits < 64) value &= (uint64_t{1} << bits) - 1;
                        code.i32_const(static_cast<int32_t>(value));
                    }
                    set(inst.result);
                    return;
                }
                case Op::ConstFloat:
                    if (inst.type == mir::Ty::F64) {
                        code.f64_const_bits(static_cast<uint64_t>(inst.imm));
                    } else {
                        code.f32_const_bits(static_cast<uint32_t>(inst.imm));
                    }
                    set(inst.result);
                    return;
                case Op::ConstNull:
                    code.i32_const(0);
                    set(inst.result);
                    return;
                case Op::GlobalAddr:
                    if (object_mode) {
                        code.i32_const_memory((*data_symbol)[inst.a], 0);
                        set(inst.result);
                        return;
                    }
                    code.i32_const(static_cast<int32_t>(global_address[inst.a]));
                    set(inst.result);
                    return;
                case Op::FuncAddr:
                    if (object_mode) {
                        code.i32_const_table((*fn_symbol)[inst.a]);
                        set(inst.result);
                        return;
                    }
                    if (table_slot[inst.a] == 0) {
                        error(std::format("wasm backend: function '{}' address taken but "
                                           "not in the table", module.functions[inst.a].name));
                        return;
                    }
                    code.i32_const(static_cast<int32_t>(table_slot[inst.a]));
                    set(inst.result);
                    return;
                case Op::SlotAddr:
                    code.local_get(frame_local);
                    code.i32_const(static_cast<int32_t>(slot_offset[inst.a]));
                    code.op(op::i32_add);
                    set(inst.result);
                    return;
                case Op::StackAlloc:
                    sp_get();
                    push_address(inst.a);
                    code.op(op::i32_sub);
                    code.i32_const(-16);
                    code.op(op::i32_and);
                    code.local_tee(value_local[inst.result]);
                    sp_set();
                    return;

                case Op::Load: {
                    get(inst.a);
                    const auto type = inst.type;
                    if (type == mir::Ty::F32 || type == mir::Ty::F64) {
                        code.load(val_type(type), 0, false, 0, 0);
                    } else if (type == mir::Ty::I64) {
                        code.load(ValType::I64, 64, false, 0, 0);
                    } else {
                        const auto bits = type == mir::Ty::I16 ? 16u
                            : (type == mir::Ty::I1 || type == mir::Ty::I8) ? 8u : 32u;
                        code.load(ValType::I32, bits, false, 0, 0);
                    }
                    set(inst.result);
                    return;
                }
                case Op::Store: {
                    get(inst.a);
                    get(inst.b);
                    const auto type = ty(inst.b);
                    if (type == mir::Ty::F32 || type == mir::Ty::F64) {
                        code.store(val_type(type), 0, 0, 0);
                    } else if (type == mir::Ty::I64) {
                        code.store(ValType::I64, 64, 0, 0);
                    } else {
                        const auto bits = type == mir::Ty::I16 ? 16u
                            : (type == mir::Ty::I1 || type == mir::Ty::I8) ? 8u : 32u;
                        code.store(ValType::I32, bits, 0, 0);
                    }
                    return;
                }
                case Op::MemCopy:
                    get(inst.a);
                    get(inst.b);
                    push_address(inst.c);
                    code.memory_copy();
                    return;
                case Op::MemSet:
                    get(inst.a);
                    get(inst.b);
                    push_address(inst.c);
                    code.memory_fill();
                    return;

                case Op::PtrAddConst:
                    get(inst.a);
                    code.i32_const(static_cast<int32_t>(inst.imm));
                    code.op(op::i32_add);
                    set(inst.result);
                    return;
                case Op::PtrAdd:
                    get(inst.a);
                    push_address(inst.b);
                    code.op(op::i32_add);
                    set(inst.result);
                    return;

                case Op::Add: case Op::Sub: case Op::Mul:
                case Op::And: case Op::Or: case Op::Xor: {
                    const bool wide = is_i64_class(inst.type);
                    get(inst.a);
                    get(inst.b);
                    uint8_t opcode = 0;
                    switch (inst.op) {
                    case Op::Add: opcode = wide ? op::i64_add : op::i32_add; break;
                    case Op::Sub: opcode = wide ? op::i64_sub : op::i32_sub; break;
                    case Op::Mul: opcode = wide ? op::i64_mul : op::i32_mul; break;
                    case Op::And: opcode = wide ? op::i64_and : op::i32_and; break;
                    case Op::Or: opcode = wide ? op::i64_or : op::i32_or; break;
                    default: opcode = wide ? op::i64_xor : op::i32_xor; break;
                    }
                    code.op(opcode);
                    if (inst.op == Op::Add || inst.op == Op::Sub || inst.op == Op::Mul) {
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                }
                case Op::SDiv: case Op::SRem: {
                    // Widened to i64 so INT32_MIN/-1 wraps as it does through the
                    // x86 backend's 64-bit idiv instead of trapping.
                    if (is_i64_class(inst.type)) {
                        get(inst.a);
                        get(inst.b);
                        code.op(inst.op == Op::SDiv ? op::i64_div_s : op::i64_rem_s);
                    } else {
                        push_signed64(inst.a);
                        push_signed64(inst.b);
                        code.op(inst.op == Op::SDiv ? op::i64_div_s : op::i64_rem_s);
                        code.op(op::i32_wrap_i64);
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                }
                case Op::UDiv: case Op::URem: {
                    const bool wide = is_i64_class(inst.type);
                    get(inst.a);
                    get(inst.b);
                    if (inst.op == Op::UDiv) code.op(wide ? op::i64_div_u : op::i32_div_u);
                    else code.op(wide ? op::i64_rem_u : op::i32_rem_u);
                    set(inst.result);
                    return;
                }
                case Op::Shl: case Op::LShr: case Op::AShr: {
                    if (is_i64_class(inst.type)) {
                        get(inst.a);
                        get(inst.b);
                        code.op(inst.op == Op::Shl ? op::i64_shl
                                : inst.op == Op::LShr ? op::i64_shr_u : op::i64_shr_s);
                        set(inst.result);
                        return;
                    }
                    // Narrow shifts run at 64 bits: wasm's i32 shifts take the
                    // count mod 32 where the x86 backend's W64 shifts take it mod
                    // 64, and the results differ for counts in [width, 64).
                    if (inst.op == Op::AShr) push_signed64(inst.a);
                    else push_unsigned64(inst.a);
                    push_unsigned64(inst.b);
                    code.op(inst.op == Op::Shl ? op::i64_shl
                            : inst.op == Op::LShr ? op::i64_shr_u : op::i64_shr_s);
                    code.op(op::i32_wrap_i64);
                    if (inst.op != Op::LShr) mask_narrow(inst.type);
                    return set(inst.result);
                }
                case Op::Not:
                    get(inst.a);
                    if (inst.type == mir::Ty::I1) {
                        code.i32_const(1);
                        code.op(op::i32_xor);
                    } else if (is_i64_class(inst.type)) {
                        code.i64_const(-1);
                        code.op(op::i64_xor);
                    } else {
                        code.i32_const(-1);
                        code.op(op::i32_xor);
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                case Op::Neg:
                    if (is_i64_class(inst.type)) {
                        code.i64_const(0);
                        get(inst.a);
                        code.op(op::i64_sub);
                    } else {
                        code.i32_const(0);
                        get(inst.a);
                        code.op(op::i32_sub);
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;

                case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: {
                    const bool dbl = inst.type == mir::Ty::F64;
                    get(inst.a);
                    get(inst.b);
                    switch (inst.op) {
                    case Op::FAdd: code.op(dbl ? op::f64_add : op::f32_add); break;
                    case Op::FSub: code.op(dbl ? op::f64_sub : op::f32_sub); break;
                    case Op::FMul: code.op(dbl ? op::f64_mul : op::f32_mul); break;
                    default: code.op(dbl ? op::f64_div : op::f32_div); break;
                    }
                    set(inst.result);
                    return;
                }
                case Op::FRem: {
                    const bool dbl = inst.type == mir::Ty::F64;
                    const auto callee = dbl ? fmod_index : fmodf_index;
                    if (callee < 0) {
                        error("wasm backend: internal error: fmod import missing");
                        return;
                    }
                    get(inst.a);
                    get(inst.b);
                    if (object_mode) {
                        code.call_reloc(static_cast<uint32_t>(callee),
                                         static_cast<uint32_t>(dbl ? fmod_symbol : fmodf_symbol));
                    } else {
                        code.call(static_cast<uint32_t>(callee));
                    }
                    set(inst.result);
                    return;
                }
                case Op::FNeg:
                    get(inst.a);
                    code.op(inst.type == mir::Ty::F64 ? op::f64_neg : op::f32_neg);
                    set(inst.result);
                    return;

                case Op::ICmpEq: case Op::ICmpNe:
                case Op::ICmpSlt: case Op::ICmpSle: case Op::ICmpSgt: case Op::ICmpSge:
                case Op::ICmpUlt: case Op::ICmpUle: case Op::ICmpUgt: case Op::ICmpUge:
                    emit_icmp(inst);
                    return;
                case Op::FCmpOeq: case Op::FCmpOne:
                case Op::FCmpOlt: case Op::FCmpOle: case Op::FCmpOgt: case Op::FCmpOge:
                    emit_fcmp(inst);
                    return;

                case Op::Trunc:
                    get(inst.a);
                    if (is_i64_class(ty(inst.a))) code.op(op::i32_wrap_i64);
                    if (inst.type == mir::Ty::I32 || inst.type == mir::Ty::Ptr) {
                        // full 32 bits survive the wrap
                    } else {
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                case Op::ZExt: case Op::PtrToInt: case Op::IntToPtr: {
                    get(inst.a);
                    const bool from64 = is_i64_class(ty(inst.a));
                    const bool to64 = is_i64_class(inst.type);
                    if (!from64 && to64) code.op(op::i64_extend_i32_u);
                    else if (from64 && !to64) code.op(op::i32_wrap_i64);
                    mask_narrow(inst.type);
                    set(inst.result);
                    return;
                }
                case Op::SExt: {
                    get(inst.a);
                    switch (ty(inst.a)) {
                    case mir::Ty::I1:
                    case mir::Ty::I8: code.op(op::i32_extend8_s); break;
                    case mir::Ty::I16: code.op(op::i32_extend16_s); break;
                    default: break;
                    }
                    if (is_i64_class(inst.type)) {
                        code.op(op::i64_extend_i32_s);
                    } else {
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                }
                case Op::FPTrunc: case Op::FPExt:
                    get(inst.a);
                    code.op(inst.type == mir::Ty::F64 ? op::f64_promote_f32 : op::f32_demote_f64);
                    set(inst.result);
                    return;
                case Op::SIToFP:
                    push_signed64(inst.a);
                    code.op(inst.type == mir::Ty::F64 ? op::f64_convert_i64_s
                                                       : op::f32_convert_i64_s);
                    set(inst.result);
                    return;
                case Op::UIToFP:
                    push_unsigned64(inst.a);
                    code.op(inst.type == mir::Ty::F64 ? op::f64_convert_i64_u
                                                       : op::f32_convert_i64_u);
                    set(inst.result);
                    return;
                case Op::FPToSI: case Op::FPToUI: {
                    // Saturating truncation: never traps, like x86's cvttsd2si;
                    // the two differ only on inputs whose conversion is undefined.
                    get(inst.a);
                    const bool from_dbl = ty(inst.a) == mir::Ty::F64;
                    const bool to_signed = inst.op == Op::FPToSI;
                    code.op_fc(from_dbl
                        ? (to_signed ? op::i64_trunc_sat_f64_s : op::i64_trunc_sat_f64_u)
                        : (to_signed ? op::i64_trunc_sat_f32_s : op::i64_trunc_sat_f32_u));
                    if (!is_i64_class(inst.type)) {
                        code.op(op::i32_wrap_i64);
                        mask_narrow(inst.type);
                    }
                    set(inst.result);
                    return;
                }
                case Op::Bitcast: {
                    // Same-width reinterpretation; a same-class bitcast (int↔int,
                    // ptr↔int) is just the copy.
                    const auto from = ty(inst.a);
                    const auto to = inst.type;
                    get(inst.a);
                    if (from == mir::Ty::F32 && val_type(to) == ValType::I32) {
                        code.op(op::i32_reinterpret_f32);
                    } else if (from == mir::Ty::F64 && to == mir::Ty::I64) {
                        code.op(op::i64_reinterpret_f64);
                    } else if (val_type(from) == ValType::I32 && to == mir::Ty::F32) {
                        code.op(op::f32_reinterpret_i32);
                    } else if (from == mir::Ty::I64 && to == mir::Ty::F64) {
                        code.op(op::f64_reinterpret_i64);
                    }
                    set(inst.result);
                    return;
                }

                case Op::Select:
                    get(inst.b);
                    get(inst.c);
                    get(inst.a);
                    code.select_op();
                    set(inst.result);
                    return;

                case Op::Call: emit_call(inst, false); return;
                case Op::CallIndirect: emit_call(inst, true); return;

                case Op::Asm:
                    error("wasm backend: inline 'asm' is x86-only and cannot be compiled "
                          "for a wasm target");
                    return;

                case Op::Jump: {
                    const auto &params = fn.blocks[inst.a].params;
                    for (size_t i = 0; i < inst.args.size() && i < params.size(); ++i) {
                        get(inst.args[i]);
                        code.local_set(staging_local.at(params[i]));
                    }
                    for (size_t i = 0; i < inst.args.size() && i < params.size(); ++i) {
                        code.local_get(staging_local.at(params[i]));
                        code.local_set(value_local[params[i]]);
                    }
                    code.i32_const(static_cast<int32_t>(inst.a));
                    code.local_set(state_local);
                    code.br(loop_depth());
                    return;
                }
                case Op::Branch:
                    code.i32_const(static_cast<int32_t>(inst.b));
                    code.i32_const(static_cast<int32_t>(inst.c));
                    get(inst.a);
                    code.select_op();
                    code.local_set(state_local);
                    code.br(loop_depth());
                    return;
                case Op::Switch: {
                    const bool wide = is_i64_class(ty(inst.a));
                    for (size_t i = 0; i + 1 < inst.args.size(); i += 2) {
                        get(inst.a);
                        // Case values are stored 32-bit-truncated; compare raw at
                        // 32 bits, sign-extended at 64 — the x86 backend's rule.
                        if (wide) {
                            code.i64_const(static_cast<int64_t>(static_cast<int32_t>(inst.args[i])));
                            code.op(op::i64_eq);
                        } else {
                            code.i32_const(static_cast<int32_t>(inst.args[i]));
                            code.op(op::i32_eq);
                        }
                        code.if_void();
                        code.i32_const(static_cast<int32_t>(inst.args[i + 1]));
                        code.local_set(state_local);
                        code.br(loop_depth(1));
                        code.end();
                    }
                    code.i32_const(static_cast<int32_t>(inst.b));
                    code.local_set(state_local);
                    code.br(loop_depth());
                    return;
                }
                case Op::Return:
                    if (needs_frame) {
                        code.local_get(saved_sp_local);
                        sp_set();
                    }
                    if (!inst.args.empty()) get(inst.args.front());
                    code.return_op();
                    return;
                case Op::Unreachable:
                    code.unreachable_op();
                    return;
                }
                error(std::format("wasm backend: unhandled MIR op '{}'", op_name(inst.op)));
            }

            void emit() {
                layout_frame();
                assign_locals();

                if (needs_frame) {
                    sp_get();
                    code.local_tee(saved_sp_local);
                    code.i32_const(static_cast<int32_t>(frame_size));
                    code.op(op::i32_sub);
                    code.local_tee(frame_local);
                    sp_set();
                }

                if (!use_dispatch) {
                    // Straight-line body: no state machine needed.
                    if (!fn.blocks.empty()) {
                        current_block = 0;
                        for (const auto &inst : fn.blocks[0].insts) emit_inst(inst);
                    }
                    code.unreachable_op(); // the terminator returned or trapped
                    return;
                }

                // The dispatch loop: 'loop { block×N { br_table $state } ... }'.
                // MIR block i's code lands after the i-th 'end'; a jump sets
                // $state and branches back to the loop head.
                const auto n = static_cast<uint32_t>(fn.blocks.size());
                code.loop_void();
                for (uint32_t i = 0; i < n; ++i) code.block_void();
                code.local_get(state_local);
                std::vector<uint32_t> depths(n);
                for (uint32_t i = 0; i < n; ++i) depths[i] = i;
                code.br_table(depths, 0);
                for (uint32_t b = 0; b < n; ++b) {
                    code.end(); // close block b's landing pad
                    current_block = b;
                    for (const auto &inst : fn.blocks[b].insts) emit_inst(inst);
                }
                code.end();            // the loop
                code.unreachable_op(); // every path returned or branched
            }
        };
    }

    auto generate(const mir::Module &module, const uint32_t test_info,
                   const uint32_t test_runner) -> Result {
        Result result;
        if (module.pointer_bits != 32) {
            result.errors.push_back("wasm backend: module was lowered with 64-bit pointers; "
                                     "compile with a wasm32 target");
            return result;
        }

        wasm::Module out;

        // ---- reference scan --------------------------------------------------
        // Imports are declared only for functions actually reached (called,
        // address-taken, or targeted by a global-initializer relocation), so an
        // unused 'ext fn' declaration does not obligate the host to provide it.
        std::unordered_set<uint32_t> referenced;
        bool needs_fmod = false;
        bool needs_fmodf = false;
        std::vector<bool> address_taken(module.functions.size(), false);
        for (const auto &fn : module.functions) {
            if (!fn.has_body) continue;
            for (const auto &block : fn.blocks) {
                for (const auto &inst : block.insts) {
                    if (inst.op == mir::Op::Call) referenced.insert(inst.a);
                    if (inst.op == mir::Op::FuncAddr) {
                        referenced.insert(inst.a);
                        address_taken[inst.a] = true;
                    }
                    if (inst.op == mir::Op::FRem) {
                        (inst.type == mir::Ty::F64 ? needs_fmod : needs_fmodf) = true;
                    }
                }
            }
        }
        for (const auto &global : module.globals) {
            for (const auto &reloc : global.relocations) {
                if (reloc.kind == mir::Relocation::Kind::FunctionAddr) {
                    referenced.insert(reloc.target);
                    address_taken[reloc.target] = true;
                }
            }
        }

        // ---- types -----------------------------------------------------------
        std::vector<uint32_t> type_of_sig(module.signatures.size());
        for (size_t i = 0; i < module.signatures.size(); ++i) {
            type_of_sig[i] = out.intern_type(func_type_of(module, static_cast<uint32_t>(i)));
        }

        // ---- function index space: imports first -----------------------------
        std::vector<uint32_t> wasm_index(module.functions.size(), UINT32_MAX);
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (fn.has_body || !referenced.contains(static_cast<uint32_t>(i))) continue;
            wasm_index[i] = static_cast<uint32_t>(out.imports.size());
            out.imports.push_back({
                .module = fn.import_module.empty() ? "env" : fn.import_module,
                .name = fn.import_name.empty() ? fn.name : fn.import_name,
                .type_index = type_of_sig[fn.signature],
            });
        }
        int64_t fmod_index = -1;
        int64_t fmodf_index = -1;
        if (needs_fmod) {
            fmod_index = static_cast<int64_t>(out.imports.size());
            out.imports.push_back({"env", "fmod",
                out.intern_type({{ValType::F64, ValType::F64}, {ValType::F64}})});
        }
        if (needs_fmodf) {
            fmodf_index = static_cast<int64_t>(out.imports.size());
            out.imports.push_back({"env", "fmodf",
                out.intern_type({{ValType::F32, ValType::F32}, {ValType::F32}})});
        }
        const auto import_count = static_cast<uint32_t>(out.imports.size());
        uint32_t defined = 0;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (module.functions[i].has_body) wasm_index[i] = import_count + defined++;
        }

        // ---- table: one slot per address-taken function, from slot 1 ---------
        std::vector<uint32_t> table_slot(module.functions.size(), 0);
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (!address_taken[i]) continue;
            if (wasm_index[i] == UINT32_MAX) continue; // unreferenced import
            table_slot[i] = static_cast<uint32_t>(out.table_elements.size() + 1);
            out.table_elements.push_back(wasm_index[i]);
        }
        out.table_size = static_cast<uint32_t>(out.table_elements.size() + 1);

        // ---- memory layout ---------------------------------------------------
        // [0, 1024) null guard, then the data image, then zero-initialized
        // globals, then the shadow stack; __heap_base is the stack top.
        std::vector<uint32_t> global_address(module.globals.size(), 0);
        uint32_t cursor = DATA_BASE;
        for (size_t i = 0; i < module.globals.size(); ++i) {
            const auto &global = module.globals[i];
            if (global.init.empty()) continue; // placed after the image
            const auto align = std::max<uint32_t>(1, global.align);
            cursor = (cursor + align - 1) / align * align;
            global_address[i] = cursor;
            cursor += std::max<uint32_t>(1, global.size);
        }
        const auto data_end = cursor;
        for (size_t i = 0; i < module.globals.size(); ++i) {
            const auto &global = module.globals[i];
            if (!global.init.empty()) continue;
            const auto align = std::max<uint32_t>(1, global.align);
            cursor = (cursor + align - 1) / align * align;
            global_address[i] = cursor;
            cursor += std::max<uint32_t>(1, global.size);
        }
        const auto stack_base = (cursor + 15) / 16 * 16;
        const auto stack_top = stack_base + STACK_SIZE;
        const auto heap_base = stack_top;
        out.memory_min_pages = (heap_base + PAGE - 1) / PAGE + 16; // 1 MiB of heap slack

        // One data segment covering the initialized image, relocations resolved
        // in place: a function target becomes its table slot, a global target its
        // absolute address — final layout, nothing left for a linker.
        if (data_end > DATA_BASE) {
            std::vector<uint8_t> image(data_end - DATA_BASE, 0);
            for (size_t i = 0; i < module.globals.size(); ++i) {
                const auto &global = module.globals[i];
                if (global.init.empty()) continue;
                std::copy(global.init.begin(), global.init.end(),
                          image.begin() + (global_address[i] - DATA_BASE));
            }
            for (size_t i = 0; i < module.globals.size(); ++i) {
                const auto &global = module.globals[i];
                if (global.init.empty()) continue;
                for (const auto &reloc : global.relocations) {
                    uint32_t value = 0;
                    if (reloc.kind == mir::Relocation::Kind::FunctionAddr) {
                        value = table_slot[reloc.target];
                        if (value == 0) {
                            result.errors.push_back(std::format(
                                "wasm backend: relocation against '{}' which has no table slot",
                                module.functions[reloc.target].name));
                        }
                    } else {
                        value = global_address[reloc.target];
                    }
                    value += static_cast<uint32_t>(reloc.addend);
                    const auto at = global_address[i] - DATA_BASE + reloc.offset;
                    for (int b = 0; b < 4; ++b) {
                        image[at + b] = static_cast<uint8_t>(value >> (8 * b));
                    }
                }
            }
            out.data.push_back({DATA_BASE, std::move(image)});
        }

        // ---- globals: the shadow stack pointer -------------------------------
        const auto sp_global = static_cast<uint32_t>(out.globals.size());
        out.globals.push_back({ValType::I32, true, static_cast<int64_t>(stack_top)});
        const auto heap_base_global = static_cast<uint32_t>(out.globals.size());
        out.globals.push_back({ValType::I32, false, static_cast<int64_t>(heap_base)});

        // ---- bodies ----------------------------------------------------------
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body) continue;
            FunctionContext ctx{
                .module = module,
                .fn = fn,
                .errors = result.errors,
                .wasm_index = wasm_index,
                .type_of_sig = type_of_sig,
                .global_address = global_address,
                .table_slot = table_slot,
                .sp_global = sp_global,
                .fmod_index = fmod_index,
                .fmodf_index = fmodf_index,
            };
            ctx.emit();
            out.functions.push_back({
                .type_index = out.intern_type(defined_func_type(module, fn)),
                .locals = std::move(ctx.locals),
                .body = std::move(ctx.code),
            });
        }

        // ---- entry glue: an exported "main" wrapper --------------------------
        // Mirrors backend_x86's _start: call '_init' when defined, then the
        // user's main per its validated shape (or the test runner), and RETURN
        // the exit code — the embedder owns process exit on this target.
        int64_t main_index = -1;
        int64_t init_index = -1;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (!module.functions[i].has_body) continue;
            if (module.functions[i].name == "main") main_index = static_cast<int64_t>(i);
            if (module.functions[i].name == "_init") init_index = static_cast<int64_t>(i);
        }
        const bool test_mode = test_info != UINT32_MAX && test_runner != UINT32_MAX;
        if (main_index >= 0 || test_mode) {
            wasm::Code body;
            std::vector<ValType> locals;
            if (init_index >= 0) body.call(wasm_index[init_index]);
            if (test_mode) {
                body.i32_const(static_cast<int32_t>(global_address[test_info]));
                body.call(wasm_index[test_runner]);
                body.i32_const(0);
                body.return_op();
            } else {
                const auto &main_sig = module.signatures[module.functions[main_index].signature];
                const bool sret_main = main_sig.result == mir::Ty::Void && !main_sig.params.empty();
                if (sret_main) {
                    // main '-> error(...)': the wrapper owns the sret blob on the
                    // shadow stack; exit code is (tag != 0), as on x86.
                    locals.push_back(ValType::I32); // 0: blob pointer
                    body.global_get(sp_global);
                    body.i32_const(128);
                    body.op(op::i32_sub);
                    body.local_tee(0);
                    body.global_set(sp_global);
                    body.local_get(0);
                    body.call(wasm_index[main_index]);
                    body.local_get(0);
                    body.load(ValType::I32, 32, false, 0, 0);
                    body.i32_const(0);
                    body.op(op::i32_ne);
                    body.local_get(0);
                    body.i32_const(128);
                    body.op(op::i32_add);
                    body.global_set(sp_global);
                    body.return_op();
                } else if (main_sig.result == mir::Ty::Void) {
                    body.call(wasm_index[main_index]);
                    body.i32_const(0);
                    body.return_op();
                } else {
                    body.call(wasm_index[main_index]);
                    body.return_op();
                }
            }
            const auto wrapper_type = out.intern_type({{}, {ValType::I32}});
            const auto wrapper_index = import_count + static_cast<uint32_t>(out.functions.size());
            out.functions.push_back({wrapper_type, std::move(locals), std::move(body)});
            out.exports.push_back({"main", wasm::ExportKind::Function, wrapper_index});
        }

        // ---- exports ---------------------------------------------------------
        out.exports.push_back({"memory", wasm::ExportKind::Memory, 0});
        out.exports.push_back({"__heap_base", wasm::ExportKind::Global, heap_base_global});
        std::unordered_set<std::string> taken{"main", "memory", "__heap_base"};
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body || fn.linkage != mir::Linkage::External) continue;
            if (!taken.insert(fn.name).second) continue;
            out.exports.push_back({fn.name, wasm::ExportKind::Function, wasm_index[i]});
        }

        if (!result.errors.empty()) return result;
        result.bytes = out.serialize();
        result.ok = true;
        return result;
    }

    auto generate_object(const mir::Module &module, const uint32_t test_info,
                          const uint32_t test_runner) -> Result {
        Result result;
        if (module.pointer_bits != 32) {
            result.errors.push_back("wasm backend: module was lowered with 64-bit pointers; "
                                     "compile with a wasm32 target");
            return result;
        }

        wasm::ObjectModule out;

        // ---- reference scan (as in generate) ---------------------------------
        std::unordered_set<uint32_t> referenced;
        bool needs_fmod = false;
        bool needs_fmodf = false;
        std::vector<bool> address_taken(module.functions.size(), false);
        for (const auto &fn : module.functions) {
            if (!fn.has_body) continue;
            for (const auto &block : fn.blocks) {
                for (const auto &inst : block.insts) {
                    if (inst.op == mir::Op::Call) referenced.insert(inst.a);
                    if (inst.op == mir::Op::FuncAddr) {
                        referenced.insert(inst.a);
                        address_taken[inst.a] = true;
                    }
                    if (inst.op == mir::Op::FRem) {
                        (inst.type == mir::Ty::F64 ? needs_fmod : needs_fmodf) = true;
                    }
                }
            }
        }
        for (const auto &global : module.globals) {
            for (const auto &reloc : global.relocations) {
                if (reloc.kind == mir::Relocation::Kind::FunctionAddr) {
                    referenced.insert(reloc.target);
                    address_taken[reloc.target] = true;
                }
            }
        }

        std::vector<uint32_t> type_of_sig(module.signatures.size());
        for (size_t i = 0; i < module.signatures.size(); ++i) {
            type_of_sig[i] = out.intern_type(func_type_of(module, static_cast<uint32_t>(i)));
        }

        // ---- function index space and symbols, imports first ------------------
        // Symbol i corresponds to function index i (imports, then defined, then
        // the entry wrapper), followed by data symbols, then __stack_pointer.
        std::vector<uint32_t> wasm_index(module.functions.size(), UINT32_MAX);
        std::vector<uint32_t> fn_symbol(module.functions.size(), UINT32_MAX);
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (fn.has_body || !referenced.contains(static_cast<uint32_t>(i))) continue;
            wasm_index[i] = static_cast<uint32_t>(out.imports.size());
            fn_symbol[i] = static_cast<uint32_t>(out.function_symbols.size());
            out.imports.push_back({
                .module = fn.import_module.empty() ? "env" : fn.import_module,
                .name = fn.import_name.empty() ? fn.name : fn.import_name,
                .type_index = type_of_sig[fn.signature],
            });
            out.function_symbols.push_back({.name = {}, .function_index = wasm_index[i],
                                             .defined = false, .exported = false});
        }
        int64_t fmod_index = -1;
        int64_t fmodf_index = -1;
        int64_t fmod_symbol = -1;
        int64_t fmodf_symbol = -1;
        const auto add_runtime_import = [&](const char *name, const ValType ty) {
            const auto index = static_cast<int64_t>(out.imports.size());
            const auto symbol = static_cast<int64_t>(out.function_symbols.size());
            out.imports.push_back({"env", name, out.intern_type({{ty, ty}, {ty}})});
            out.function_symbols.push_back({.name = {}, .function_index = static_cast<uint32_t>(index),
                                             .defined = false, .exported = false});
            return std::pair{index, symbol};
        };
        if (needs_fmod) std::tie(fmod_index, fmod_symbol) = add_runtime_import("fmod", ValType::F64);
        if (needs_fmodf) std::tie(fmodf_index, fmodf_symbol) = add_runtime_import("fmodf", ValType::F32);

        const auto import_count = static_cast<uint32_t>(out.imports.size());
        uint32_t defined = 0;
        int64_t main_index = -1;
        int64_t init_index = -1;
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body) continue;
            wasm_index[i] = import_count + defined++;
            fn_symbol[i] = static_cast<uint32_t>(out.function_symbols.size());
            if (fn.name == "main") main_index = static_cast<int64_t>(i);
            if (fn.name == "_init") init_index = static_cast<int64_t>(i);
            out.function_symbols.push_back({
                .name = fn.name,
                .function_index = wasm_index[i],
                .defined = true,
                .exported = fn.linkage == mir::Linkage::External && fn.name != "main",
            });
        }
        // The entry wrapper takes the C name 'main' (emscripten's runtime calls
        // it); the user's own main keeps its body under an internal spelling.
        const bool test_mode = test_info != UINT32_MAX && test_runner != UINT32_MAX;
        const bool have_wrapper = main_index >= 0 || test_mode;
        uint32_t wrapper_index = 0;
        if (have_wrapper) {
            if (main_index >= 0) out.function_symbols[fn_symbol[main_index]].name = "__original_main";
            wrapper_index = import_count + defined;
            out.function_symbols.push_back({.name = "main", .function_index = wrapper_index,
                                             .defined = true, .exported = true});
        }

        // ---- table slots for address-taken functions ---------------------------
        for (size_t i = 0; i < module.functions.size(); ++i) {
            if (address_taken[i] && wasm_index[i] != UINT32_MAX) {
                out.table_elements.push_back(wasm_index[i]);
            }
        }

        // ---- data segments and symbols ----------------------------------------
        // One segment per global; layout is wasm-ld's. BSS globals carry a
        // zero-filled segment — small in this compiler's output, and simpler
        // than the WASM_SEG_FLAG dance.
        std::vector<uint32_t> data_symbol(module.globals.size(), UINT32_MAX);
        const auto data_symbol_base = static_cast<uint32_t>(out.function_symbols.size());
        for (size_t i = 0; i < module.globals.size(); ++i) {
            const auto &global = module.globals[i];
            const auto segment = static_cast<uint32_t>(out.segments.size());
            data_symbol[i] = data_symbol_base + static_cast<uint32_t>(out.data_symbols.size());
            const char *prefix = global.init.empty() ? ".bss."
                                : global.is_constant ? ".rodata." : ".data.";
            wasm::ObjectSegment seg;
            seg.name = prefix + global.name;
            seg.align_log2 = static_cast<uint32_t>(std::countr_zero(std::max(1u, global.align)));
            seg.bytes = global.init.empty()
                ? std::vector<uint8_t>(std::max(1u, global.size), 0) : global.init;
            for (const auto &reloc : global.relocations) {
                if (reloc.kind == mir::Relocation::Kind::FunctionAddr) {
                    seg.relocs.push_back({wasm::reloc::TABLE_INDEX_I32, reloc.offset,
                                          fn_symbol[reloc.target], 0});
                } else {
                    seg.relocs.push_back({wasm::reloc::MEMORY_ADDR_I32, reloc.offset,
                                          /*patched below*/ 0, reloc.addend});
                    seg.relocs.back().index = data_symbol_base; // placeholder
                }
            }
            out.segments.push_back(std::move(seg));
            out.data_symbols.push_back({.name = global.name, .segment = segment,
                                         .offset = 0, .size = std::max(1u, global.size)});
        }
        // Data->data relocations could not know their targets' symbol indices on
        // the first pass; resolve them now.
        for (size_t i = 0; i < module.globals.size(); ++i) {
            auto &seg = out.segments[i];
            size_t entry = 0;
            for (const auto &reloc : module.globals[i].relocations) {
                if (reloc.kind == mir::Relocation::Kind::GlobalAddr) {
                    seg.relocs[entry].index = data_symbol[reloc.target];
                }
                ++entry;
            }
        }
        out.import_stack_pointer = true;
        const auto sp_symbol = data_symbol_base + static_cast<uint32_t>(out.data_symbols.size());

        // ---- bodies ----------------------------------------------------------
        std::vector<uint32_t> no_slots(module.functions.size(), 0);
        std::vector<uint32_t> empty_addresses(module.globals.size(), 0);
        for (size_t i = 0; i < module.functions.size(); ++i) {
            const auto &fn = module.functions[i];
            if (!fn.has_body) continue;
            FunctionContext ctx{
                .module = module,
                .fn = fn,
                .errors = result.errors,
                .wasm_index = wasm_index,
                .type_of_sig = type_of_sig,
                .global_address = empty_addresses,
                .table_slot = no_slots,
                .sp_global = 0,
                .fmod_index = fmod_index,
                .fmodf_index = fmodf_index,
                .object_mode = true,
                .fn_symbol = &fn_symbol,
                .data_symbol = &data_symbol,
                .sp_symbol = sp_symbol,
                .fmod_symbol = fmod_symbol,
                .fmodf_symbol = fmodf_symbol,
            };
            ctx.emit();
            out.functions.push_back({
                .type_index = out.intern_type(defined_func_type(module, fn)),
                .locals = std::move(ctx.locals),
                .body = std::move(ctx.code),
            });
        }

        // ---- entry glue: the C main(argc, argv) emscripten calls ---------------
        if (have_wrapper) {
            wasm::Code body;
            std::vector<ValType> locals;
            if (init_index >= 0) body.call_reloc(wasm_index[init_index], fn_symbol[init_index]);
            if (test_mode) {
                body.i32_const_memory(data_symbol[test_info], 0);
                body.call_reloc(wasm_index[test_runner], fn_symbol[test_runner]);
                body.i32_const(0);
                body.return_op();
            } else {
                const auto &main_sig = module.signatures[module.functions[main_index].signature];
                const bool sret_main = main_sig.result == mir::Ty::Void && !main_sig.params.empty();
                if (sret_main) {
                    locals.push_back(ValType::I32); // 2: blob pointer (after argc/argv)
                    const uint32_t blob = 2;
                    body.global_get_reloc(0, sp_symbol);
                    body.i32_const(128);
                    body.op(op::i32_sub);
                    body.local_tee(blob);
                    body.global_set_reloc(0, sp_symbol);
                    body.local_get(blob);
                    body.call_reloc(wasm_index[main_index], fn_symbol[main_index]);
                    body.local_get(blob);
                    body.load(ValType::I32, 32, false, 0, 0);
                    body.i32_const(0);
                    body.op(op::i32_ne);
                    body.local_get(blob);
                    body.i32_const(128);
                    body.op(op::i32_add);
                    body.global_set_reloc(0, sp_symbol);
                    body.return_op();
                } else if (main_sig.result == mir::Ty::Void) {
                    body.call_reloc(wasm_index[main_index], fn_symbol[main_index]);
                    body.i32_const(0);
                    body.return_op();
                } else {
                    body.call_reloc(wasm_index[main_index], fn_symbol[main_index]);
                    body.return_op();
                }
            }
            const auto wrapper_type = out.intern_type({{ValType::I32, ValType::I32}, {ValType::I32}});
            out.functions.push_back({wrapper_type, std::move(locals), std::move(body)});
        }

        if (!result.errors.empty()) return result;
        result.bytes = out.serialize();
        result.ok = true;
        return result;
    }
}
