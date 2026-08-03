#pragma once

// x86-64 instruction encoder (stage 4, docs/backend.md). Emits machine code bytes
// into a growable buffer, with intra-function branches resolved through labels and
// cross-object references recorded as relocations against symbol indices the caller
// owns (the ELF writer maps them to its own symbol table).
//
// Deliberately small: exactly the instructions the trivial-regalloc ISel emits, in
// exactly the addressing modes it uses — register-register, register-immediate, and
// [base + disp32] memory operands, plus RIP-relative addresses for globals. Nothing
// speculative; every addition should come with an encoding test pinning its bytes
// (the "encoder differential" in the validation plan).
//
// Encoding notes, so the tables below are checkable against the manual:
//   REX = 0x40 | W<<3 | R<<2 | X<<1 | B. W selects 64-bit operands; R extends the
//   ModRM reg field, B the r/m (or opcode-embedded register), X an SIB index.
//   ModRM = mod<<6 | reg<<3 | rm. mod 0b11 is register-direct; 0b10 is [base+disp32].
//   [RSP+disp] needs an SIB byte (rm=100 selects SIB); [RBP] needs an explicit disp.
//   RIP-relative is mod=00, rm=101, disp32 — patched by a PC-relative relocation.

#include <cstdint>
#include <vector>

namespace x86 {
    // Order is the hardware encoding (the low three bits + REX extension bit).
    enum class Reg : uint8_t {
        RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
        R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15,
    };
    enum class XReg : uint8_t {
        XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3, XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7,
    };

    // Operand width in bits. Byte operations on SPL/BPL/SIL/DIL need a REX prefix
    // even without extension bits; the encoder handles that.
    enum class Width : uint8_t { W8 = 8, W16 = 16, W32 = 32, W64 = 64 };

    enum class Cond : uint8_t {
        // The low nibble of the 0F 9x setcc / 0F 8x jcc opcodes.
        E = 0x4, NE = 0x5,
        L = 0xC, LE = 0xE, G = 0xF, GE = 0xD,   // signed
        B = 0x2, BE = 0x6, A = 0x7, AE = 0x3,   // unsigned
        P = 0xA, NP = 0xB,                       // parity, for float equality
    };

    // ALU operations that share the classic /r encoding family. The value is the
    // opcode "slot" (the /digit for the 0x81 immediate form, and selects the
    // register-form opcode as slot*8 + 0x01/0x03).
    enum class Alu : uint8_t { Add = 0, Or = 1, And = 4, Sub = 5, Xor = 6, Cmp = 7 };

    struct Relocation {
        enum class Kind : uint8_t {
            // call/jmp rel32 to another function — PLT32 in ELF terms.
            Call32,
            // RIP-relative disp32 to a data symbol — PC32 with addend -4.
            Rip32,
        };
        Kind kind = Kind::Rip32;
        uint32_t offset = 0;   // where the 4-byte field sits in 'code'
        uint32_t symbol = 0;   // caller-defined symbol index
        int64_t addend = 0;    // extra displacement into the target
    };

    using Label = uint32_t;

    class Encoder {
      public:
        std::vector<uint8_t> code;
        std::vector<Relocation> relocations;

        // --- labels (intra-function control flow) ---
        auto make_label() -> Label;
        void bind(Label label);
        [[nodiscard]] auto label_offset(Label label) const -> uint32_t;
        // Called once after emission: patches every recorded jump's rel32.
        void resolve_labels();

        // --- moves ---
        void mov_ri(Reg dst, int64_t imm);                    // 64-bit materialize (movabs / mov r32)
        void mov_rr(Width w, Reg dst, Reg src);
        void load(Width w, Reg dst, Reg base, int32_t disp);  // mov dst, [base+disp]
        void store(Width w, Reg base, int32_t disp, Reg src); // mov [base+disp], src
        void movzx(Width from, Reg dst, Reg src);             // 8/16/32 -> 64
        void movsx(Width from, Reg dst, Reg src);             // 8/16/32 -> 64
        void lea(Reg dst, Reg base, int32_t disp);
        // lea dst, [rip + sym]: records a Rip32 relocation.
        void lea_rip(Reg dst, uint32_t symbol, int64_t addend);

        // --- integer arithmetic (64-bit unless noted) ---
        void alu_rr(Alu op, Width w, Reg dst, Reg src);
        void alu_ri(Alu op, Width w, Reg dst, int32_t imm);
        void imul_rr(Reg dst, Reg src);
        // div/idiv rdx:rax by src; quotient in rax, remainder in rdx. The caller
        // must have set up rdx (cqo/xor) — exposed separately because signed and
        // unsigned differ exactly there.
        void cqo();
        void zero(Reg reg); // xor r32, r32
        void div_r(Reg src);
        void idiv_r(Reg src);
        void neg_r(Width w, Reg reg);
        void not_r(Width w, Reg reg);
        // Shift by CL.
        void shl_cl(Width w, Reg reg);
        void shr_cl(Width w, Reg reg);
        void sar_cl(Width w, Reg reg);

        void test_rr(Width w, Reg a, Reg b);
        void setcc(Cond cond, Reg dst8);

        // --- SSE scalar float ---
        void movss_load(XReg dst, Reg base, int32_t disp);
        void movss_store(Reg base, int32_t disp, XReg src);
        void movsd_load(XReg dst, Reg base, int32_t disp);
        void movsd_store(Reg base, int32_t disp, XReg src);
        void sse_arith(uint8_t opcode, bool is_double, XReg dst, XReg src); // 0x58 add, 0x5C sub, 0x59 mul, 0x5E div
        void ucomis(bool is_double, XReg a, XReg b);
        void cvt_i2f(bool to_double, Width from, XReg dst, Reg src);  // cvtsi2ss/sd
        void cvt_f2i(bool from_double, Width to, Reg dst, XReg src);  // cvttss/sd2si
        void cvt_f2f(bool to_double, XReg dst, XReg src);             // cvtss2sd / cvtsd2ss
        void mov_r_x(XReg dst, Reg src);  // movq xmm, r64
        void mov_x_r(Reg dst, XReg src);  // movq r64, xmm

        // --- control flow ---
        void jmp(Label target);
        void jcc(Cond cond, Label target);
        void call_sym(uint32_t symbol);   // call rel32 + Call32 relocation
        void call_r(Reg target);
        void ret();
        void push_r(Reg reg);
        void pop_r(Reg reg);
        void sub_rsp(int32_t bytes);
        void add_rsp(int32_t bytes);
        void ud2();

      private:
        struct PendingJump {
            uint32_t patch_offset = 0; // where the rel32 field sits
            Label target = 0;
        };
        std::vector<uint32_t> labels_;      // label -> code offset (UINT32_MAX unbound)
        std::vector<PendingJump> pending_;

        void byte(uint8_t value) { code.push_back(value); }
        void imm32(int32_t value);
        void imm64(int64_t value);
        // REX prefix for a (reg, r/m) pair; emitted when any bit is set, or when a
        // byte op touches SPL/BPL/SIL/DIL (which need REX to mean those registers).
        void rex(bool w, uint8_t reg, uint8_t rm, bool force = false, uint8_t index = 0);
        void modrm_reg(uint8_t reg, uint8_t rm);
        // [base + disp32] with the RSP-SIB and RBP-disp special cases handled.
        void modrm_mem(uint8_t reg, Reg base, int32_t disp);
    };
}
