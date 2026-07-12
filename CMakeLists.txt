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
        src/compiler/ast.cpp
        src/compiler/ast.hpp
        src/compiler/ast_parser.cpp
        src/compiler/ast_parser.hpp
        src/compiler/diagnostic_engine.cpp
        src/compiler/diagnostic_engine.hpp
        src/compiler/lexer.cpp
        src/compiler/lexer.hpp
        src/compiler/module_resolver.cpp
        src/compiler/module_resolver.hpp
        src/compiler/resolved_type.hpp
        src/compiler/sema.cpp
        src/compiler/sema.hpp
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

include(FetchContent)
FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(mirage-lsp
        src/lsp/main.cpp
        src/lsp/server.hpp
        src/lsp/server.cpp
        src/lsp/transport.hpp
        src/lsp/transport.cpp
        src/lsp/uri.hpp
        src/lsp/uri.cpp
        src/lsp/analysis.hpp
        src/lsp/analysis.cpp
        src/lsp/handlers/diagnostics.hpp
        src/lsp/handlers/diagnostics.cpp
        src/lsp/type_printer.hpp
        src/lsp/type_printer.cpp
        src/lsp/handlers/common.hpp
        src/lsp/handlers/common.cpp
        src/lsp/handlers/hover.hpp
        src/lsp/handlers/hover.cpp
        src/lsp/handlers/definition.hpp
        src/lsp/handlers/definition.cpp
        $<TARGET_OBJECTS:mirage_core>
)

target_include_directories(mirage-lsp PRIVATE src)
target_link_libraries(mirage-lsp PRIVATE nlohmann_json::nlohmann_json)
