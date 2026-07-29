#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Static x86_64 general-purpose register data, shared by asm_lexer (tokenizing register names),
// sema_check (clobber-set construction, width-mismatch suggestions), and codegen (clobber
// constraint-string emission). Kept as a single dependency-free header so all three can include
// it without pulling in LLVM or any compiler-internal headers.
namespace asm_registers {
    struct RegisterInfo {
        std::string_view name;   // e.g. "eax" — always lowercase
        std::string_view family; // 64-bit root register, e.g. "rax"
        uint32_t width_bits;     // 8, 16, 32, or 64
    };

    // 16 registers per width tier x 4 tiers = 64 entries.
    inline constexpr std::array<RegisterInfo, 64> registers = {{
        // 64-bit
        {"rax", "rax", 64}, {"rbx", "rbx", 64}, {"rcx", "rcx", 64}, {"rdx", "rdx", 64},
        {"rdi", "rdi", 64}, {"rsi", "rsi", 64}, {"rsp", "rsp", 64}, {"rbp", "rbp", 64},
        {"r8", "r8", 64},   {"r9", "r9", 64},   {"r10", "r10", 64}, {"r11", "r11", 64},
        {"r12", "r12", 64}, {"r13", "r13", 64}, {"r14", "r14", 64}, {"r15", "r15", 64},

        // 32-bit
        {"eax", "rax", 32}, {"ebx", "rbx", 32}, {"ecx", "rcx", 32}, {"edx", "rdx", 32},
        {"edi", "rdi", 32}, {"esi", "rsi", 32}, {"esp", "rsp", 32}, {"ebp", "rbp", 32},
        {"r8d", "r8", 32},  {"r9d", "r9", 32},  {"r10d", "r10", 32}, {"r11d", "r11", 32},
        {"r12d", "r12", 32}, {"r13d", "r13", 32}, {"r14d", "r14", 32}, {"r15d", "r15", 32},

        // 16-bit
        {"ax", "rax", 16}, {"bx", "rbx", 16}, {"cx", "rcx", 16}, {"dx", "rdx", 16},
        {"di", "rdi", 16}, {"si", "rsi", 16}, {"sp", "rsp", 16}, {"bp", "rbp", 16},
        {"r8w", "r8", 16}, {"r9w", "r9", 16}, {"r10w", "r10", 16}, {"r11w", "r11", 16},
        {"r12w", "r12", 16}, {"r13w", "r13", 16}, {"r14w", "r14", 16}, {"r15w", "r15", 16},

        // 8-bit
        {"al", "rax", 8}, {"bl", "rbx", 8}, {"cl", "rcx", 8}, {"dl", "rdx", 8},
        {"dil", "rdi", 8}, {"sil", "rsi", 8}, {"spl", "rsp", 8}, {"bpl", "rbp", 8},
        {"r8b", "r8", 8}, {"r9b", "r9", 8}, {"r10b", "r10", 8}, {"r11b", "r11", 8},
        {"r12b", "r12", 8}, {"r13b", "r13", 8}, {"r14b", "r14", 8}, {"r15b", "r15", 8},
    }};

    // Registers/register-like names that are recognized as real x86 features but are explicitly
    // out of scope for v1 inline asm (see spec.md's "Inline Assembly" section) — kept distinct
    // from an ordinary unrecognized word so callers can emit a precise "not supported in v1"
    // diagnostic instead of a misleading "unknown identifier" error.
    inline constexpr std::array<std::string_view, 93> unsupported_registers = {{
        // Legacy 8-bit high-byte registers. Listed as unsupported rather than added to
        // 'registers' above: they are not simply another width tier of their family. They
        // alias bits 8-15 rather than 0-7, cannot be encoded in an instruction that also
        // uses a REX prefix (so they are mutually exclusive with dil/sil/spl/bpl and r8-r15),
        // and would need LLVM's separate 'q'/'Q' constraint classes to express. Naming them
        // here turns 'asm { mov ah, 1 }' into a precise "not supported in v1" diagnostic
        // instead of a misleading "undefined variable 'ah'".
        "ah", "bh", "ch", "dh",
        // SSE/AVX
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
        "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
        "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15",
        "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7",
        "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15",
        "zmm16", "zmm17", "zmm18", "zmm19", "zmm20", "zmm21", "zmm22", "zmm23",
        "zmm24", "zmm25", "zmm26", "zmm27", "zmm28", "zmm29", "zmm30", "zmm31",
        // x87 FPU
        "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7",
        // Segment registers
        "cs", "ds", "ss", "es", "fs", "gs",
        // Control/debug registers (a representative subset — cr0-4/8, dr0-3/6-7 are the ones that
        // actually exist on x86_64; the full cr/dr numeric range is not architecturally valid, so
        // enumerating cr0-15/dr0-15 verbatim would falsely legitimize non-existent registers).
        "cr0", "cr2", "cr3", "cr4", "cr8",
        "dr0", "dr1", "dr2", "dr3", "dr6", "dr7",
    }};

    [[nodiscard]] constexpr auto lookup_register(const std::string_view name) -> const RegisterInfo * {
        for (const auto &reg : registers) {
            if (reg.name == name) {
                return &reg;
            }
        }
        return nullptr;
    }

    [[nodiscard]] constexpr auto is_unsupported_register(const std::string_view name) -> bool {
        for (const auto &unsupported : unsupported_registers) {
            if (unsupported == name) {
                return true;
            }
        }
        return false;
    }

    // Finds the register in the given family (e.g. "rax") at the given width (e.g. 32), used to
    // suggest a fix for a width-mismatched operand (e.g. "rax" + i32 variable -> suggest "eax").
    [[nodiscard]] constexpr auto lookup_register_by_family_and_width(const std::string_view family, const uint32_t width) -> const RegisterInfo * {
        for (const auto &reg : registers) {
            if (reg.family == family && reg.width_bits == width) {
                return &reg;
            }
        }
        return nullptr;
    }
}
