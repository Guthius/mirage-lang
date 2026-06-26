#pragma once

#include <Compiler/Ast/Stmt.hpp>
#include <Compiler/Ast/Type.hpp>
#include <Compiler/SourceLocation.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Ast {
    struct FunctionDecl;
    struct ExtFunctionDecl;
    struct VarDecl;

    using Decl = std::variant<FunctionDecl, ExtFunctionDecl, VarDecl>;

    struct FunctionDecl {
        struct param {
            bool is_mut;
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_pub;
        std::string name;
        std::vector<param> params;
        std::vector<Type> return_types;
        Stmt body;
        SourceLocation location;
    };

    struct ExtFunctionDecl {
        struct param {
            std::string name;
            Type type;
            SourceLocation location;
        };

        bool is_pub;
        std::string name;
        std::vector<param> params;
        std::optional<Type> return_type;
        SourceLocation location;
    };

    struct VarDecl {
        bool is_pub;
        bool is_mut;
        std::string name;
        std::optional<Type> type;
        std::optional<Expr> init;
        SourceLocation location;
    };
}
