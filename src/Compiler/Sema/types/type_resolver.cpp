#include <Compiler/Sema/Sema.hpp>

namespace Sema {
    namespace {
        auto resolve_builtin(const Ast::BuiltinTypeKind kind) -> ResolvedType {
            switch (kind) {
            case Ast::BuiltinTypeKind::U8:     return ResolvedType{TypeKind::U8};
            case Ast::BuiltinTypeKind::U16:    return ResolvedType{TypeKind::U16};
            case Ast::BuiltinTypeKind::U32:    return ResolvedType{TypeKind::U32};
            case Ast::BuiltinTypeKind::U64:    return ResolvedType{TypeKind::U64};
            case Ast::BuiltinTypeKind::I8:     return ResolvedType{TypeKind::I8};
            case Ast::BuiltinTypeKind::I16:    return ResolvedType{TypeKind::I16};
            case Ast::BuiltinTypeKind::I32:    return ResolvedType{TypeKind::I32};
            case Ast::BuiltinTypeKind::I64:    return ResolvedType{TypeKind::I64};
            case Ast::BuiltinTypeKind::F32:    return ResolvedType{TypeKind::F32};
            case Ast::BuiltinTypeKind::F64:    return ResolvedType{TypeKind::F64};
            case Ast::BuiltinTypeKind::Usize:  return ResolvedType{TypeKind::USize};
            case Ast::BuiltinTypeKind::Bool:   return ResolvedType{TypeKind::Bool};
            case Ast::BuiltinTypeKind::Byte:   return ResolvedType{TypeKind::Byte};
            case Ast::BuiltinTypeKind::Error:  return ResolvedType{TypeKind::Error};
            case Ast::BuiltinTypeKind::Anyptr: return ResolvedType{TypeKind::Anyptr};
            case Ast::BuiltinTypeKind::Type:   return ResolvedType{TypeKind::Void};
            }
            return ResolvedType{TypeKind::Void};
        }

        auto resolve(const std::monostate &, SemaResult &) -> ResolvedType {
            return ResolvedType{
                .Kind = TypeKind::Void,
            };
        }

        auto resolve(const Ast::BuiltinType &type, SemaResult &) -> ResolvedType {
            return resolve_builtin(type.Kind);
        }

        auto resolve(const std::unique_ptr<Ast::PointerType> &type, SemaResult &result) -> ResolvedType {
            auto pointee = resolve_type(type->Pointee, result);

            const auto pointer = ResolvedType{
                .Kind = TypeKind::Pointer,
                .PointeeIndex = static_cast<int>(result.PointerPointees.size()),
            };

            result.PointerPointees.push_back(pointee);

            return pointer;
        }

        auto resolve(const Ast::NamedType &, SemaResult &) -> ResolvedType {
            //         diagnostics_.ReportError(
            // DiagnosticStage::Sema, alt->Location,
            // std::format("unknown type '{}' (named types not yet supported)", alt->Name));

            return ResolvedType{
                .Kind = TypeKind::Void,
            };
        }
    }

    auto resolve_type(const Ast::Type &type, SemaResult &result) -> ResolvedType {
        return std::visit([&](const auto &v) { return resolve(v, result); }, type);
    }
}
