#include "compiler/ast.hpp"
#include "compiler/diagnostic_engine.hpp"
#include "compiler/lexer.hpp"
#include "compiler/source_manager.hpp"

#include <cstdio>
#include <string_view>

namespace {
    int failures = 0;

    // Feeds `source` through the full lex -> parse pipeline and asserts it returns (doesn't
    // hang) with at least one reported error. Each `source` here reproduces one of the eleven
    // parser loop sites that used to be able to spin forever on a token none of their
    // expect()/expect_identifier()/parse_type() calls could consume - if the progress guard
    // regresses, this test hangs instead of failing, which ctest surfaces as a TIMEOUT.
    void expect_terminates_with_errors(const char *name, const std::string_view source) {
        SourceManager source_manager;
        DiagnosticEngine diagnostics(source_manager);

        auto tokens = lexer::tokenize(source, "<test>", diagnostics);
        auto decls = ast::parse(tokens, diagnostics);

        if (!diagnostics.has_errors()) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: expected at least one parse error, got none\n", name);
        }
    }
}

auto main() -> int {
    // Shape 1: struct/union/enum/trait/impl body loops - a stray token that starts neither a
    // member nor the closing brace used to leave the loop stuck at the same position forever.
    expect_terminates_with_errors("struct_body_stray_token", "type T = struct { ! }\n");
    expect_terminates_with_errors("union_body_stray_token", "type T = union { ! }\n");
    expect_terminates_with_errors("enum_body_stray_token", "type T = enum { ! }\n");
    expect_terminates_with_errors("trait_body_stray_token", "type T = trait { ! }\n");
    expect_terminates_with_errors("trait_composition_list_stray_token", "type T = trait(A, ! ) { }\n");
    expect_terminates_with_errors("impl_body_stray_token", "impl T { ! }\n");
    expect_terminates_with_errors("trait_impl_body_stray_token", "impl T for S { ! }\n");

    // Shape 2: fn/ext fn/macro/impl-method/trait-method parameter list loops - a token that's
    // neither an identifier, ':', a type-starter, ',', nor ')' used to spin the same way.
    // '&x: i32' is the single easiest of these to hit while just typing ordinary code.
    expect_terminates_with_errors("fn_params_stray_token", "fn foo(&x: i32) {}\n");
    expect_terminates_with_errors("ext_fn_params_stray_token", "ext fn foo(&x: i32)\n");
    expect_terminates_with_errors("macro_params_stray_token", "macro foo(&x: i32) -> i32 = x\n");
    expect_terminates_with_errors("trait_method_params_stray_token", "type T = trait { fn foo(self, &x: i32) }\n");
    expect_terminates_with_errors("impl_method_params_stray_token", "impl T { fn foo(self, &x: i32) {} }\n");

    // Shape 3: braced-initializer member loops. '{.A: 1}' enters the bitset member-list loop,
    // whose expect(',') fails on the ':' without consuming it - and neither does the
    // expect('.')/expect_identifier() pair at the top of the next iteration, so the loop sat
    // on the same token forever. Found via examples/lexer/token.mir, which reaches it through
    // a postfix 'kind match { ... }' form the grammar does not define.
    expect_terminates_with_errors("bitset_member_list_bad_separator", "fn f() { const x := {.A: 1} }\n");
    expect_terminates_with_errors("bitset_member_list_stray_token", "fn f() { const x := {.A ! .B} }\n");
    expect_terminates_with_errors("match_operand_braced_initializer", "fn f() { match { .A: 1 } }\n");
    expect_terminates_with_errors("postfix_match_form", "fn f(k: i32) -> i32 { return k match { .A: 1, .B: 2 } }\n");
    expect_terminates_with_errors("struct_field_list_bad_separator", "fn f() { const x := {.a = 1 : 2} }\n");

    // Shape 4: the remaining delimiter-terminated list loops that were still unguarded.
    expect_terminates_with_errors("call_args_stray_token", "fn f() { g(1 ! 2) }\n");
    expect_terminates_with_errors("multi_return_stray_token", "fn f() -> ( ! ) {}\n");
    expect_terminates_with_errors("fnptr_params_stray_token", "type F = fn( ! ) -> i32\n");
    expect_terminates_with_errors("fnptr_multi_return_stray_token", "type F = fn() -> ( ! )\n");
    expect_terminates_with_errors("switch_arms_stray_token", "fn f() { switch y { ! } }\n");
    expect_terminates_with_errors("tagged_payload_stray_token", "fn f() { const x := .V{.a = 1 ! } }\n");
    expect_terminates_with_errors("qualified_payload_stray_token", "fn f() { const x := T.V{.a = 1 ! } }\n");
    expect_terminates_with_errors("match_arms_stray_token", "fn f() { const x := match y { ! } }\n");

    if (failures > 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::puts("all parser progress-guard tests passed");
    return 0;
}
