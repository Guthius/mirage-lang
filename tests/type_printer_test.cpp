// Locks the two ResolvedType renderers together on their shared subset.
//
// lsp/type_printer.cpp's type_to_string and compiler/sema.cpp's describe_type
// deliberately stay separate functions (describe_type must remain a simple,
// LLVM-free diagnostic formatter; type_to_string qualifies by module and expands
// generics for hover). Each carries a comment claiming to mirror the other, and
// they had already drifted once — Any/Type/Namespace rendered as "<type>" in
// diagnostics while hover said "any"/"type"/"<namespace>". This test pins every
// type both can render without a full analysed program, so the next drift fails
// here instead of surfacing as a diagnostic and a hover spelling the same type
// differently.

#include "compiler/sema.hpp"
#include "lsp/type_printer.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {
    int failures = 0;

    void check_agree(const sema::ResolvedType &type, const sema::Program &program, const char *label) {
        // "" as the current module: describe_type never module-qualifies, so agreement
        // is only claimed for types with no declaring module in the first place.
        const auto lsp_text = lsp::type_to_string(type, program, "");
        const auto sema_text = sema::describe_type(type, program);
        if (lsp_text == sema_text) {
            return;
        }
        ++failures;
        std::fprintf(stderr, "FAIL %s: type_to_string=\"%s\" describe_type=\"%s\"\n",
                     label, lsp_text.c_str(), sema_text.c_str());
    }
}

int main() {
    sema::Program program;

    // Builtins and the kinds carrying no side-table index.
    const std::pair<sema::TypeKind, const char *> plain_kinds[] = {
        {sema::TypeKind::Invalid, "invalid"},
        {sema::TypeKind::Void, "void"},
        {sema::TypeKind::U8, "u8"},
        {sema::TypeKind::U16, "u16"},
        {sema::TypeKind::U32, "u32"},
        {sema::TypeKind::U64, "u64"},
        {sema::TypeKind::I8, "i8"},
        {sema::TypeKind::I16, "i16"},
        {sema::TypeKind::I32, "i32"},
        {sema::TypeKind::I64, "i64"},
        {sema::TypeKind::F32, "f32"},
        {sema::TypeKind::F64, "f64"},
        {sema::TypeKind::USize, "usize"},
        {sema::TypeKind::Bool, "bool"},
        {sema::TypeKind::Anyptr, "anyptr"},
        {sema::TypeKind::Any, "any"},
        {sema::TypeKind::Type, "type"},
        {sema::TypeKind::Namespace, "namespace"},
    };
    for (const auto &[kind, label] : plain_kinds) {
        check_agree(sema::ResolvedType{.kind = kind}, program, label);
    }

    // Pointer to a builtin.
    program.pointer_pointees.push_back(sema::ResolvedType{.kind = sema::TypeKind::I32});
    check_agree(sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = 0}, program, "*i32");

    // Slice of a builtin.
    program.slices.push_back(sema::SliceInfo{.element_type = sema::ResolvedType{.kind = sema::TypeKind::U8}});
    check_agree(sema::ResolvedType{.kind = sema::TypeKind::Slice, .slice_index = 0}, program, "[]u8");

    // Array of a builtin.
    program.arrays.push_back(sema::ArrayInfo{.element_type = sema::ResolvedType{.kind = sema::TypeKind::U64}, .count = 4});
    check_agree(sema::ResolvedType{.kind = sema::TypeKind::Array, .array_index = 0}, program, "[4]u64");

    // Nested: pointer to slice of builtin.
    program.pointer_pointees.push_back(sema::ResolvedType{.kind = sema::TypeKind::Slice, .slice_index = 0});
    check_agree(sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = 1}, program, "*[]u8");

    // An unconstrained Opaque with no recorded parameter name — both fall back to the
    // bare placeholder.
    check_agree(sema::ResolvedType{.kind = sema::TypeKind::Opaque}, program, "opaque");

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all type printer agreement checks passed\n");
    return 0;
}
