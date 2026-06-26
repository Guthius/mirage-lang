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
        src/Compiler/Ast/Decl.cpp
        src/Compiler/Ast/Decl.hpp
        src/Compiler/Ast/Expr.cpp
        src/Compiler/Ast/Expr.hpp
        src/Compiler/Ast/Parser.cpp
        src/Compiler/Ast/Parser.hpp
        src/Compiler/Ast/Stmt.cpp
        src/Compiler/Ast/Stmt.hpp
        src/Compiler/Ast/Type.cpp
        src/Compiler/Ast/Type.hpp
        src/Compiler/Diagnostics/DiagnosticEngine.cpp
        src/Compiler/Diagnostics/DiagnosticEngine.hpp
        src/Compiler/Lexer/Lexer.cpp
        src/Compiler/Lexer/Lexer.hpp

        src/Compiler/Sema/types/type_resolver.cpp

        src/Compiler/Sema/ResolvedType.hpp
        src/Compiler/Sema/Sema.cpp
        src/Compiler/Sema/Sema.hpp
        src/Compiler/Ast.hpp
        src/Compiler/SourceLocation.hpp
        src/Compiler/Token.cpp
        src/Compiler/Token.hpp
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
