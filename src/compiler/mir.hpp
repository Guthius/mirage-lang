#pragma once

// Mirage IR (MIR) — the target-independent intermediate representation the native x86-64
// and wasm backends are being built on, replacing LLVM.
//
// WHY THIS SHAPE
//
// Two facts about the existing compiler decide almost every choice here, and both are worth
// stating because neither is obvious:
//
//  1. SEMA OWNS LAYOUT. resolved_type_size/resolved_type_align and StructInfo::fields[].offset
//     are computed in the front end from Options::pointer_size; no data-layout query
//     influences a single layout decision. So MIR needs no structural type system and no
//     layout engine: a type is a size, an alignment and a scalar kind, and everything else
//     is a byte offset sema already computed. That is why Ty below is eight scalars and
//     nothing more, and why there is no getelementptr — every address computation is plain
//     pointer arithmetic.
//
//  2. THE FRONT END EMITS MEMORY FORM, NOT SSA. Every local is a stack slot written and read
//     through load/store; the LLVM emitter it replaces needed phi nodes in exactly three
//     places. So no SSA construction is required, and a simple register allocator suffices.
//     Block parameters (rather than phi) carry the three real cases and are easier both to
//     allocate registers for and to lower to wasm's structured control flow.
//
// A third constraint governs the CODING STYLE rather than the design: this compiler is going
// to be rewritten in Mirage. Everything here is therefore data-oriented and index-based —
// flat vectors with u32 handles, no inheritance, no variant-of-unique_ptr, no templates —
// so the port is a transliteration rather than a redesign. sema::Program's parallel arenas
// (struct_index, union_index, …) are the model; this is more of the same.
//
// AGGREGATES ARE MEMORY. A MIR value is always one machine scalar. Aggregates live in stack
// slots and are manipulated through load/store at sema-computed offsets. This is what lets
// instruction selection, register allocation and the wasm encoder all stay simple, and it
// costs nothing in expressiveness precisely because of fact (1).

#include <cstdint>
#include <string>
#include <vector>

namespace mir {
    // The only types MIR has. Anything larger lives in memory and is reached through Ptr.
    // I1 is the comparison/branch type; it is stored as a byte, which is a backend concern
    // rather than one visible here.
    enum class Ty : uint8_t { Void, I1, I8, I16, I32, I64, F32, F64, Ptr };

    [[nodiscard]] auto type_name(Ty type) -> const char *;
    // Width in BITS, or 0 for Void. Ptr answers the target's pointer width, which is why
    // this takes it as a parameter rather than assuming 64.
    [[nodiscard]] auto type_bits(Ty type, uint32_t pointer_bits) -> uint32_t;
    [[nodiscard]] auto is_float(Ty type) -> bool;
    [[nodiscard]] auto is_integer(Ty type) -> bool;

    enum class Op : uint8_t {
        // --- constants -------------------------------------------------------------
        ConstInt,      // imm = the value (sign/zero interpretation is the type's)
        ConstFloat,    // imm = the bit pattern of the f32/f64
        ConstNull,     // a null pointer
        // Address of a Module::globals entry. 'a' is the global index.
        GlobalAddr,
        // Address of a Function::slots entry. 'a' is the slot index.
        SlotAddr,
        // Address of a Module::functions entry, as a code pointer. 'a' is the function index.
        FuncAddr,

        // --- memory ----------------------------------------------------------------
        Load,          // a = address
        Store,         // a = address, b = value        (defines nothing)
        // 'a' = destination, 'b' = source, 'c' = byte count. Lowered to a libc call or an
        // inline sequence per target; kept as one op so the decision is the backend's.
        MemCopy,
        MemSet,        // a = destination, b = byte value (I8), c = byte count

        // --- address arithmetic ----------------------------------------------------
        // Replaces LLVM's typed getelementptr entirely: because sema computed every offset,
        // both forms below are plain pointer arithmetic.
        PtrAddConst,   // a = base, imm = signed byte offset
        PtrAdd,        // a = base, b = byte offset (target word width)

        // --- integer arithmetic ----------------------------------------------------
        Add, Sub, Mul, SDiv, UDiv, SRem, URem,
        And, Or, Xor, Shl, LShr, AShr,
        Not, Neg,

        // --- float arithmetic ------------------------------------------------------
        FAdd, FSub, FMul, FDiv, FRem, FNeg,

        // --- comparison (all define I1) --------------------------------------------
        ICmpEq, ICmpNe,
        ICmpSlt, ICmpSle, ICmpSgt, ICmpSge,
        ICmpUlt, ICmpUle, ICmpUgt, ICmpUge,
        // Ordered float comparisons only, matching what the front end emits.
        FCmpOeq, FCmpOne, FCmpOlt, FCmpOle, FCmpOgt, FCmpOge,

        // --- conversion ------------------------------------------------------------
        Trunc, ZExt, SExt,
        FPTrunc, FPExt,
        FPToSI, FPToUI, SIToFP, UIToFP,
        PtrToInt, IntToPtr,
        // A no-op reinterpretation between same-width scalars. Distinct from the casts
        // above so the verifier can insist those actually change something.
        Bitcast,

        Select,        // a = condition (I1), b = if-true, c = if-false

        // --- calls -----------------------------------------------------------------
        // 'a' indexes Module::functions; arguments are in Inst::args. Defines the return
        // value, or nothing when the callee returns Void.
        Call,
        // 'a' = callee address value, 'b' = Module::signatures index. wasm's call_indirect
        // needs the signature as a type index, which is why it is carried explicitly rather
        // than inferred.
        CallIndirect,

        // --- terminators -----------------------------------------------------------
        Jump,          // a = target block; args are the target's block arguments
        Branch,        // a = condition (I1), b = true block, c = false block
        // 'a' = scrutinee. Cases live in Inst::args as (value, block) pairs; 'b' is the
        // default block.
        Switch,
        Return,        // args = returned values (0 or 1; multi-return is lowered before MIR)
        Unreachable,
    };

    [[nodiscard]] auto op_name(Op op) -> const char *;
    [[nodiscard]] auto is_terminator(Op op) -> bool;
    // Whether the op defines a value. Store/MemCopy/MemSet and every terminator do not; a
    // Call defines one only when its callee returns non-Void, so that case is decided by
    // the caller rather than here.
    [[nodiscard]] auto defines_value(Op op) -> bool;

    // A value is an instruction result or a block parameter, identified by a dense index
    // into Function::values. NO_VALUE is the absent-operand marker.
    using ValueId = uint32_t;
    using BlockId = uint32_t;
    inline constexpr uint32_t NO_VALUE = UINT32_MAX;
    inline constexpr uint32_t NO_BLOCK = UINT32_MAX;

    struct Inst {
        Op op = Op::Unreachable;
        Ty type = Ty::Void;      // type of the defined value; Void when nothing is defined
        ValueId result = NO_VALUE;
        // Operand meaning is per-op; see the Op comments above. NO_VALUE when unused.
        uint32_t a = NO_VALUE;
        uint32_t b = NO_VALUE;
        uint32_t c = NO_VALUE;
        int64_t imm = 0;         // constants, PtrAddConst's offset
        // Call arguments, Jump block arguments, Switch (value, block) pairs, Return values.
        // A separate vector rather than a side table so an Inst is self-contained, which is
        // what makes the eventual Mirage port a transliteration.
        std::vector<uint32_t> args;
        // Source position, for diagnostics emitted after the front end is done with the AST.
        uint32_t line = 0;
        uint32_t column = 0;
    };

    // Where a value came from, so the verifier and printer can talk about it without a
    // separate def-use map.
    struct ValueDef {
        BlockId block = NO_BLOCK;
        // Index into Block::insts, or the block-parameter index when is_param.
        uint32_t index = 0;
        Ty type = Ty::Void;
        bool is_param = false;
    };

    struct Block {
        std::string label;
        // Parameters replace phi nodes: a predecessor passes matching arguments on its
        // Jump/Branch. Strictly simpler to allocate registers for, and it is what wasm's
        // structured control flow wants anyway.
        std::vector<ValueId> params;
        std::vector<Inst> insts;
    };

    // An addressable stack slot. Every aggregate local is one of these; a scalar local
    // starts as one too and is promoted to a value by the promote-slots pass when its
    // address never escapes.
    struct Slot {
        uint32_t size = 0;
        uint32_t align = 1;
        std::string name;
        // Set when the slot's address is passed to a call, stored, or converted to an
        // integer. The promote-slots pass refuses to touch such a slot.
        bool address_escapes = false;
    };

    // A deduplicated function signature. wasm's call_indirect needs these as type indices;
    // x86-64 ignores them.
    struct Signature {
        std::vector<Ty> params;
        Ty result = Ty::Void;
        bool is_variadic = false;
    };

    enum class Linkage : uint8_t { Internal, External };

    // How a function's arguments and result cross its boundary. Mirage passes aggregates
    // raw (the compiler owns both sides); C routes them through the platform ABI. Mirrors
    // sema::CallConv, kept separate so MIR does not depend on sema.
    enum class CallConv : uint8_t { Mirage, C };

    struct Function {
        std::string name;
        Linkage linkage = Linkage::Internal;
        CallConv conv = CallConv::Mirage;
        uint32_t signature = 0;          // index into Module::signatures
        std::vector<ValueId> params;     // entry block's parameters, in order
        std::vector<Block> blocks;
        std::vector<Slot> slots;
        std::vector<ValueDef> values;
        // False for a declaration with no body (an 'ext fn', or an imported wasm function).
        bool has_body = false;
        // Wasm import binding ('@import'); empty on a definition or a native target.
        std::string import_module;
        std::string import_name;
        std::string section;             // '@section'; empty means the default
    };

    // One relocation inside a global's initializer bytes: at 'offset', store the address of
    // 'target' plus 'addend'. This is how a vtable entry or a string-slice pointer gets its
    // value, since neither is known until layout.
    struct Relocation {
        enum class Kind : uint8_t { FunctionAddr, GlobalAddr };
        Kind kind = Kind::GlobalAddr;
        uint32_t offset = 0;   // byte offset within the global's initializer
        uint32_t target = 0;   // function or global index, per 'kind'
        int64_t addend = 0;
    };

    struct Global {
        std::string name;
        Linkage linkage = Linkage::Internal;
        uint32_t size = 0;
        uint32_t align = 1;
        bool is_constant = false;
        // Initializer bytes. Empty means zero-initialized (.bss); otherwise size() == size.
        std::vector<uint8_t> init;
        std::vector<Relocation> relocations;
        std::string section;
    };

    struct Module {
        std::string name;
        uint32_t pointer_bits = 64;
        std::vector<Function> functions;
        std::vector<Global> globals;
        std::vector<Signature> signatures;

        // Returns an existing index for an identical signature, or appends. Deduplication is
        // required for wasm (a type index must be unique per shape) and free elsewhere.
        auto intern_signature(Signature sig) -> uint32_t;
    };

    // Builds one function's body. Mirrors llvm::IRBuilder's method surface deliberately:
    // porting the 6,900-line LLVM emitter is then a mechanical rename rather than a rewrite,
    // and the three places where it cannot be mechanical (GEP, aggregates, inline asm) are
    // exactly the three worth concentrating review on.
    class Builder {
      public:
        Builder(Module &module, uint32_t function_index);

        [[nodiscard]] auto function() -> Function & { return module_.functions[function_]; }
        [[nodiscard]] auto function() const -> const Function & { return module_.functions[function_]; }

        auto create_block(std::string label) -> BlockId;
        auto add_block_param(BlockId block, Ty type) -> ValueId;
        void set_insert_point(BlockId block);
        [[nodiscard]] auto insert_block() const -> BlockId { return current_; }
        // Whether the current block already ends in a terminator. The emitter checks this
        // constantly ("if the block is closed, stop emitting"), so it is part of the API.
        [[nodiscard]] auto block_is_terminated() const -> bool;

        auto add_slot(uint32_t size, uint32_t align, std::string name) -> uint32_t;
        void mark_slot_escaping(uint32_t slot);

        void set_location(uint32_t line, uint32_t column);

        // --- constants ---
        auto const_int(Ty type, int64_t value) -> ValueId;
        auto const_float(Ty type, double value) -> ValueId;
        auto const_null() -> ValueId;
        auto global_addr(uint32_t global_index) -> ValueId;
        auto slot_addr(uint32_t slot_index) -> ValueId;
        auto func_addr(uint32_t function_index) -> ValueId;

        // --- memory ---
        auto load(Ty type, ValueId address) -> ValueId;
        void store(ValueId address, ValueId value);
        void mem_copy(ValueId dst, ValueId src, ValueId bytes);
        void mem_set(ValueId dst, ValueId byte, ValueId bytes);

        // --- address arithmetic (replaces getelementptr) ---
        auto ptr_add_const(ValueId base, int64_t byte_offset) -> ValueId;
        auto ptr_add(ValueId base, ValueId byte_offset) -> ValueId;

        // --- arithmetic / comparison / conversion ---
        auto binary(Op op, Ty type, ValueId lhs, ValueId rhs) -> ValueId;
        auto unary(Op op, Ty type, ValueId operand) -> ValueId;
        auto compare(Op op, ValueId lhs, ValueId rhs) -> ValueId;
        auto convert(Op op, Ty type, ValueId operand) -> ValueId;
        auto select(ValueId condition, ValueId if_true, ValueId if_false, Ty type) -> ValueId;

        // --- calls ---
        auto call(uint32_t callee, Ty result_type, const std::vector<ValueId> &args) -> ValueId;
        auto call_indirect(ValueId callee, uint32_t signature, Ty result_type,
                            const std::vector<ValueId> &args) -> ValueId;

        // --- terminators ---
        void jump(BlockId target, const std::vector<ValueId> &args = {});
        void branch(ValueId condition, BlockId if_true, BlockId if_false);
        void switch_on(ValueId scrutinee, BlockId default_block,
                        const std::vector<std::pair<int64_t, BlockId>> &cases);
        void ret(ValueId value = NO_VALUE);
        void unreachable();

        [[nodiscard]] auto value_type(ValueId value) const -> Ty;

      private:
        auto emit(Inst inst) -> ValueId;
        auto new_value(Ty type, BlockId block, uint32_t index, bool is_param) -> ValueId;

        Module &module_;
        uint32_t function_;
        BlockId current_ = NO_BLOCK;
        uint32_t line_ = 0;
        uint32_t column_ = 0;
    };

    // A structural problem found by verify(). Message is human-readable and names the
    // function and block; 'function' indexes Module::functions.
    struct VerifyError {
        uint32_t function = 0;
        std::string message;
    };

    // The analogue of llvm::verifyModule, which the LLVM path calls on every compile and
    // which has caught real bugs (see create_entry_alloca's comment about an instruction
    // used outside its own function). Checks: every value defined before use and in a block
    // that dominates — approximated here by "defined in this function at all", since the
    // emitter is memory-form and does not build the kind of CFG where the distinction
    // bites; exactly one terminator, at the end of each block; branch targets in range;
    // block-parameter arity and types matching every predecessor's arguments; operand types
    // matching the op.
    //
    // Returns an empty vector when the module is well-formed.
    [[nodiscard]] auto verify(const Module &module) -> std::vector<VerifyError>;

    // promote_slots (stage 3) — mem2reg-lite. A slot whose address never escapes,
    // accessed only by full-width loads/stores of one scalar type, becomes a value;
    // merge points become block parameters, with branch/switch edges into them split
    // through jump-only trampolines (those terminators cannot carry block arguments).
    // Runs in place; the result must still pass verify(). See mir_passes.cpp for the
    // algorithm notes.
    struct PromoteStats {
        size_t slots_promoted = 0;
        size_t params_added = 0;
    };
    auto promote_slots(Module &module) -> PromoteStats;

    // peephole (stage 3) — local cleanups to a fixpoint: width-correct integer
    // constant folding, identity simplification (x+0, x*1, ptr.add p,0 ...), trivial
    // and unused block-parameter removal (undoing promote_slots' deliberate
    // redundancy), and dead pure-instruction elimination. Runs in place; the result
    // must still pass verify().
    struct PeepholeStats {
        size_t folded = 0;
        size_t simplified = 0;
        size_t params_removed = 0;
        size_t dead_removed = 0;
    };
    auto peephole(Module &module) -> PeepholeStats;

    // Textual MIR: the primary debugging surface for the whole backend effort, and what
    // '--emit-mir' prints. Stable and diffable — two compiles of the same input must
    // produce byte-identical output, which is why nothing here is keyed on a hash map.
    [[nodiscard]] auto print(const Module &module) -> std::string;
    [[nodiscard]] auto print(const Module &module, const Function &function) -> std::string;
}
