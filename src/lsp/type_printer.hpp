#pragma once

#include "compiler/resolved_type.hpp"
#include "compiler/sema.hpp"

#include <string>

namespace lsp {
    // The single canonical ResolvedType -> human-readable-string pretty
    // printer (e.g. "u32", "*u8", "[]i32", "MyStruct", "mod.OtherType",
    // "fn(u32) -> bool"). Hover is the only current caller, but any future
    // capability (e.g. completion) that needs to display a type must reuse
    // this rather than reimplementing it. `current_module_path` decides
    // whether a Struct/Enum/Union name is qualified with a "module." prefix
    // (qualified only when the type's owning module differs from it).
    auto type_to_string(const sema::ResolvedType &type, const sema::Program &program,
                         const std::string &current_module_path) -> std::string;

    // Expands a Struct/Enum/Union type into its full definition (e.g.
    // "struct Foo { x: i32, y: i32 }", "enum Color { Red = 0, Green = 1 }",
    // "union(enum) Shape { circle: { radius: f64 }, point }") rather than just its bare name.
    // For any other TypeKind, or if the type's info can't be found, falls back to
    // `type_to_string`. Hovering a type name is the intended caller - "type Foo" alone tells
    // the user nothing they didn't already know from the hovered text itself.
    auto describe_type_definition(const sema::ResolvedType &type, const sema::Program &program,
                                   const std::string &current_module_path) -> std::string;
}
