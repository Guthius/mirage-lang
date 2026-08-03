#pragma once

// ELF64 relocatable-object writer (stage 4, docs/backend.md). Takes flat section
// contents, a symbol list and relocations, and produces the bytes of a `.o` any
// System V linker accepts. Nothing here knows about MIR or x86 — the backend maps
// its own structures onto these.
//
// Layout produced: Ehdr, section contents (padded to alignment), then the section
// header table. Sections, in fixed order: NULL, .text, .rela.text, .data,
// .rela.data, .rodata, .rela.rodata, .bss, .symtab, .strtab, .shstrtab. Symbols are
// emitted locals-first (sh_info of .symtab is the first global's index, as the spec
// requires); callers reference symbols by THEIR index into 'symbols', and the writer
// remaps to the reordered table.

#include <cstdint>
#include <string>
#include <vector>

namespace elf {
    enum class Section : uint8_t { Undefined, Text, Data, Rodata, Bss };

    struct Symbol {
        std::string name;
        Section section = Section::Undefined; // Undefined = external reference
        uint64_t value = 0;                    // offset within its section
        uint64_t size = 0;
        bool is_global = false;                // STB_GLOBAL vs STB_LOCAL
        bool is_function = false;              // STT_FUNC vs STT_OBJECT
    };

    struct Relocation {
        Section in = Section::Text;  // which section's bytes get patched
        uint64_t offset = 0;
        uint32_t symbol = 0;         // index into the caller's 'symbols' vector
        uint32_t type = 0;           // R_X86_64_* value, caller-chosen
        int64_t addend = 0;
    };

    // R_X86_64 relocation types actually used by this backend.
    inline constexpr uint32_t R_X86_64_64 = 1;    // absolute 8-byte (data)
    inline constexpr uint32_t R_X86_64_PC32 = 2;  // RIP-relative 4-byte
    inline constexpr uint32_t R_X86_64_PLT32 = 4; // call rel32

    struct Object {
        std::vector<uint8_t> text;
        std::vector<uint8_t> data;
        std::vector<uint8_t> rodata;
        uint64_t bss_size = 0;
        uint64_t text_align = 16;
        uint64_t data_align = 8;
        uint64_t rodata_align = 8;
        uint64_t bss_align = 8;
        std::vector<Symbol> symbols;
        std::vector<Relocation> relocations;
    };

    [[nodiscard]] auto write(const Object &object) -> std::vector<uint8_t>;
}
