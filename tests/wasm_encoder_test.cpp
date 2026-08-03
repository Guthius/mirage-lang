// Byte-exact tests for the wasm module encoder (stage 7). The heavyweight check
// is node's own validator, which every wasm_differential_test.py run drives over
// the whole corpus; these pin the encoding primitives that would fail SILENTLY
// there — LEB128 edge cases (a wrong sign bit still validates, with a different
// constant) and the section framing of a minimal module, hand-derived from the
// spec.

#include "compiler/wasm_encoder.hpp"

#include <cstdio>
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
}

int main() {
    using wasm::ValType;

    // --- signed LEB128 through i32.const/i64.const --------------------------
    {
        wasm::Code code;
        code.i32_const(42);
        expect("i32.const 42", code.bytes, {0x41, 0x2A});
    }
    {
        // -1 is a single 0x7F byte; an unsigned encoder would emit 5 bytes of
        // 0xFF and still validate — as the wrong constant.
        wasm::Code code;
        code.i32_const(-1);
        expect("i32.const -1", code.bytes, {0x41, 0x7F});
    }
    {
        // 64 sets bit 6, which collides with the sign bit and forces a
        // continuation byte.
        wasm::Code code;
        code.i32_const(64);
        expect("i32.const 64", code.bytes, {0x41, 0xC0, 0x00});
    }
    {
        wasm::Code code;
        code.i64_const(624485); // the spec's own worked example
        expect("i64.const 624485", code.bytes, {0x42, 0xE5, 0x8E, 0x26});
    }
    {
        wasm::Code code;
        code.br_table({0, 1, 2}, 0);
        expect("br_table [0 1 2] default 0", code.bytes,
               {0x0E, 0x03, 0x00, 0x01, 0x02, 0x00});
    }
    {
        wasm::Code code;
        code.load(ValType::I32, 8, false, 0, 12); // i32.load8_u align=0 offset=12
        code.store(ValType::I64, 64, 0, 8);       // i64.store align=0 offset=8
        expect("load8_u/store64 with offsets", code.bytes,
               {0x2D, 0x00, 0x0C, 0x37, 0x00, 0x08});
    }
    {
        wasm::Code code;
        code.memory_copy();
        code.memory_fill();
        expect("bulk memory ops", code.bytes,
               {0xFC, 0x0A, 0x00, 0x00, 0xFC, 0x0B, 0x00});
    }

    // --- a minimal module, hand-derived section by section ------------------
    {
        wasm::Module module;
        const auto type = module.intern_type({{}, {ValType::I32}});
        wasm::Code body;
        body.i32_const(42);
        body.return_op();
        module.functions.push_back({type, {}, body});
        module.exports.push_back({"main", wasm::ExportKind::Function, 0});
        expect("minimal module", module.serialize(),
               {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,      // magic+version
                0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,            // type ()->i32
                0x03, 0x02, 0x01, 0x00,                              // function 0: type 0
                0x07, 0x08, 0x01, 0x04, 'm', 'a', 'i', 'n', 0x00, 0x00, // export "main"
                0x0A, 0x07, 0x01, 0x05, 0x00, 0x41, 0x2A, 0x0F, 0x0B});  // code
    }

    if (failures != 0) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("\nall wasm encoder tests passed\n");
    return 0;
}
