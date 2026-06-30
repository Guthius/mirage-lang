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

add_executable(mirage
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

        src/main.cpp
)

target_include_directories(mirage
        SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS}
        PRIVATE src
)

target_link_libraries(mirage
        PUBLIC ${LLVM_LIBS}
)

target_compile_definitions(mirage PUBLIC ${LLVM_DEFINITIONS})
