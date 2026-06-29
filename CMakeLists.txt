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
        src/Compiler/ast.cpp
        src/Compiler/ast.hpp
        src/Compiler/ast_parser.cpp
        src/Compiler/ast_parser.hpp
        src/Compiler/diagnostic_engine.cpp
        src/Compiler/diagnostic_engine.hpp
        src/Compiler/lexer.cpp
        src/Compiler/lexer.hpp
        src/Compiler/module_resolver.cpp
        src/Compiler/module_resolver.hpp
        src/Compiler/resolved_type.hpp
        src/Compiler/sema.cpp
        src/Compiler/sema.hpp
        src/Compiler/source_location.hpp
        src/Compiler/symbol_table.hpp
        src/Compiler/token.cpp
        src/Compiler/token.hpp

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
