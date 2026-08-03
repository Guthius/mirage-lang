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

    void Code::append(const Code &other) {
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
}
