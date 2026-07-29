#pragma once

#include "compiler/ast.hpp"
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

    // Renders a declared 'ast::Type' node directly from the AST (e.g. "*T", "[N]u8",
    // "List[i32]"), with NO sema resolution involved. The one caller that needs this: an
    // UNSPECIALIZED generic type/function/method's own declaration has no ResolvedType at
    // all for its generic-param-referencing param/return/field types ('T' in 'fn get(self)
    // -> T' is never resolved on the template — see sema's declare_type/
    // resolve_impl_signatures_for_module skip for generic decls) — hover on the
    // DECLARATION itself (as opposed to a concrete instantiation, which "type_to_string"
    // already renders correctly via generic_args_suffix) must fall back to printing the
    // syntax as written instead. Best-effort for inline anonymous struct/enum/union/trait/
    // error/bitset types and non-trivial array-size expressions (renders a short
    // placeholder) since those essentially never appear in a param/return/field type
    // position in practice.
    auto ast_type_to_string(const ast::Type &type) -> std::string;

    // Renders a 'generic_params' list as written ("[T: type, N: usize]"), or "" if empty.
    auto ast_generic_params_to_string(const std::vector<ast::GenericParam> &params) -> std::string;
}
