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

    using Decl = std::variant<FunctionDecl, ExtFunctionDecl>;

    struct FunctionDecl {
        struct Param {
            bool IsMutable;
            std::string Name;
            Type Type;
            SourceLocation Location;
        };

        bool IsPublic;
        std::string Name;
        std::vector<Param> Params;
        std::vector<Type> ReturnTypes;
        Stmt Body;
        SourceLocation Location;
    };

    struct ExtFunctionDecl {
        struct Param {
            std::string Name;
            Type Type;
            SourceLocation Location;
        };

        bool IsPublic;
        std::string Name;
        std::vector<Param> Params;
        std::optional<Type> ReturnType;
        SourceLocation Location;
    };
}
