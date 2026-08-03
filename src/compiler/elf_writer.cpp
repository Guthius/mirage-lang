#include "elf_writer.hpp"

// See the header for the produced layout. Offsets are computed in one forward walk;
// every multi-byte field is little-endian, matching the only target this backend
// has (x86-64 Linux).

#include <algorithm>

namespace elf {
    namespace {
        constexpr uint32_t SHT_PROGBITS = 1;
        constexpr uint32_t SHT_SYMTAB = 2;
        constexpr uint32_t SHT_STRTAB = 3;
        constexpr uint32_t SHT_RELA = 4;
        constexpr uint32_t SHT_NOBITS = 8;
        constexpr uint64_t SHF_WRITE = 0x1;
        constexpr uint64_t SHF_ALLOC = 0x2;
        constexpr uint64_t SHF_EXECINSTR = 0x4;

        // Section header indices, matching the fixed emission order.
        enum : uint16_t {
            SH_NULL = 0, SH_TEXT, SH_RELA_TEXT, SH_DATA, SH_RELA_DATA,
            SH_RODATA, SH_RELA_RODATA, SH_BSS, SH_SYMTAB, SH_STRTAB, SH_SHSTRTAB,
            SH_COUNT,
        };

        auto section_index(const Section s) -> uint16_t {
            switch (s) {
            case Section::Text:   return SH_TEXT;
            case Section::Data:   return SH_DATA;
            case Section::Rodata: return SH_RODATA;
            case Section::Bss:    return SH_BSS;
            case Section::Undefined: return 0;
            }
            return 0;
        }

        struct Buffer {
            std::vector<uint8_t> bytes;
            void u8(uint8_t v) { bytes.push_back(v); }
            void u16(uint16_t v) { for (int i = 0; i < 2; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
            void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
            void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
            void raw(const std::vector<uint8_t> &data) { bytes.insert(bytes.end(), data.begin(), data.end()); }
            void pad_to(const uint64_t align) {
                while (align > 1 && bytes.size() % align != 0) u8(0);
            }
        };

        struct StringTable {
            std::vector<uint8_t> bytes{0}; // index 0 is the empty string
            auto add(const std::string &s) -> uint32_t {
                const auto at = static_cast<uint32_t>(bytes.size());
                bytes.insert(bytes.end(), s.begin(), s.end());
                bytes.push_back(0);
                return at;
            }
        };
    }

    auto write(const Object &object) -> std::vector<uint8_t> {
        // ---- symbols: locals first, remembering the remap --------------------
        std::vector<uint32_t> order(object.symbols.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::stable_sort(order, [&](const uint32_t a, const uint32_t b) {
            return !object.symbols[a].is_global && object.symbols[b].is_global;
        });
        std::vector<uint32_t> remap(object.symbols.size());
        for (uint32_t i = 0; i < order.size(); ++i) remap[order[i]] = i + 1; // +1: null entry
        uint32_t first_global = static_cast<uint32_t>(order.size()) + 1;
        for (uint32_t i = 0; i < order.size(); ++i) {
            if (object.symbols[order[i]].is_global) { first_global = i + 1; break; }
        }

        StringTable strtab;
        Buffer symtab;
        // Null entry.
        for (int i = 0; i < 24; ++i) symtab.u8(0);
        for (const auto index : order) {
            const auto &sym = object.symbols[index];
            symtab.u32(strtab.add(sym.name));
            const uint8_t bind = sym.is_global ? 1 : 0; // GLOBAL : LOCAL
            const uint8_t type = sym.section == Section::Undefined ? 0
                                : sym.is_function ? 2 : 1;           // NOTYPE/FUNC/OBJECT
            symtab.u8(static_cast<uint8_t>((bind << 4) | type));
            symtab.u8(0); // st_other: default visibility
            symtab.u16(section_index(sym.section));
            symtab.u64(sym.value);
            symtab.u64(sym.size);
        }

        // ---- relocation sections ---------------------------------------------
        const auto build_rela = [&](const Section in) {
            Buffer rela;
            for (const auto &rel : object.relocations) {
                if (rel.in != in) continue;
                rela.u64(rel.offset);
                rela.u64((static_cast<uint64_t>(remap[rel.symbol]) << 32) | rel.type);
                rela.u64(static_cast<uint64_t>(rel.addend));
            }
            return rela;
        };
        const auto rela_text = build_rela(Section::Text);
        const auto rela_data = build_rela(Section::Data);
        const auto rela_rodata = build_rela(Section::Rodata);

        // ---- section name table ----------------------------------------------
        StringTable shstrtab;
        const auto name_text = shstrtab.add(".text");
        const auto name_rela_text = shstrtab.add(".rela.text");
        const auto name_data = shstrtab.add(".data");
        const auto name_rela_data = shstrtab.add(".rela.data");
        const auto name_rodata = shstrtab.add(".rodata");
        const auto name_rela_rodata = shstrtab.add(".rela.rodata");
        const auto name_bss = shstrtab.add(".bss");
        const auto name_symtab = shstrtab.add(".symtab");
        const auto name_strtab = shstrtab.add(".strtab");
        const auto name_shstrtab = shstrtab.add(".shstrtab");

        // ---- assemble the file -----------------------------------------------
        Buffer out;
        // Ehdr (64 bytes), with the section-header offset patched at the end.
        out.raw({0x7f, 'E', 'L', 'F', 2 /*64-bit*/, 1 /*LE*/, 1 /*version*/, 0});
        for (int i = 0; i < 8; ++i) out.u8(0);
        out.u16(1);        // ET_REL
        out.u16(0x3E);     // EM_X86_64
        out.u32(1);        // EV_CURRENT
        out.u64(0);        // e_entry
        out.u64(0);        // e_phoff
        const auto shoff_field = out.bytes.size();
        out.u64(0);        // e_shoff (patched below)
        out.u32(0);        // e_flags
        out.u16(64);       // e_ehsize
        out.u16(0);        // e_phentsize
        out.u16(0);        // e_phnum
        out.u16(64);       // e_shentsize
        out.u16(SH_COUNT); // e_shnum
        out.u16(SH_SHSTRTAB);

        struct Placed { uint64_t offset; uint64_t size; };
        const auto place = [&](const std::vector<uint8_t> &content, const uint64_t align) {
            out.pad_to(align);
            const Placed placed{out.bytes.size(), content.size()};
            out.raw(content);
            return placed;
        };
        const auto text = place(object.text, object.text_align);
        const auto rt = place(rela_text.bytes, 8);
        const auto data = place(object.data, object.data_align);
        const auto rd = place(rela_data.bytes, 8);
        const auto rodata = place(object.rodata, object.rodata_align);
        const auto rr = place(rela_rodata.bytes, 8);
        out.pad_to(object.bss_align);
        const Placed bss{out.bytes.size(), object.bss_size}; // NOBITS: no content
        const auto symtab_placed = place(symtab.bytes, 8);
        const auto strtab_placed = place(strtab.bytes, 1);
        const auto shstrtab_placed = place(shstrtab.bytes, 1);

        out.pad_to(8);
        const auto shoff = out.bytes.size();
        const auto shdr = [&](const uint32_t name, const uint32_t type, const uint64_t flags,
                               const Placed &placed, const uint32_t link, const uint32_t info,
                               const uint64_t align, const uint64_t entsize) {
            out.u32(name);
            out.u32(type);
            out.u64(flags);
            out.u64(0); // sh_addr
            out.u64(placed.offset);
            out.u64(placed.size);
            out.u32(link);
            out.u32(info);
            out.u64(align);
            out.u64(entsize);
        };
        // NULL
        shdr(0, 0, 0, {0, 0}, 0, 0, 0, 0);
        shdr(name_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text, 0, 0, object.text_align, 0);
        shdr(name_rela_text, SHT_RELA, 0, rt, SH_SYMTAB, SH_TEXT, 8, 24);
        shdr(name_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, data, 0, 0, object.data_align, 0);
        shdr(name_rela_data, SHT_RELA, 0, rd, SH_SYMTAB, SH_DATA, 8, 24);
        shdr(name_rodata, SHT_PROGBITS, SHF_ALLOC, rodata, 0, 0, object.rodata_align, 0);
        shdr(name_rela_rodata, SHT_RELA, 0, rr, SH_SYMTAB, SH_RODATA, 8, 24);
        shdr(name_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss, 0, 0, object.bss_align, 0);
        shdr(name_symtab, SHT_SYMTAB, 0, symtab_placed, SH_STRTAB, first_global, 8, 24);
        shdr(name_strtab, SHT_STRTAB, 0, strtab_placed, 0, 0, 1, 0);
        shdr(name_shstrtab, SHT_STRTAB, 0, shstrtab_placed, 0, 0, 1, 0);

        // Patch e_shoff.
        for (int i = 0; i < 8; ++i) {
            out.bytes[shoff_field + i] = static_cast<uint8_t>(shoff >> (8 * i));
        }
        return out.bytes;
    }
}
