#include <Compiler/Sema/Sema.hpp>

namespace Sema {
    namespace {
        auto resolve_builtin(const ast::BuiltinTypeKind kind) -> ResolvedType {
            switch (kind) {
            case ast::BuiltinTypeKind::U8:     return ResolvedType{TypeKind::U8};
            case ast::BuiltinTypeKind::U16:    return ResolvedType{TypeKind::U16};
            case ast::BuiltinTypeKind::U32:    return ResolvedType{TypeKind::U32};
            case ast::BuiltinTypeKind::U64:    return ResolvedType{TypeKind::U64};
            case ast::BuiltinTypeKind::I8:     return ResolvedType{TypeKind::I8};
            case ast::BuiltinTypeKind::I16:    return ResolvedType{TypeKind::I16};
            case ast::BuiltinTypeKind::I32:    return ResolvedType{TypeKind::I32};
            case ast::BuiltinTypeKind::I64:    return ResolvedType{TypeKind::I64};
            case ast::BuiltinTypeKind::F32:    return ResolvedType{TypeKind::F32};
            case ast::BuiltinTypeKind::F64:    return ResolvedType{TypeKind::F64};
            case ast::BuiltinTypeKind::Usize:  return ResolvedType{TypeKind::USize};
            case ast::BuiltinTypeKind::Bool:   return ResolvedType{TypeKind::Bool};
            case ast::BuiltinTypeKind::Byte:   return ResolvedType{TypeKind::Byte};
            case ast::BuiltinTypeKind::Error:  return ResolvedType{TypeKind::Error};
            case ast::BuiltinTypeKind::Anyptr: return ResolvedType{TypeKind::Anyptr};
            case ast::BuiltinTypeKind::Type:   return ResolvedType{TypeKind::Void};
            }
            return ResolvedType{TypeKind::Void};
        }

        auto resolve(const std::monostate &, SemaResult &) -> ResolvedType {
            return ResolvedType{
                .Kind = TypeKind::Void,
            };
        }

        auto resolve(const ast::BuiltinType &type, SemaResult &) -> ResolvedType {
            return resolve_builtin(type.kind);
        }

        auto resolve(const std::unique_ptr<ast::PointerType> &type, SemaResult &result) -> ResolvedType {
            auto pointee = resolve_type(type->pointee, result);

            const auto pointer = ResolvedType{
                .Kind = TypeKind::Pointer,
                .PointeeIndex = static_cast<int>(result.PointerPointees.size()),
            };

            result.PointerPointees.push_back(pointee);

            return pointer;
        }

        auto resolve(const ast::NamedType &, SemaResult &) -> ResolvedType {
            //         diagnostics_.ReportError(
            // DiagnosticStage::Sema, alt->Location,
            // std::format("unknown type '{}' (named types not yet supported)", alt->Name));

            return ResolvedType{
                .Kind = TypeKind::Void,
            };
        }

        auto resolve(const std::unique_ptr<ast::StructType> &, SemaResult &) -> ResolvedType {
            return ResolvedType{
                .Kind = TypeKind::Void,
            };
        }
    }

    auto resolve_type(const ast::Type &type, SemaResult &result) -> ResolvedType {
        return std::visit([&](const auto &v) { return resolve(v, result); }, type);
    }
}
