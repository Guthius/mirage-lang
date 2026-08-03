cmake_minimum_required(VERSION 4.2)
project(Mirage LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if (LLVM_LINK_LLVM_DYLIB)
    set(LLVM_LIBS LLVM)
else ()
    llvm_map_components_to_libnames(LLVM_LIBS
            core
            support
            irreader
            passes
            x86asmparser
            x86codegen
            x86desc
            x86info
    )
endif ()

# Shared compiler core (lexer, parser, module resolver, sema) - LLVM-free,
# reused by both the `mirage` compiler and the `mirage-lsp` language server.
add_library(mirage_core OBJECT
        src/compiler/asm_lexer.cpp
        src/compiler/asm_lexer.hpp
        src/compiler/asm_parser.cpp
        src/compiler/asm_parser.hpp
        src/compiler/asm_registers.hpp
        src/compiler/ast.cpp
        src/compiler/ast.hpp
        src/compiler/ast_parser.cpp
        src/compiler/ast_parser.hpp
        src/compiler/diagnostic_engine.cpp
        src/compiler/diagnostic_engine.hpp
        src/compiler/lexer.cpp
        src/compiler/lexer.hpp
        src/compiler/mir.cpp
        src/compiler/mir.hpp
        src/compiler/mir_passes.cpp
        src/compiler/mirgen.cpp
        src/compiler/backend_x86.cpp
        src/compiler/backend_x86.hpp
        src/compiler/elf_writer.cpp
        src/compiler/elf_writer.hpp
        src/compiler/x86_encoder.cpp
        src/compiler/x86_encoder.hpp
        src/compiler/x86_regalloc.cpp
        src/compiler/x86_regalloc.hpp
        src/compiler/wasm_encoder.cpp
        src/compiler/wasm_encoder.hpp
        src/compiler/backend_wasm.cpp
        src/compiler/backend_wasm.hpp
        src/compiler/mirgen.hpp
        src/compiler/module_resolver.cpp
        src/compiler/module_resolver.hpp
        src/compiler/resolved_type.hpp
        src/compiler/sema.cpp
        src/compiler/sema.hpp
        src/compiler/sema_attributes.cpp
        src/compiler/sema_check.cpp
        src/compiler/sema_declare.cpp
        src/compiler/source_location.hpp
        src/compiler/source_manager.cpp
        src/compiler/source_manager.hpp
        src/compiler/symbol_table.hpp
        src/compiler/token.cpp
        src/compiler/token.hpp
        src/compiler/type_resolver.cpp
        src/compiler/value_resolver.cpp
)

target_include_directories(mirage_core PUBLIC src)

add_executable(mirage
        src/compiler/codegen.cpp
        src/compiler/codegen.hpp
        src/main.cpp
        $<TARGET_OBJECTS:mirage_core>
)

target_include_directories(mirage
        SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS}
        PRIVATE src
)

target_link_libraries(mirage
        PUBLIC ${LLVM_LIBS}
)

target_compile_definitions(mirage PUBLIC ${LLVM_DEFINITIONS})

add_executable(lexer_asi_test
        tests/lexer_asi_test.cpp
        src/compiler/lexer.cpp
        src/compiler/lexer.hpp
        src/compiler/token.cpp
        src/compiler/token.hpp
        src/compiler/diagnostic_engine.cpp
        src/compiler/diagnostic_engine.hpp
        src/compiler/source_manager.cpp
        src/compiler/source_manager.hpp
        src/compiler/source_location.hpp
)

target_include_directories(lexer_asi_test
        PRIVATE src
)

enable_testing()
add_test(NAME lexer_asi_test COMMAND lexer_asi_test)

add_executable(lexer_robustness_test
        tests/lexer_robustness_test.cpp
        src/compiler/lexer.cpp
        src/compiler/lexer.hpp
        src/compiler/token.cpp
        src/compiler/token.hpp
        src/compiler/diagnostic_engine.cpp
        src/compiler/diagnostic_engine.hpp
        src/compiler/source_manager.cpp
        src/compiler/source_manager.hpp
        src/compiler/source_location.hpp
)

target_include_directories(lexer_robustness_test
        PRIVATE src
)

add_test(NAME lexer_robustness_test COMMAND lexer_robustness_test)
set_tests_properties(lexer_robustness_test PROPERTIES TIMEOUT 10)

# MIR is standalone by construction — it depends on nothing but the standard library, which
# is what lets the backends be developed and tested without a front end attached.
add_executable(mir_test
        tests/mir_test.cpp
        src/compiler/mir.cpp
        src/compiler/mir_passes.cpp
        src/compiler/mir.hpp
)

add_executable(x86_encoder_test
        tests/x86_encoder_test.cpp
        src/compiler/x86_encoder.cpp
        src/compiler/x86_encoder.hpp
)

add_executable(wasm_encoder_test
        tests/wasm_encoder_test.cpp
        src/compiler/wasm_encoder.cpp
        src/compiler/wasm_encoder.hpp
)

target_include_directories(wasm_encoder_test
        PRIVATE src
)

add_test(NAME wasm_encoder_test COMMAND wasm_encoder_test)
set_tests_properties(wasm_encoder_test PROPERTIES TIMEOUT 10)

add_executable(x86_regalloc_test
        tests/x86_regalloc_test.cpp
        src/compiler/mir.cpp
        src/compiler/mir.hpp
        src/compiler/x86_regalloc.cpp
        src/compiler/x86_regalloc.hpp
)

target_include_directories(x86_regalloc_test
        PRIVATE src
)

add_test(NAME x86_regalloc_test COMMAND x86_regalloc_test)
set_tests_properties(x86_regalloc_test PROPERTIES TIMEOUT 10)

target_include_directories(x86_encoder_test
        PRIVATE src
)

add_test(NAME x86_encoder_test COMMAND x86_encoder_test)
set_tests_properties(x86_encoder_test PROPERTIES TIMEOUT 10)

target_include_directories(mir_test
        PRIVATE src
)

add_test(NAME mir_test COMMAND mir_test)
set_tests_properties(mir_test PROPERTIES TIMEOUT 10)

add_executable(diagnostic_engine_test
        tests/diagnostic_engine_test.cpp
        src/compiler/diagnostic_engine.cpp
        src/compiler/diagnostic_engine.hpp
        src/compiler/source_manager.cpp
        src/compiler/source_manager.hpp
        src/compiler/source_location.hpp
)

target_include_directories(diagnostic_engine_test
        PRIVATE src
)

add_test(NAME diagnostic_engine_test COMMAND diagnostic_engine_test)
set_tests_properties(diagnostic_engine_test PROPERTIES TIMEOUT 10)

add_executable(ast_parser_progress_test
        tests/ast_parser_progress_test.cpp
        $<TARGET_OBJECTS:mirage_core>
)

# Pins lsp::type_to_string and sema::describe_type together on the subset of
# ResolvedTypes both can render (see the test's own header comment) — the two
# printers are deliberately separate functions and have drifted before.
add_executable(type_printer_test
        tests/type_printer_test.cpp
        src/lsp/type_printer.cpp
        src/lsp/type_printer.hpp
        $<TARGET_OBJECTS:mirage_core>
)

target_include_directories(type_printer_test
        PRIVATE src
)

add_test(NAME type_printer_test COMMAND type_printer_test)
set_tests_properties(type_printer_test PROPERTIES TIMEOUT 10)

target_include_directories(ast_parser_progress_test
        PRIVATE src
)

add_test(NAME ast_parser_progress_test COMMAND ast_parser_progress_test)
set_tests_properties(ast_parser_progress_test PROPERTIES TIMEOUT 10)

include(FetchContent)
FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(mirage-lsp
        src/lsp/main.cpp
        src/lsp/server.hpp
        src/lsp/server.cpp
        src/lsp/task_queue.hpp
        src/lsp/transport.hpp
        src/lsp/transport.cpp
        src/lsp/uri.hpp
        src/lsp/uri.cpp
        src/lsp/position_encoding.hpp
        src/lsp/position_encoding.cpp
        src/lsp/analysis.hpp
        src/lsp/analysis.cpp
        src/lsp/handlers/diagnostics.hpp
        src/lsp/handlers/diagnostics.cpp
        src/lsp/handlers/locations.hpp
        src/lsp/type_printer.hpp
        src/lsp/type_printer.cpp
        src/lsp/handlers/common.hpp
        src/lsp/handlers/common.cpp
        src/lsp/handlers/ast_walker.hpp
        src/lsp/handlers/ast_walker.cpp
        src/lsp/handlers/hover.hpp
        src/lsp/handlers/hover.cpp
        src/lsp/handlers/definition.hpp
        src/lsp/handlers/definition.cpp
        src/lsp/handlers/completion.hpp
        src/lsp/handlers/completion.cpp
        src/lsp/handlers/references.hpp
        src/lsp/handlers/references.cpp
        src/lsp/handlers/inlay_hint.hpp
        src/lsp/handlers/inlay_hint.cpp
        $<TARGET_OBJECTS:mirage_core>
)

find_package(Threads REQUIRED)

target_include_directories(mirage-lsp PRIVATE src)
target_link_libraries(mirage-lsp PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
