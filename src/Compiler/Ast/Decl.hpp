#pragma once

#include <Compiler/Ast/Stmt.hpp>
#include <Compiler/Ast/Type.hpp>
#include <Compiler/SourceLocation.hpp>

#include <string>
#include <vector>

namespace Ast {
    struct FunctionDecl;

    using Decl = std::variant<FunctionDecl>;

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
}
