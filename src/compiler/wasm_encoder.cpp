#include "wasm_encoder.hpp"

// Binary format per the WebAssembly core spec: sections in ascending id order,
// LEB128 integers, and function bodies with run-length-compressed local
// declarations. tests/wasm_encoder_test.cpp pins bytes; the module-level check is
// node's own validator in the differential harness (WebAssembly.Module rejects a
// malformed binary outright, so every harness run is also a format audit).

namespace wasm {
    namespace {
        void uleb(std::vector<uint8_t> &out, uint64_t value) {
            do {
                auto byte = static_cast<uint8_t>(value & 0x7F);
                value >>= 7;
                if (value != 0) byte |= 0x80;
                out.push_back(byte);
            } while (value != 0);
        }

        void sleb(std::vector<uint8_t> &out, int64_t value) {
            bool more = true;
            while (more) {
                auto byte = static_cast<uint8_t>(value & 0x7F);
                value >>= 7;
                if ((value == 0 && (byte & 0x40) == 0) || (value == -1 && (byte & 0x40) != 0)) {
                    more = false;
                } else {
                    byte |= 0x80;
                }
                out.push_back(byte);
            }
        }

        void name(std::vector<uint8_t> &out, const std::string &text) {
            uleb(out, text.size());
            out.insert(out.end(), text.begin(), text.end());
        }

        // 5-byte padded LEBs: a relocation target must have a fixed width so the
        // linker can patch any value in place.
        void uleb_padded(std::vector<uint8_t> &out, uint32_t value) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
                value >>= 7;
            }
            out.push_back(static_cast<uint8_t>(value & 0x7F));
        }
        void sleb_padded(std::vector<uint8_t> &out, const int64_t value) {
            auto v = value;
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
                v >>= 7;
            }
            out.push_back(static_cast<uint8_t>(v & 0x7F));
        }

        void section(std::vector<uint8_t> &out, const uint8_t id,
                      const std::vector<uint8_t> &payload) {
            if (payload.empty()) return;
            out.push_back(id);
            uleb(out, payload.size());
            out.insert(out.end(), payload.begin(), payload.end());
        }
    }

    // --- Code -------------------------------------------------------------------

    void Code::op(const uint8_t opcode) { bytes.push_back(opcode); }
    void Code::op_fc(const uint32_t sub) {
        bytes.push_back(0xFC);
        uleb(bytes, sub);
    }

    void Code::unreachable_op() { op(0x00); }
    void Code::block_void() { op(0x02); bytes.push_back(0x40); }
    void Code::loop_void() { op(0x03); bytes.push_back(0x40); }
    void Code::if_void() { op(0x04); bytes.push_back(0x40); }
    void Code::else_op() { op(0x05); }
    void Code::end() { op(0x0B); }
    void Code::br(const uint32_t depth) { op(0x0C); uleb(bytes, depth); }
    void Code::br_if(const uint32_t depth) { op(0x0D); uleb(bytes, depth); }
    void Code::br_table(const std::vector<uint32_t> &depths, const uint32_t default_depth) {
        op(0x0E);
        uleb(bytes, depths.size());
        for (const auto depth : depths) uleb(bytes, depth);
        uleb(bytes, default_depth);
    }
    void Code::return_op() { op(0x0F); }
    void Code::call(const uint32_t function_index) { op(0x10); uleb(bytes, function_index); }
    void Code::call_indirect(const uint32_t type_index) {
        op(0x11);
        uleb(bytes, type_index);
        bytes.push_back(0x00); // table 0
    }
    void Code::drop() { op(0x1A); }
    void Code::select_op() { op(0x1B); }

    void Code::local_get(const uint32_t index) { op(0x20); uleb(bytes, index); }
    void Code::local_set(const uint32_t index) { op(0x21); uleb(bytes, index); }
    void Code::local_tee(const uint32_t index) { op(0x22); uleb(bytes, index); }
    void Code::global_get(const uint32_t index) { op(0x23); uleb(bytes, index); }
    void Code::global_set(const uint32_t index) { op(0x24); uleb(bytes, index); }

    void Code::load(const ValType type, const uint32_t bits, const bool sign_extend,
                     const uint32_t align_log2, const uint32_t offset) {
        uint8_t opcode = 0;
        if (type == ValType::I32) {
            if (bits == 8) opcode = sign_extend ? 0x2C : 0x2D;
            else if (bits == 16) opcode = sign_extend ? 0x2E : 0x2F;
            else opcode = 0x28;
        } else if (type == ValType::I64) {
            if (bits == 8) opcode = sign_extend ? 0x30 : 0x31;
            else if (bits == 16) opcode = sign_extend ? 0x32 : 0x33;
            else if (bits == 32) opcode = sign_extend ? 0x34 : 0x35;
            else opcode = 0x29;
        } else if (type == ValType::F32) {
            opcode = 0x2A;
        } else {
            opcode = 0x2B;
        }
        op(opcode);
        uleb(bytes, align_log2);
        uleb(bytes, offset);
    }

    void Code::store(const ValType type, const uint32_t bits, const uint32_t align_log2,
                      const uint32_t offset) {
        uint8_t opcode = 0;
        if (type == ValType::I32) {
            if (bits == 8) opcode = 0x3A;
            else if (bits == 16) opcode = 0x3B;
            else opcode = 0x36;
        } else if (type == ValType::I64) {
            if (bits == 8) opcode = 0x3C;
            else if (bits == 16) opcode = 0x3D;
            else if (bits == 32) opcode = 0x3E;
            else opcode = 0x37;
        } else if (type == ValType::F32) {
            opcode = 0x38;
        } else {
            opcode = 0x39;
        }
        op(opcode);
        uleb(bytes, align_log2);
        uleb(bytes, offset);
    }

    void Code::memory_copy() {
        op_fc(10);
        bytes.push_back(0x00); // destination memory
        bytes.push_back(0x00); // source memory
    }
    void Code::memory_fill() {
        op_fc(11);
        bytes.push_back(0x00);
    }

    void Code::i32_const(const int32_t value) { op(0x41); sleb(bytes, value); }
    void Code::i64_const(const int64_t value) { op(0x42); sleb(bytes, value); }
    void Code::f32_const_bits(const uint32_t bits) {
        op(0x43);
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void Code::f64_const_bits(const uint64_t bits) {
        op(0x44);
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }

    void Code::call_reloc(const uint32_t function_index, const uint32_t symbol) {
        op(0x10);
        relocs.push_back({reloc::FUNCTION_INDEX_LEB,
                          static_cast<uint32_t>(bytes.size()), symbol, 0});
        uleb_padded(bytes, function_index);
    }
    void Code::i32_const_table(const uint32_t symbol) {
        op(0x41);
        relocs.push_back({reloc::TABLE_INDEX_SLEB,
                          static_cast<uint32_t>(bytes.size()), symbol, 0});
        sleb_padded(bytes, 0);
    }
    void Code::i32_const_memory(const uint32_t symbol, const int64_t addend) {
        op(0x41);
        relocs.push_back({reloc::MEMORY_ADDR_SLEB,
                          static_cast<uint32_t>(bytes.size()), symbol, addend});
        sleb_padded(bytes, 0);
    }
    void Code::global_get_reloc(const uint32_t global_index, const uint32_t symbol) {
        op(0x23);
        relocs.push_back({reloc::GLOBAL_INDEX_LEB,
                          static_cast<uint32_t>(bytes.size()), symbol, 0});
        uleb_padded(bytes, global_index);
    }
    void Code::global_set_reloc(const uint32_t global_index, const uint32_t symbol) {
        op(0x24);
        relocs.push_back({reloc::GLOBAL_INDEX_LEB,
                          static_cast<uint32_t>(bytes.size()), symbol, 0});
        uleb_padded(bytes, global_index);
    }
    void Code::call_indirect_reloc(const uint32_t type_index) {
        op(0x11);
        // For TYPE_INDEX_LEB the reloc's index field IS the type index.
        relocs.push_back({reloc::TYPE_INDEX_LEB,
                          static_cast<uint32_t>(bytes.size()), type_index, 0});
        uleb_padded(bytes, type_index);
        bytes.push_back(0x00); // table 0
    }

    void Code::append(const Code &other) {
        const auto base = static_cast<uint32_t>(bytes.size());
        for (auto entry : other.relocs) {
            entry.offset += base;
            relocs.push_back(entry);
        }
        bytes.insert(bytes.end(), other.bytes.begin(), other.bytes.end());
    }

    // --- Module -----------------------------------------------------------------

    auto Module::intern_type(FuncType type) -> uint32_t {
        for (uint32_t i = 0; i < types.size(); ++i) {
            if (types[i] == type) return i;
        }
        types.push_back(std::move(type));
        return static_cast<uint32_t>(types.size() - 1);
    }

    auto Module::serialize() const -> std::vector<uint8_t> {
        std::vector<uint8_t> out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

        // 1: types
        {
            std::vector<uint8_t> payload;
            uleb(payload, types.size());
            for (const auto &type : types) {
                payload.push_back(0x60);
                uleb(payload, type.params.size());
                for (const auto param : type.params) payload.push_back(static_cast<uint8_t>(param));
                uleb(payload, type.results.size());
                for (const auto result : type.results) payload.push_back(static_cast<uint8_t>(result));
            }
            section(out, 1, payload);
        }
        // 2: imports
        if (!imports.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, imports.size());
            for (const auto &import : imports) {
                name(payload, import.module);
                name(payload, import.name);
                payload.push_back(0x00); // function
                uleb(payload, import.type_index);
            }
            section(out, 2, payload);
        }
        // 3: functions
        if (!functions.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, functions.size());
            for (const auto &function : functions) uleb(payload, function.type_index);
            section(out, 3, payload);
        }
        // 4: table
        if (table_size > 0) {
            std::vector<uint8_t> payload;
            uleb(payload, 1);
            payload.push_back(static_cast<uint8_t>(ValType::Funcref));
            payload.push_back(0x00); // min only
            uleb(payload, table_size);
            section(out, 4, payload);
        }
        // 5: memory
        if (memory_min_pages > 0) {
            std::vector<uint8_t> payload;
            uleb(payload, 1);
            payload.push_back(0x00); // min only
            uleb(payload, memory_min_pages);
            section(out, 5, payload);
        }
        // 6: globals
        if (!globals.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, globals.size());
            for (const auto &global : globals) {
                payload.push_back(static_cast<uint8_t>(global.type));
                payload.push_back(global.mutable_ ? 0x01 : 0x00);
                if (global.type == ValType::I64) {
                    payload.push_back(0x42);
                    sleb(payload, global.init);
                } else {
                    payload.push_back(0x41);
                    sleb(payload, static_cast<int32_t>(global.init));
                }
                payload.push_back(0x0B);
            }
            section(out, 6, payload);
        }
        // 7: exports
        if (!exports.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, exports.size());
            for (const auto &entry : exports) {
                name(payload, entry.name);
                payload.push_back(static_cast<uint8_t>(entry.kind));
                uleb(payload, entry.index);
            }
            section(out, 7, payload);
        }
        // 9: elements (one active segment at table offset 1; a null function
        // pointer is 0, which traps on call_indirect instead of dispatching)
        if (!table_elements.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, 1);
            payload.push_back(0x00); // active, table 0, funcref
            payload.push_back(0x41); // i32.const 1
            sleb(payload, 1);
            payload.push_back(0x0B);
            uleb(payload, table_elements.size());
            for (const auto index : table_elements) uleb(payload, index);
            section(out, 9, payload);
        }
        // 10: code
        if (!functions.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, functions.size());
            for (const auto &function : functions) {
                std::vector<uint8_t> body;
                // Run-length-compressed local declarations.
                std::vector<std::pair<ValType, uint32_t>> runs;
                for (const auto local : function.locals) {
                    if (!runs.empty() && runs.back().first == local) runs.back().second += 1;
                    else runs.push_back({local, 1});
                }
                uleb(body, runs.size());
                for (const auto &[type, count] : runs) {
                    uleb(body, count);
                    body.push_back(static_cast<uint8_t>(type));
                }
                body.insert(body.end(), function.body.bytes.begin(), function.body.bytes.end());
                body.push_back(0x0B); // end
                uleb(payload, body.size());
                payload.insert(payload.end(), body.begin(), body.end());
            }
            section(out, 10, payload);
        }
        // 11: data
        if (!data.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, data.size());
            for (const auto &segment : data) {
                payload.push_back(0x00); // active, memory 0
                payload.push_back(0x41); // i32.const
                sleb(payload, static_cast<int32_t>(segment.offset));
                payload.push_back(0x0B);
                uleb(payload, segment.bytes.size());
                payload.insert(payload.end(), segment.bytes.begin(), segment.bytes.end());
            }
            section(out, 11, payload);
        }
        return out;
    }

    // --- ObjectModule -------------------------------------------------------------

    auto ObjectModule::intern_type(FuncType type) -> uint32_t {
        for (uint32_t i = 0; i < types.size(); ++i) {
            if (types[i] == type) return i;
        }
        types.push_back(std::move(type));
        return static_cast<uint32_t>(types.size() - 1);
    }

    auto ObjectModule::serialize() const -> std::vector<uint8_t> {
        std::vector<uint8_t> out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        // Reloc custom sections name their target by SECTION ORDINAL (0-based,
        // counting every section emitted); track it as we go.
        uint32_t ordinal = 0;
        int64_t code_ordinal = -1;
        int64_t data_ordinal = -1;
        const auto emit_section = [&](const uint8_t id, const std::vector<uint8_t> &payload) {
            section(out, id, payload);
            return ordinal++;
        };

        // 1: types
        {
            std::vector<uint8_t> payload;
            uleb(payload, types.size());
            for (const auto &type : types) {
                payload.push_back(0x60);
                uleb(payload, type.params.size());
                for (const auto param : type.params) payload.push_back(static_cast<uint8_t>(param));
                uleb(payload, type.results.size());
                for (const auto result : type.results) payload.push_back(static_cast<uint8_t>(result));
            }
            emit_section(1, payload);
        }
        // 2: imports — memory and the funcref table are imports in an object
        // (the linker owns both), function imports carry their symbols' names,
        // and the shadow-stack pointer is the imported global env.__stack_pointer.
        {
            std::vector<uint8_t> payload;
            const auto count = 2 + imports.size() + (import_stack_pointer ? 1 : 0);
            uleb(payload, count);
            name(payload, "env");
            name(payload, "__linear_memory");
            payload.push_back(0x02); // memory
            payload.push_back(0x00); // min only
            uleb(payload, 1);
            for (const auto &import : imports) {
                name(payload, import.module);
                name(payload, import.name);
                payload.push_back(0x00);
                uleb(payload, import.type_index);
            }
            name(payload, "env");
            name(payload, "__indirect_function_table");
            payload.push_back(0x01); // table
            payload.push_back(static_cast<uint8_t>(ValType::Funcref));
            payload.push_back(0x00); // min only
            uleb(payload, table_elements.size() + 1);
            if (import_stack_pointer) {
                name(payload, "env");
                name(payload, "__stack_pointer");
                payload.push_back(0x03); // global
                payload.push_back(static_cast<uint8_t>(ValType::I32));
                payload.push_back(0x01); // mutable
            }
            emit_section(2, payload);
        }
        // 3: functions
        if (!functions.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, functions.size());
            for (const auto &function : functions) uleb(payload, function.type_index);
            emit_section(3, payload);
        }
        // 9: elements — address-taken functions from slot 1; the linker rewrites
        // slots through the TABLE_INDEX relocations, this just reserves them.
        if (!table_elements.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, 1);
            payload.push_back(0x00);
            payload.push_back(0x41);
            sleb(payload, 1);
            payload.push_back(0x0B);
            uleb(payload, table_elements.size());
            for (const auto index : table_elements) uleb(payload, index);
            emit_section(9, payload);
        }
        // 12: datacount
        if (!segments.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, segments.size());
            emit_section(12, payload);
        }
        // 10: code, collecting section-relative reloc offsets as bodies land.
        std::vector<Reloc> code_relocs;
        if (!functions.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, functions.size());
            for (const auto &function : functions) {
                std::vector<uint8_t> body;
                std::vector<std::pair<ValType, uint32_t>> runs;
                for (const auto local : function.locals) {
                    if (!runs.empty() && runs.back().first == local) runs.back().second += 1;
                    else runs.push_back({local, 1});
                }
                uleb(body, runs.size());
                for (const auto &[type, count] : runs) {
                    uleb(body, count);
                    body.push_back(static_cast<uint8_t>(type));
                }
                const auto code_start = static_cast<uint32_t>(body.size());
                body.insert(body.end(), function.body.bytes.begin(), function.body.bytes.end());
                body.push_back(0x0B);

                std::vector<uint8_t> size_field;
                uleb(size_field, body.size());
                const auto body_base = static_cast<uint32_t>(payload.size()) +
                                       static_cast<uint32_t>(size_field.size()) + code_start;
                for (auto entry : function.body.relocs) {
                    entry.offset += body_base;
                    code_relocs.push_back(entry);
                }
                payload.insert(payload.end(), size_field.begin(), size_field.end());
                payload.insert(payload.end(), body.begin(), body.end());
            }
            code_ordinal = emit_section(10, payload);
        }
        // 11: data, likewise collecting rebased reloc offsets.
        std::vector<Reloc> data_relocs;
        if (!segments.empty()) {
            std::vector<uint8_t> payload;
            uleb(payload, segments.size());
            for (const auto &segment : segments) {
                payload.push_back(0x00);
                payload.push_back(0x41);
                sleb(payload, 0);
                payload.push_back(0x0B);
                uleb(payload, segment.bytes.size());
                const auto base = static_cast<uint32_t>(payload.size());
                payload.insert(payload.end(), segment.bytes.begin(), segment.bytes.end());
                for (auto entry : segment.relocs) {
                    entry.offset += base;
                    data_relocs.push_back(entry);
                }
            }
            data_ordinal = emit_section(11, payload);
        }
        // custom "linking": symbol table + segment info, version 2.
        {
            std::vector<uint8_t> payload;
            name(payload, "linking");
            uleb(payload, 2);

            std::vector<uint8_t> symtab;
            uleb(symtab, function_symbols.size() + data_symbols.size() +
                          (import_stack_pointer ? 1 : 0));
            for (const auto &symbol : function_symbols) {
                symtab.push_back(0x00); // SYMTAB_FUNCTION
                uint32_t flags = 0;
                if (!symbol.defined) flags |= 0x10;      // WASM_SYM_UNDEFINED
                if (symbol.exported) flags |= 0x20;      // WASM_SYM_EXPORTED
                uleb(symtab, flags);
                uleb(symtab, symbol.function_index);
                if (symbol.defined) name(symtab, symbol.name);
            }
            for (const auto &symbol : data_symbols) {
                symtab.push_back(0x01); // SYMTAB_DATA
                uleb(symtab, 0);
                name(symtab, symbol.name);
                uleb(symtab, symbol.segment);
                uleb(symtab, symbol.offset);
                uleb(symtab, symbol.size);
            }
            if (import_stack_pointer) {
                symtab.push_back(0x02); // SYMTAB_GLOBAL
                uleb(symtab, 0x10);     // undefined; named by its import
                uleb(symtab, 0);        // global index 0 (the only global import)
            }
            payload.push_back(0x08); // WASM_SYMBOL_TABLE
            uleb(payload, symtab.size());
            payload.insert(payload.end(), symtab.begin(), symtab.end());

            if (!segments.empty()) {
                std::vector<uint8_t> info;
                uleb(info, segments.size());
                for (const auto &segment : segments) {
                    name(info, segment.name);
                    uleb(info, segment.align_log2);
                    uleb(info, 0);
                }
                payload.push_back(0x05); // WASM_SEGMENT_INFO
                uleb(payload, info.size());
                payload.insert(payload.end(), info.begin(), info.end());
            }
            emit_section(0, payload);
        }
        // custom reloc sections.
        const auto emit_relocs = [&](const char *section_name, const int64_t target,
                                      const std::vector<Reloc> &entries) {
            if (target < 0 || entries.empty()) return;
            std::vector<uint8_t> payload;
            name(payload, section_name);
            uleb(payload, static_cast<uint64_t>(target));
            uleb(payload, entries.size());
            for (const auto &entry : entries) {
                payload.push_back(entry.type);
                uleb(payload, entry.offset);
                uleb(payload, entry.index);
                if (entry.type == reloc::MEMORY_ADDR_LEB ||
                    entry.type == reloc::MEMORY_ADDR_SLEB ||
                    entry.type == reloc::MEMORY_ADDR_I32) {
                    sleb(payload, entry.addend);
                }
            }
            emit_section(0, payload);
        };
        emit_relocs("reloc.CODE", code_ordinal, code_relocs);
        emit_relocs("reloc.DATA", data_ordinal, data_relocs);
        return out;
    }
}
