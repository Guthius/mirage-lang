cmake_minimum_required(VERSION 4.2)
project(Mirage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

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
        src/Compiler/Ast.hpp
        src/Compiler/SourceLocation.hpp
        src/Compiler/Token.cpp
        src/Compiler/Token.hpp
        src/main.cpp
)

target_include_directories(mirage PRIVATE src)
