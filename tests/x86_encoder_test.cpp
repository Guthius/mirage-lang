// Byte-exact tests for the x86-64 encoder (docs/backend.md validation #3: the class
// of bug an encoder produces is working-but-wrong code, and byte comparison is what
// catches it). Every expected sequence was cross-checked against GNU 'as' output for
// the same instruction; where 'as' picks a shorter form (disp8, imm8) the expectation
// was re-derived by hand for this encoder's deliberate always-disp32/imm32 choice —
// mod bits and field widths are the only difference.

#include "compiler/x86_encoder.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {
    int failures = 0;

    auto hex(const std::vector<uint8_t> &bytes) -> std::string {
        std::string out;
        char buf[4];
        for (const auto b : bytes) {
            std::snprintf(buf, sizeof buf, "%02x ", b);
            out += buf;
        }
        return out;
    }

    void expect(const char *what, const std::vector<uint8_t> &actual,
                const std::vector<uint8_t> &wanted) {
        if (actual == wanted) {
            std::printf("ok: %s\n", what);
        } else {
            ++failures;
            std::printf("FAIL: %s\n  wanted: %s\n  got:    %s\n",
                        what, hex(wanted).c_str(), hex(actual).c_str());
        }
    }

    template <typename F>
    void check(const char *what, F &&emit, const std::vector<uint8_t> &wanted) {
        x86::Encoder e;
        emit(e);
        e.resolve_labels();
        expect(what, e.code, wanted);
    }
}

int main() {
    using x86::Reg;
    using x86::XReg;
    using x86::Width;
    using x86::Alu;
    using x86::Cond;

    check("mov eax, 42", [](x86::Encoder &e) { e.mov_ri(Reg::RAX, 42); },
          {0xb8, 0x2a, 0x00, 0x00, 0x00});
    check("movabs r10, 0x0123456789abcdef",
          [](x86::Encoder &e) { e.mov_ri(Reg::R10, 0x0123456789abcdefLL); },
          {0x49, 0xba, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
    check("mov r9, rcx", [](x86::Encoder &e) { e.mov_rr(Width::W64, Reg::R9, Reg::RCX); },
          {0x49, 0x89, 0xc9});
    check("mov dl, al", [](x86::Encoder &e) { e.mov_rr(Width::W8, Reg::RDX, Reg::RAX); },
          {0x88, 0xc2});
    check("mov rax, [rbp+0x10]", [](x86::Encoder &e) { e.load(Width::W64, Reg::RAX, Reg::RBP, 0x10); },
          {0x48, 0x8b, 0x85, 0x10, 0x00, 0x00, 0x00});
    check("mov ecx, [rsp+8]", [](x86::Encoder &e) { e.load(Width::W32, Reg::RCX, Reg::RSP, 8); },
          {0x8b, 0x8c, 0x24, 0x08, 0x00, 0x00, 0x00});
    check("mov [rbp+0x20], rax", [](x86::Encoder &e) { e.store(Width::W64, Reg::RBP, 0x20, Reg::RAX); },
          {0x48, 0x89, 0x85, 0x20, 0x00, 0x00, 0x00});
    check("mov [rax+1], dil", [](x86::Encoder &e) { e.store(Width::W8, Reg::RAX, 1, Reg::RDI); },
          {0x40, 0x88, 0xb8, 0x01, 0x00, 0x00, 0x00});
    check("movzx rax, cl", [](x86::Encoder &e) { e.movzx(Width::W8, Reg::RAX, Reg::RCX); },
          {0x48, 0x0f, 0xb6, 0xc1});
    check("movsx rdx, r9w", [](x86::Encoder &e) { e.movsx(Width::W16, Reg::RDX, Reg::R9); },
          {0x49, 0x0f, 0xbf, 0xd1});
    check("movsxd rsi, edi", [](x86::Encoder &e) { e.movsx(Width::W32, Reg::RSI, Reg::RDI); },
          {0x48, 0x63, 0xf7});
    check("mov esi, edi (zext32)", [](x86::Encoder &e) { e.movzx(Width::W32, Reg::RSI, Reg::RDI); },
          {0x89, 0xfe});
    check("lea rdi, [rbp+0x18]", [](x86::Encoder &e) { e.lea(Reg::RDI, Reg::RBP, 0x18); },
          {0x48, 0x8d, 0xbd, 0x18, 0x00, 0x00, 0x00});
    check("add rax, rdx", [](x86::Encoder &e) { e.alu_rr(Alu::Add, Width::W64, Reg::RAX, Reg::RDX); },
          {0x48, 0x01, 0xd0});
    check("sub rsp, 0x30", [](x86::Encoder &e) { e.sub_rsp(0x30); },
          {0x48, 0x81, 0xec, 0x30, 0x00, 0x00, 0x00});
    check("cmp ecx, r8d", [](x86::Encoder &e) { e.alu_rr(Alu::Cmp, Width::W32, Reg::RCX, Reg::R8); },
          {0x44, 0x39, 0xc1});
    check("imul rax, rbx", [](x86::Encoder &e) { e.imul_rr(Reg::RAX, Reg::RBX); },
          {0x48, 0x0f, 0xaf, 0xc3});
    check("cqo", [](x86::Encoder &e) { e.cqo(); }, {0x48, 0x99});
    check("xor edx, edx", [](x86::Encoder &e) { e.zero(Reg::RDX); }, {0x31, 0xd2});
    check("div rcx", [](x86::Encoder &e) { e.div_r(Reg::RCX); }, {0x48, 0xf7, 0xf1});
    check("idiv r11", [](x86::Encoder &e) { e.idiv_r(Reg::R11); }, {0x49, 0xf7, 0xfb});
    check("neg rax", [](x86::Encoder &e) { e.neg_r(Width::W64, Reg::RAX); }, {0x48, 0xf7, 0xd8});
    check("not ecx", [](x86::Encoder &e) { e.not_r(Width::W32, Reg::RCX); }, {0xf7, 0xd1});
    check("shl rax, cl", [](x86::Encoder &e) { e.shl_cl(Width::W64, Reg::RAX); }, {0x48, 0xd3, 0xe0});
    check("sar r9, cl", [](x86::Encoder &e) { e.sar_cl(Width::W64, Reg::R9); }, {0x49, 0xd3, 0xf9});
    check("test rax, rax", [](x86::Encoder &e) { e.test_rr(Width::W64, Reg::RAX, Reg::RAX); },
          {0x48, 0x85, 0xc0});
    check("sete al", [](x86::Encoder &e) { e.setcc(Cond::E, Reg::RAX); }, {0x0f, 0x94, 0xc0});
    check("setl sil", [](x86::Encoder &e) { e.setcc(Cond::L, Reg::RSI); },
          {0x40, 0x0f, 0x9c, 0xc6});
    check("movss xmm1, [rbp+4]", [](x86::Encoder &e) { e.movss_load(XReg::XMM1, Reg::RBP, 4); },
          {0xf3, 0x0f, 0x10, 0x8d, 0x04, 0x00, 0x00, 0x00});
    check("movsd [rsp+8], xmm0", [](x86::Encoder &e) { e.movsd_store(Reg::RSP, 8, XReg::XMM0); },
          {0xf2, 0x0f, 0x11, 0x84, 0x24, 0x08, 0x00, 0x00, 0x00});
    check("addsd xmm0, xmm1", [](x86::Encoder &e) { e.sse_arith(0x58, true, XReg::XMM0, XReg::XMM1); },
          {0xf2, 0x0f, 0x58, 0xc1});
    check("ucomiss xmm0, xmm1", [](x86::Encoder &e) { e.ucomis(false, XReg::XMM0, XReg::XMM1); },
          {0x0f, 0x2e, 0xc1});
    check("cvtsi2sd xmm0, rax", [](x86::Encoder &e) { e.cvt_i2f(true, Width::W64, XReg::XMM0, Reg::RAX); },
          {0xf2, 0x48, 0x0f, 0x2a, 0xc0});
    check("cvttsd2si rax, xmm1", [](x86::Encoder &e) { e.cvt_f2i(true, Width::W64, Reg::RAX, XReg::XMM1); },
          {0xf2, 0x48, 0x0f, 0x2c, 0xc1});
    check("cvtss2sd xmm0, xmm1", [](x86::Encoder &e) { e.cvt_f2f(true, XReg::XMM0, XReg::XMM1); },
          {0xf3, 0x0f, 0x5a, 0xc1});
    check("movq xmm0, rax", [](x86::Encoder &e) { e.mov_r_x(XReg::XMM0, Reg::RAX); },
          {0x66, 0x48, 0x0f, 0x6e, 0xc0});
    check("movq rax, xmm0", [](x86::Encoder &e) { e.mov_x_r(Reg::RAX, XReg::XMM0); },
          {0x66, 0x48, 0x0f, 0x7e, 0xc0});
    check("call rax", [](x86::Encoder &e) { e.call_r(Reg::RAX); }, {0xff, 0xd0});
    check("ret", [](x86::Encoder &e) { e.ret(); }, {0xc3});
    check("push rbp / pop r15", [](x86::Encoder &e) { e.push_r(Reg::RBP); e.pop_r(Reg::R15); },
          {0x55, 0x41, 0x5f});
    check("ud2", [](x86::Encoder &e) { e.ud2(); }, {0x0f, 0x0b});

    // A backward and a forward jump, resolved through labels: jmp rel32 counts from
    // the END of the 5-byte instruction.
    check("label round-trip",
          [](x86::Encoder &e) {
              const auto top = e.make_label();
              const auto out = e.make_label();
              e.bind(top);
              e.jcc(Cond::E, out);   // forward: 6 bytes, rel = +5 (over the jmp)
              e.jmp(top);            // backward: 5 bytes, rel = -11
              e.bind(out);
          },
          {0x0f, 0x84, 0x05, 0x00, 0x00, 0x00,
           0xe9, 0xf5, 0xff, 0xff, 0xff});

    // Relocation bookkeeping: a call and a RIP-relative lea leave 4-byte holes with
    // the right kinds, offsets and addends.
    {
        x86::Encoder e;
        e.call_sym(7);
        e.lea_rip(Reg::RDI, 3, 16);
        expect("call+lea bytes", e.code,
               {0xe8, 0x00, 0x00, 0x00, 0x00,
                0x48, 0x8d, 0x3d, 0x00, 0x00, 0x00, 0x00});
        const bool relocs_ok =
            e.relocations.size() == 2 &&
            e.relocations[0].kind == x86::Relocation::Kind::Call32 &&
            e.relocations[0].offset == 1 && e.relocations[0].symbol == 7 &&
            e.relocations[0].addend == -4 &&
            e.relocations[1].kind == x86::Relocation::Kind::Rip32 &&
            e.relocations[1].offset == 8 && e.relocations[1].symbol == 3 &&
            e.relocations[1].addend == 16;
        if (relocs_ok) std::printf("ok: relocation records\n");
        else { ++failures; std::printf("FAIL: relocation records\n"); }
    }

    if (failures != 0) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("\nall x86 encoder tests passed\n");
    return 0;
}
