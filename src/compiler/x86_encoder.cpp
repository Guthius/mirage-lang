#include "x86_encoder.hpp"

// See the header for the encoding conventions. Every emitter here follows the same
// shape: optional prefixes (66 for 16-bit, F3/F2 for SSE scalar), optional REX,
// opcode byte(s), ModRM (+SIB), displacement, immediate. The unit tests pin the
// exact bytes of one instance of each form against hand-checked encodings.

#include <cassert>

namespace x86 {
    namespace {
        auto low3(const uint8_t reg) -> uint8_t { return reg & 7; }
        auto ext(const uint8_t reg) -> bool { return (reg & 8) != 0; }

        // Byte ops on SPL/BPL/SIL/DIL encode as AH/CH/DH/BH unless a REX prefix is
        // present; force one for them.
        auto needs_byte_rex(const uint8_t reg) -> bool { return reg >= 4 && reg <= 7; }
    }

    void Encoder::imm32(const int32_t value) {
        for (int i = 0; i < 4; ++i) byte(static_cast<uint8_t>(value >> (8 * i)));
    }
    void Encoder::imm64(const int64_t value) {
        for (int i = 0; i < 8; ++i) byte(static_cast<uint8_t>(value >> (8 * i)));
    }

    void Encoder::rex(const bool w, const uint8_t reg, const uint8_t rm, const bool force,
                       const uint8_t index) {
        uint8_t prefix = 0x40;
        if (w) prefix |= 0x08;
        if (ext(reg)) prefix |= 0x04;
        if (ext(index)) prefix |= 0x02;
        if (ext(rm)) prefix |= 0x01;
        if (prefix != 0x40 || force) byte(prefix);
    }

    void Encoder::modrm_reg(const uint8_t reg, const uint8_t rm) {
        byte(static_cast<uint8_t>(0xC0 | (low3(reg) << 3) | low3(rm)));
    }

    void Encoder::modrm_mem(const uint8_t reg, const Reg base, const int32_t disp) {
        const auto base_bits = static_cast<uint8_t>(base);
        // Always use disp32 (mod=10): the trivial allocator's frame offsets are not
        // worth a disp8 special case, and one form keeps the tests simple.
        byte(static_cast<uint8_t>(0x80 | (low3(reg) << 3) | low3(base_bits)));
        if (low3(base_bits) == 4) {
            // RSP/R12 as base require an SIB byte: scale=0, index=100 (none), base.
            byte(static_cast<uint8_t>(0x24));
        }
        imm32(disp);
    }

    // --- labels -------------------------------------------------------------------

    auto Encoder::make_label() -> Label {
        labels_.push_back(UINT32_MAX);
        return static_cast<Label>(labels_.size() - 1);
    }
    void Encoder::bind(const Label label) {
        labels_[label] = static_cast<uint32_t>(code.size());
    }
    auto Encoder::label_offset(const Label label) const -> uint32_t {
        return labels_[label];
    }
    void Encoder::resolve_labels() {
        for (const auto &jump : pending_) {
            const auto target = labels_[jump.target];
            assert(target != UINT32_MAX && "jump to an unbound label");
            const auto rel = static_cast<int32_t>(target) -
                             (static_cast<int32_t>(jump.patch_offset) + 4);
            for (int i = 0; i < 4; ++i) {
                code[jump.patch_offset + i] = static_cast<uint8_t>(rel >> (8 * i));
            }
        }
        pending_.clear();
    }

    // --- moves --------------------------------------------------------------------

    void Encoder::mov_ri(const Reg dst, const int64_t imm) {
        const auto d = static_cast<uint8_t>(dst);
        if (imm >= 0 && imm <= INT32_MAX) {
            // mov r32, imm32 zero-extends to 64 bits: shorter, same result.
            rex(false, 0, d);
            byte(static_cast<uint8_t>(0xB8 | low3(d)));
            imm32(static_cast<int32_t>(imm));
            return;
        }
        rex(true, 0, d);
        byte(static_cast<uint8_t>(0xB8 | low3(d)));
        imm64(imm);
    }

    void Encoder::mov_rr(const Width w, const Reg dst, const Reg src) {
        const auto d = static_cast<uint8_t>(dst);
        const auto s = static_cast<uint8_t>(src);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, s, d, w == Width::W8 && (needs_byte_rex(s) || needs_byte_rex(d)));
        byte(w == Width::W8 ? 0x88 : 0x89);
        modrm_reg(s, d);
    }

    void Encoder::load(const Width w, const Reg dst, const Reg base, const int32_t disp) {
        const auto d = static_cast<uint8_t>(dst);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, d, static_cast<uint8_t>(base), w == Width::W8 && needs_byte_rex(d));
        byte(w == Width::W8 ? 0x8A : 0x8B);
        modrm_mem(d, base, disp);
    }

    void Encoder::store(const Width w, const Reg base, const int32_t disp, const Reg src) {
        const auto s = static_cast<uint8_t>(src);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, s, static_cast<uint8_t>(base), w == Width::W8 && needs_byte_rex(s));
        byte(w == Width::W8 ? 0x88 : 0x89);
        modrm_mem(s, base, disp);
    }

    void Encoder::movzx(const Width from, const Reg dst, const Reg src) {
        const auto d = static_cast<uint8_t>(dst);
        const auto s = static_cast<uint8_t>(src);
        if (from == Width::W32) {
            // mov r32, r32 zero-extends; there is no movzx from 32.
            rex(false, s, d);
            byte(0x89);
            modrm_reg(s, d);
            return;
        }
        rex(true, d, s, from == Width::W8 && needs_byte_rex(s));
        byte(0x0F);
        byte(from == Width::W8 ? 0xB6 : 0xB7);
        modrm_reg(d, s);
    }

    void Encoder::movsx(const Width from, const Reg dst, const Reg src) {
        const auto d = static_cast<uint8_t>(dst);
        const auto s = static_cast<uint8_t>(src);
        rex(true, d, s, from == Width::W8 && needs_byte_rex(s));
        if (from == Width::W32) {
            byte(0x63); // movsxd
        } else {
            byte(0x0F);
            byte(from == Width::W8 ? 0xBE : 0xBF);
        }
        modrm_reg(d, s);
    }

    void Encoder::lea(const Reg dst, const Reg base, const int32_t disp) {
        const auto d = static_cast<uint8_t>(dst);
        rex(true, d, static_cast<uint8_t>(base));
        byte(0x8D);
        modrm_mem(d, base, disp);
    }

    void Encoder::lea_rip(const Reg dst, const uint32_t symbol, const int64_t addend) {
        const auto d = static_cast<uint8_t>(dst);
        rex(true, d, 5); // rm=101 selects RIP-relative under mod=00
        byte(0x8D);
        byte(static_cast<uint8_t>(0x00 | (low3(d) << 3) | 0x05));
        relocations.push_back({.kind = Relocation::Kind::Rip32,
                                .offset = static_cast<uint32_t>(code.size()),
                                .symbol = symbol, .addend = addend});
        imm32(0);
    }

    // --- integer arithmetic -------------------------------------------------------

    void Encoder::alu_rr(const Alu op, const Width w, const Reg dst, const Reg src) {
        const auto d = static_cast<uint8_t>(dst);
        const auto s = static_cast<uint8_t>(src);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, s, d, w == Width::W8 && (needs_byte_rex(s) || needs_byte_rex(d)));
        const auto base_opcode = static_cast<uint8_t>(static_cast<uint8_t>(op) * 8);
        byte(static_cast<uint8_t>(base_opcode | (w == Width::W8 ? 0x00 : 0x01)));
        modrm_reg(s, d);
    }

    void Encoder::alu_ri(const Alu op, const Width w, const Reg dst, const int32_t imm) {
        const auto d = static_cast<uint8_t>(dst);
        if (w == Width::W16) byte(0x66);
        // Byte ops on SPL/BPL/SIL/DIL need the REX prefix to mean those registers;
        // without it 'xor sil, 1' silently becomes 'xor dh, 1'. Found by the
        // linear-scan allocator, whose result registers reach here — the trivial
        // allocator's fixed scratch set (RAX/RCX/RDX) never could.
        rex(w == Width::W64, 0, d, w == Width::W8 && needs_byte_rex(d));
        byte(w == Width::W8 ? 0x80 : 0x81);
        modrm_reg(static_cast<uint8_t>(op), d);
        if (w == Width::W8) byte(static_cast<uint8_t>(imm));
        else if (w == Width::W16) { byte(static_cast<uint8_t>(imm)); byte(static_cast<uint8_t>(imm >> 8)); }
        else imm32(imm);
    }

    void Encoder::imul_rr(const Reg dst, const Reg src) {
        const auto d = static_cast<uint8_t>(dst);
        const auto s = static_cast<uint8_t>(src);
        rex(true, d, s);
        byte(0x0F);
        byte(0xAF);
        modrm_reg(d, s);
    }

    void Encoder::cqo() { byte(0x48); byte(0x99); }
    void Encoder::zero(const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(false, r, r);
        byte(0x31);
        modrm_reg(r, r);
    }
    void Encoder::div_r(const Reg src) {
        const auto s = static_cast<uint8_t>(src);
        rex(true, 0, s);
        byte(0xF7);
        modrm_reg(6, s);
    }
    void Encoder::idiv_r(const Reg src) {
        const auto s = static_cast<uint8_t>(src);
        rex(true, 0, s);
        byte(0xF7);
        modrm_reg(7, s);
    }
    void Encoder::neg_r(const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(w == Width::W64, 0, r);
        byte(0xF7);
        modrm_reg(3, r);
    }
    void Encoder::not_r(const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(w == Width::W64, 0, r);
        byte(0xF7);
        modrm_reg(2, r);
    }
    void Encoder::shl_cl(const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(w == Width::W64, 0, r);
        byte(0xD3);
        modrm_reg(4, r);
    }
    void Encoder::shr_cl(const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(w == Width::W64, 0, r);
        byte(0xD3);
        modrm_reg(5, r);
    }
    void Encoder::sar_cl(const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(w == Width::W64, 0, r);
        byte(0xD3);
        modrm_reg(7, r);
    }

    void Encoder::test_rr(const Width w, const Reg a, const Reg b) {
        const auto ra = static_cast<uint8_t>(a);
        const auto rb = static_cast<uint8_t>(b);
        rex(w == Width::W64, rb, ra,
             w == Width::W8 && (needs_byte_rex(ra) || needs_byte_rex(rb)));
        byte(w == Width::W8 ? 0x84 : 0x85);
        modrm_reg(rb, ra);
    }

    void Encoder::setcc(const Cond cond, const Reg dst8) {
        const auto d = static_cast<uint8_t>(dst8);
        rex(false, 0, d, needs_byte_rex(d));
        byte(0x0F);
        byte(static_cast<uint8_t>(0x90 | static_cast<uint8_t>(cond)));
        modrm_reg(0, d);
    }

    // --- SSE ----------------------------------------------------------------------

    void Encoder::movss_load(const XReg dst, const Reg base, const int32_t disp) {
        byte(0xF3);
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
        byte(0x0F); byte(0x10);
        modrm_mem(static_cast<uint8_t>(dst), base, disp);
    }
    void Encoder::movss_store(const Reg base, const int32_t disp, const XReg src) {
        byte(0xF3);
        rex(false, static_cast<uint8_t>(src), static_cast<uint8_t>(base));
        byte(0x0F); byte(0x11);
        modrm_mem(static_cast<uint8_t>(src), base, disp);
    }
    void Encoder::movsd_load(const XReg dst, const Reg base, const int32_t disp) {
        byte(0xF2);
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
        byte(0x0F); byte(0x10);
        modrm_mem(static_cast<uint8_t>(dst), base, disp);
    }
    void Encoder::movsd_store(const Reg base, const int32_t disp, const XReg src) {
        byte(0xF2);
        rex(false, static_cast<uint8_t>(src), static_cast<uint8_t>(base));
        byte(0x0F); byte(0x11);
        modrm_mem(static_cast<uint8_t>(src), base, disp);
    }
    void Encoder::sse_arith(const uint8_t opcode, const bool is_double, const XReg dst, const XReg src) {
        byte(is_double ? 0xF2 : 0xF3);
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(opcode);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::movaps(const XReg dst, const XReg src) {
        // 0F 28 /r moves all 128 bits, which is a superset of the scalar copy the
        // allocator needs; no 66/F3/F2 prefix.
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(0x28);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::ucomis(const bool is_double, const XReg a, const XReg b) {
        if (is_double) byte(0x66);
        rex(false, static_cast<uint8_t>(a), static_cast<uint8_t>(b));
        byte(0x0F); byte(0x2E);
        modrm_reg(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
    }
    void Encoder::cvt_i2f(const bool to_double, const Width from, const XReg dst, const Reg src) {
        byte(to_double ? 0xF2 : 0xF3);
        rex(from == Width::W64, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(0x2A);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::cvt_f2i(const bool from_double, const Width to, const Reg dst, const XReg src) {
        byte(from_double ? 0xF2 : 0xF3);
        rex(to == Width::W64, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(0x2C);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::cvt_f2f(const bool to_double, const XReg dst, const XReg src) {
        byte(to_double ? 0xF3 : 0xF2); // cvtss2sd is F3, cvtsd2ss is F2
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(0x5A);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::mov_r_x(const XReg dst, const Reg src) {
        byte(0x66);
        rex(true, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
        byte(0x0F); byte(0x6E);
        modrm_reg(static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
    }
    void Encoder::mov_x_r(const Reg dst, const XReg src) {
        byte(0x66);
        rex(true, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
        byte(0x0F); byte(0x7E);
        modrm_reg(static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
    }

    // --- control flow -------------------------------------------------------------

    void Encoder::jmp(const Label target) {
        byte(0xE9);
        pending_.push_back({.patch_offset = static_cast<uint32_t>(code.size()), .target = target});
        imm32(0);
    }
    void Encoder::jcc(const Cond cond, const Label target) {
        byte(0x0F);
        byte(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(cond)));
        pending_.push_back({.patch_offset = static_cast<uint32_t>(code.size()), .target = target});
        imm32(0);
    }
    void Encoder::call_sym(const uint32_t symbol) {
        byte(0xE8);
        relocations.push_back({.kind = Relocation::Kind::Call32,
                                .offset = static_cast<uint32_t>(code.size()),
                                .symbol = symbol, .addend = -4});
        imm32(0);
    }
    void Encoder::call_r(const Reg target) {
        const auto t = static_cast<uint8_t>(target);
        rex(false, 0, t);
        byte(0xFF);
        modrm_reg(2, t);
    }
    void Encoder::ret() { byte(0xC3); }
    void Encoder::push_r(const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(false, 0, r);
        byte(static_cast<uint8_t>(0x50 | low3(r)));
    }
    void Encoder::pop_r(const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        rex(false, 0, r);
        byte(static_cast<uint8_t>(0x58 | low3(r)));
    }
    void Encoder::sub_rsp(const int32_t bytes) { alu_ri(Alu::Sub, Width::W64, Reg::RSP, bytes); }
    void Encoder::add_rsp(const int32_t bytes) { alu_ri(Alu::Add, Width::W64, Reg::RSP, bytes); }
    void Encoder::ud2() { byte(0x0F); byte(0x0B); }

    // --- inline-asm support -----------------------------------------------------

    void Encoder::nop() { byte(0x90); }
    void Encoder::syscall() { byte(0x0F); byte(0x05); }
    void Encoder::cpuid() { byte(0x0F); byte(0xA2); }

    void Encoder::alu_rm(const Alu op, const Width w, const Reg dst, const Reg base, const int32_t disp) {
        const auto d = static_cast<uint8_t>(dst);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, d, static_cast<uint8_t>(base), w == Width::W8 && needs_byte_rex(d));
        // The 0x03-family (reg <- reg op mem) is the r/m-as-source direction.
        byte(static_cast<uint8_t>(static_cast<uint8_t>(op) * 8 | (w == Width::W8 ? 0x02 : 0x03)));
        modrm_mem(d, base, disp);
    }

    void Encoder::alu_mr(const Alu op, const Width w, const Reg base, const int32_t disp, const Reg src) {
        const auto s = static_cast<uint8_t>(src);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, s, static_cast<uint8_t>(base), w == Width::W8 && needs_byte_rex(s));
        byte(static_cast<uint8_t>(static_cast<uint8_t>(op) * 8 | (w == Width::W8 ? 0x00 : 0x01)));
        modrm_mem(s, base, disp);
    }

    void Encoder::alu_mi(const Alu op, const Width w, const Reg base, const int32_t disp, const int32_t imm) {
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, 0, static_cast<uint8_t>(base));
        byte(w == Width::W8 ? 0x80 : 0x81);
        modrm_mem(static_cast<uint8_t>(op), base, disp);
        if (w == Width::W8) byte(static_cast<uint8_t>(imm));
        else if (w == Width::W16) { byte(static_cast<uint8_t>(imm)); byte(static_cast<uint8_t>(imm >> 8)); }
        else imm32(imm);
    }

    void Encoder::mov_mi(const Width w, const Reg base, const int32_t disp, const int32_t imm) {
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, 0, static_cast<uint8_t>(base));
        byte(w == Width::W8 ? 0xC6 : 0xC7);
        modrm_mem(0, base, disp);
        if (w == Width::W8) byte(static_cast<uint8_t>(imm));
        else if (w == Width::W16) { byte(static_cast<uint8_t>(imm)); byte(static_cast<uint8_t>(imm >> 8)); }
        else imm32(imm);
    }

    void Encoder::movzx_m(const Width from, const Reg dst, const Reg base, const int32_t disp) {
        const auto d = static_cast<uint8_t>(dst);
        if (from == Width::W32 || from == Width::W64) {
            // No movzx from 32: a plain 32-bit load already zero-extends.
            rex(false, d, static_cast<uint8_t>(base));
            byte(0x8B);
            modrm_mem(d, base, disp);
            return;
        }
        rex(true, d, static_cast<uint8_t>(base));
        byte(0x0F);
        byte(from == Width::W8 ? 0xB6 : 0xB7);
        modrm_mem(d, base, disp);
    }

    void Encoder::unary_m(const uint8_t slot, const Width w, const Reg base, const int32_t disp) {
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, 0, static_cast<uint8_t>(base));
        // inc/dec (slots 0/1) live on 0xFE/0xFF; not/neg (2/3) on 0xF6/0xF7.
        byte(slot <= 1 ? (w == Width::W8 ? 0xFE : 0xFF) : (w == Width::W8 ? 0xF6 : 0xF7));
        modrm_mem(slot <= 1 ? slot : static_cast<uint8_t>(slot), base, disp);
    }

    void Encoder::unary_r(const uint8_t slot, const Width w, const Reg reg) {
        const auto r = static_cast<uint8_t>(reg);
        if (w == Width::W16) byte(0x66);
        rex(w == Width::W64, 0, r, w == Width::W8 && needs_byte_rex(r));
        byte(slot <= 1 ? (w == Width::W8 ? 0xFE : 0xFF) : (w == Width::W8 ? 0xF6 : 0xF7));
        modrm_reg(slot, r);
    }

    // --- spilled-operand folding (stage 6) --------------------------------------

    void Encoder::div_m(const Reg base, const int32_t disp) {
        rex(true, 0, static_cast<uint8_t>(base));
        byte(0xF7);
        modrm_mem(6, base, disp);
    }
    void Encoder::idiv_m(const Reg base, const int32_t disp) {
        rex(true, 0, static_cast<uint8_t>(base));
        byte(0xF7);
        modrm_mem(7, base, disp);
    }
    void Encoder::imul_rm(const Reg dst, const Reg base, const int32_t disp) {
        const auto d = static_cast<uint8_t>(dst);
        rex(true, d, static_cast<uint8_t>(base));
        byte(0x0F);
        byte(0xAF);
        modrm_mem(d, base, disp);
    }
    void Encoder::sse_arith_m(const uint8_t opcode, const bool is_double, const XReg dst,
                               const Reg base, const int32_t disp) {
        byte(is_double ? 0xF2 : 0xF3);
        rex(false, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
        byte(0x0F); byte(opcode);
        modrm_mem(static_cast<uint8_t>(dst), base, disp);
    }
    void Encoder::ucomis_m(const bool is_double, const XReg a, const Reg base, const int32_t disp) {
        if (is_double) byte(0x66);
        rex(false, static_cast<uint8_t>(a), static_cast<uint8_t>(base));
        byte(0x0F); byte(0x2E);
        modrm_mem(static_cast<uint8_t>(a), base, disp);
    }

    void Encoder::push_m(const Reg base, const int32_t disp) {
        rex(false, 0, static_cast<uint8_t>(base));
        byte(0xFF);
        modrm_mem(6, base, disp);
    }
    void Encoder::pop_m(const Reg base, const int32_t disp) {
        rex(false, 0, static_cast<uint8_t>(base));
        byte(0x8F);
        modrm_mem(0, base, disp);
    }
}
