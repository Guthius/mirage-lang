#pragma once

// WebAssembly module encoder (stage 7, docs/backend.md): the byte-level half of
// the wasm backend. 'Code' builds one function body's instruction stream; 'Module'
// collects types, imports, functions, table, memory, globals, exports, element and
// data segments, and serializes the final binary. This layer knows nothing about
// MIR — backend_wasm.cpp does the translation — and stage 8's relocatable output
// is planned as a second serializer over this same structure, which is why the
// module is kept as data until serialize() rather than streamed.
//
// Only what the backend emits is implemented, mirroring the x86 encoder's
// posture: nothing speculative, and every opcode helper is exactly one wasm
// instruction so the translation in backend_wasm.cpp reads like the spec.

#include <cstdint>
#include <string>
#include <vector>

namespace wasm {
    enum class ValType : uint8_t {
        I32 = 0x7F,
        I64 = 0x7E,
        F32 = 0x7D,
        F64 = 0x7C,
        Funcref = 0x70,
    };

    struct FuncType {
        std::vector<ValType> params;
        std::vector<ValType> results;
        auto operator==(const FuncType &) const -> bool = default;
    };

    // One function body's instruction stream. Every method appends exactly one
    // instruction (or immediate); the caller owns structure and stack discipline.
    class Code {
      public:
        std::vector<uint8_t> bytes;

        // --- control ---
        void unreachable_op();
        void block_void();
        void loop_void();
        void if_void();
        void else_op();
        void end();
        void br(uint32_t depth);
        void br_if(uint32_t depth);
        void br_table(const std::vector<uint32_t> &depths, uint32_t default_depth);
        void return_op();
        void call(uint32_t function_index);
        void call_indirect(uint32_t type_index);
        void drop();
        void select_op();

        // --- locals and globals ---
        void local_get(uint32_t index);
        void local_set(uint32_t index);
        void local_tee(uint32_t index);
        void global_get(uint32_t index);
        void global_set(uint32_t index);

        // --- memory ---
        // 'align_log2' is the hint; offset is the constant addend.
        void load(ValType type, uint32_t bits, bool sign_extend, uint32_t align_log2,
                   uint32_t offset);
        void store(ValType type, uint32_t bits, uint32_t align_log2, uint32_t offset);
        void memory_copy();
        void memory_fill();

        // --- constants ---
        void i32_const(int32_t value);
        void i64_const(int64_t value);
        void f32_const_bits(uint32_t bits);
        void f64_const_bits(uint64_t bits);

        // --- numeric (opcode passed raw; named constants below) ---
        void op(uint8_t opcode);
        void op_fc(uint32_t sub); // 0xFC-prefixed (trunc_sat, bulk memory)

        void append(const Code &other);
    };

    // Raw opcode values for Code::op(), named where the backend uses them.
    namespace op {
        inline constexpr uint8_t i32_eqz = 0x45;
        inline constexpr uint8_t i32_eq = 0x46, i32_ne = 0x47;
        inline constexpr uint8_t i32_lt_s = 0x48, i32_lt_u = 0x49;
        inline constexpr uint8_t i32_gt_s = 0x4A, i32_gt_u = 0x4B;
        inline constexpr uint8_t i32_le_s = 0x4C, i32_le_u = 0x4D;
        inline constexpr uint8_t i32_ge_s = 0x4E, i32_ge_u = 0x4F;
        inline constexpr uint8_t i64_eqz = 0x50;
        inline constexpr uint8_t i64_eq = 0x51, i64_ne = 0x52;
        inline constexpr uint8_t i64_lt_s = 0x53, i64_lt_u = 0x54;
        inline constexpr uint8_t i64_gt_s = 0x55, i64_gt_u = 0x56;
        inline constexpr uint8_t i64_le_s = 0x57, i64_le_u = 0x58;
        inline constexpr uint8_t i64_ge_s = 0x59, i64_ge_u = 0x5A;
        inline constexpr uint8_t f32_eq = 0x5B, f32_ne = 0x5C, f32_lt = 0x5D,
                                  f32_gt = 0x5E, f32_le = 0x5F, f32_ge = 0x60;
        inline constexpr uint8_t f64_eq = 0x61, f64_ne = 0x62, f64_lt = 0x63,
                                  f64_gt = 0x64, f64_le = 0x65, f64_ge = 0x66;
        inline constexpr uint8_t i32_add = 0x6A, i32_sub = 0x6B, i32_mul = 0x6C;
        inline constexpr uint8_t i32_div_s = 0x6D, i32_div_u = 0x6E;
        inline constexpr uint8_t i32_rem_s = 0x6F, i32_rem_u = 0x70;
        inline constexpr uint8_t i32_and = 0x71, i32_or = 0x72, i32_xor = 0x73;
        inline constexpr uint8_t i32_shl = 0x74, i32_shr_s = 0x75, i32_shr_u = 0x76;
        inline constexpr uint8_t i64_add = 0x7C, i64_sub = 0x7D, i64_mul = 0x7E;
        inline constexpr uint8_t i64_div_s = 0x7F, i64_div_u = 0x80;
        inline constexpr uint8_t i64_rem_s = 0x81, i64_rem_u = 0x82;
        inline constexpr uint8_t i64_and = 0x83, i64_or = 0x84, i64_xor = 0x85;
        inline constexpr uint8_t i64_shl = 0x86, i64_shr_s = 0x87, i64_shr_u = 0x88;
        inline constexpr uint8_t f32_neg = 0x8C;
        inline constexpr uint8_t f32_add = 0x92, f32_sub = 0x93, f32_mul = 0x94, f32_div = 0x95;
        inline constexpr uint8_t f64_neg = 0x9A;
        inline constexpr uint8_t f64_add = 0xA0, f64_sub = 0xA1, f64_mul = 0xA2, f64_div = 0xA3;
        inline constexpr uint8_t i32_wrap_i64 = 0xA7;
        inline constexpr uint8_t i64_extend_i32_s = 0xAC, i64_extend_i32_u = 0xAD;
        inline constexpr uint8_t f32_convert_i32_s = 0xB2, f32_convert_i32_u = 0xB3;
        inline constexpr uint8_t f32_convert_i64_s = 0xB4, f32_convert_i64_u = 0xB5;
        inline constexpr uint8_t f32_demote_f64 = 0xB6;
        inline constexpr uint8_t f64_convert_i32_s = 0xB7, f64_convert_i32_u = 0xB8;
        inline constexpr uint8_t f64_convert_i64_s = 0xB9, f64_convert_i64_u = 0xBA;
        inline constexpr uint8_t f64_promote_f32 = 0xBB;
        inline constexpr uint8_t i32_reinterpret_f32 = 0xBC;
        inline constexpr uint8_t i64_reinterpret_f64 = 0xBD;
        inline constexpr uint8_t f32_reinterpret_i32 = 0xBE;
        inline constexpr uint8_t f64_reinterpret_i64 = 0xBF;
        inline constexpr uint8_t i32_extend8_s = 0xC0, i32_extend16_s = 0xC1;
        inline constexpr uint8_t i64_extend8_s = 0xC2, i64_extend16_s = 0xC3;
        inline constexpr uint8_t i64_extend32_s = 0xC4;
        // 0xFC-prefixed sub-opcodes (Code::op_fc).
        inline constexpr uint32_t i32_trunc_sat_f32_s = 0, i32_trunc_sat_f32_u = 1;
        inline constexpr uint32_t i32_trunc_sat_f64_s = 2, i32_trunc_sat_f64_u = 3;
        inline constexpr uint32_t i64_trunc_sat_f32_s = 4, i64_trunc_sat_f32_u = 5;
        inline constexpr uint32_t i64_trunc_sat_f64_s = 6, i64_trunc_sat_f64_u = 7;
    }

    struct Import {
        std::string module;
        std::string name;
        uint32_t type_index = 0; // function imports only — all this backend needs
    };

    struct Global {
        ValType type = ValType::I32;
        bool mutable_ = false;
        int64_t init = 0; // i32/i64 const initializer
    };

    enum class ExportKind : uint8_t { Function = 0, Table = 1, Memory = 2, Global = 3 };

    struct Export {
        std::string name;
        ExportKind kind = ExportKind::Function;
        uint32_t index = 0;
    };

    struct DataSegment {
        uint32_t offset = 0; // i32.const offset into the default memory
        std::vector<uint8_t> bytes;
    };

    struct Function {
        uint32_t type_index = 0;
        std::vector<ValType> locals; // beyond the parameters
        Code body;                    // WITHOUT the trailing 'end'; added on serialize
    };

    struct Module {
        std::vector<FuncType> types;
        std::vector<Import> imports;    // function imports; they occupy indices 0..n-1
        std::vector<Function> functions; // defined functions, indices continue after imports
        // Table 0: funcref, sized by the backend; element segment at offset 1 so a
        // null function pointer (0) traps instead of calling something.
        uint32_t table_size = 0;
        std::vector<uint32_t> table_elements; // function indices, placed from slot 1
        uint32_t memory_min_pages = 0;
        std::vector<Global> globals;
        std::vector<Export> exports;
        std::vector<DataSegment> data;

        auto intern_type(FuncType type) -> uint32_t;
        [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;
    };
}
