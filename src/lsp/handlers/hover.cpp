#include "hover.hpp"

#include "../type_printer.hpp"
#include "common.hpp"

#include "compiler/lexer.hpp"

#include <format>
#include <type_traits>

namespace lsp::handlers {
    namespace {
        // 'names' is parallel to 'returns' (from the originating ast::FunctionDecl/
        // ImplDecl::Function's optional 'return_names') — may be shorter than 'returns'
        // or absent entirely if the decl couldn't be resolved.
        auto describe_return(const sema::ResolvedType &type, size_t index, const std::vector<std::string> &names,
                              const sema::Program &program, const std::string &module_path) -> std::string {
            std::string out;
            if (index < names.size() && !names[index].empty()) out += names[index] + ": ";
            out += type_to_string(type, program, module_path);
            return out;
        }

        auto join_return_types(const std::vector<sema::ResolvedType> &returns, const std::vector<std::string> &names,
                                const sema::Program &program, const std::string &module_path) -> std::string {
            if (returns.empty()) return "";
            if (returns.size() == 1) return " -> " + describe_return(returns[0], 0, names, program, module_path);

            std::string out = " -> (";
            for (size_t i = 0; i < returns.size(); ++i) {
                if (i > 0) out += ", ";
                out += describe_return(returns[i], i, names, program, module_path);
            }
            return out + ")";
        }

        template <typename ParamList>
        auto format_params(const ParamList &decl_params, const std::vector<sema::ResolvedType> &types,
                            const sema::Program &program, const std::string &module_path) -> std::string {
            std::string out;
            for (size_t i = 0; i < types.size(); ++i) {
                if (i > 0) out += ", ";
                if (i < decl_params.size()) out += decl_params[i].name + ": ";
                out += type_to_string(types[i], program, module_path);
            }
            return out;
        }

        auto describe_symbol(const std::string &name, const sema::Symbol &symbol, const sema::Program &program,
                              const std::string &module_path) -> std::string {
            return std::visit(
                [&]<typename T>(const T &sym) -> std::string {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, sema::GlobalSymbol>) {
                        return std::string(sym.is_mut ? "mut " : "const ") + name + ": " +
                               type_to_string(sym.type, program, module_path);
                    } else if constexpr (std::is_same_v<S, sema::FunctionSymbol>) {
                        std::string params = sym.decl ? format_params(sym.decl->params, sym.params, program, module_path) : "";
                        static const std::vector<std::string> kNoNames;
                        const auto &return_names = sym.decl ? sym.decl->return_names : kNoNames;
                        return "fn " + name + "(" + params + ")" + join_return_types(sym.return_types, return_names, program, module_path);
                    } else if constexpr (std::is_same_v<S, sema::ExtFunctionSymbol>) {
                        std::string params = sym.decl ? format_params(sym.decl->params, sym.params, program, module_path) : "";
                        std::string ret = sym.return_type ? " -> " + type_to_string(*sym.return_type, program, module_path) : "";
                        return "ext fn " + name + "(" + params + ")" + ret;
                    } else if constexpr (std::is_same_v<S, sema::MacroSymbol>) {
                        std::string params = sym.decl ? format_params(sym.decl->params, sym.params, program, module_path) : "";
                        return "macro " + name + "(" + params + ") -> " + type_to_string(sym.result_type, program, module_path);
                    } else if constexpr (std::is_same_v<S, sema::ImportSymbol>) {
                        return "import \"" + sym.module_path + "\"";
                    } else if constexpr (std::is_same_v<S, sema::TypeSymbol>) {
                        return sym.resolved ? describe_type_definition(*sym.resolved, program, module_path) : "type " + name;
                    } else {
                        return name;
                    }
                },
                symbol);
        }

        auto hover_json(std::string text) -> nlohmann::json {
            return {
                {"contents", {
                    {"kind", "markdown"},
                    {"value", "```mirage\n" + std::move(text) + "\n```"},
                }},
            };
        }

        // Renders a compile-time-folded const value (see sema::evaluate_const_value) for
        // display under a const's hover signature. 'type' disambiguates a bool const (folded to
        // a bare 0/1 int64_t, same as any other integer) so it prints as 'true'/'false' rather
        // than '1'/'0', and an unsigned-typed const so it prints as unsigned rather than signed -
        // evaluate_const_value folds every integer expression through a plain signed int64_t
        // with no notion of the target type's width/signedness (e.g. '~cast(0, usize)' folds to
        // int64_t{-1} - the correct 64-bit bit pattern, just the wrong C++ type to print it as),
        // so this is the one place that actually has 'type' in hand to render it correctly.
        auto format_const_fold_value(const sema::ConstFoldValue &value, const sema::ResolvedType &type) -> std::string {
            return std::visit(
                [&]<typename T>(const T &v) -> std::string {
                    using V = std::decay_t<T>;
                    if constexpr (std::is_same_v<V, int64_t>) {
                        if (type.kind == sema::TypeKind::Bool) return v != 0 ? "true" : "false";
                        switch (type.kind) {
                        case sema::TypeKind::U8:  return std::to_string(static_cast<uint64_t>(v) & 0xFFu);
                        case sema::TypeKind::U16: return std::to_string(static_cast<uint64_t>(v) & 0xFFFFu);
                        case sema::TypeKind::U32: return std::to_string(static_cast<uint64_t>(v) & 0xFFFFFFFFu);
                        case sema::TypeKind::U64:
                        case sema::TypeKind::USize:
                            return std::to_string(static_cast<uint64_t>(v));
                        default:
                            return std::to_string(v);
                        }
                    } else {
                        return "\"" + v + "\"";
                    }
                },
                value);
        }

        // Appends the source text of a module-scope const/mut's initializer, and (if it's a
        // compile-time constant whose folded value reads DIFFERENTLY from that source text -
        // e.g. an octal '0o00010000' folds to the decimal '4096', worth showing, but a plain
        // decimal '16' folds to '16', pure repetition) its folded value as a trailing comment -
        // all on the same line as the plain "const NAME: Type" signature describe_symbol()
        // already produced. Separate from describe_symbol() itself since it needs the file's
        // token stream (to find the initializer's raw source span) and a mutable Program& +
        // DiagnosticEngine& (evaluate_const_value may need to force-resolve a referenced global
        // and can itself report diagnostics), neither of which describe_symbol()'s signature
        // carries.
        auto append_const_init_detail(std::string text, const sema::GlobalSymbol &global, const std::string &module_path,
                                       const std::vector<Token> &tokens, const std::string_view source_text,
                                       sema::Program &program, DiagnosticEngine &diag) -> std::string {
            if (!global.decl || !global.decl->init) return text;

            const auto init_text = raw_const_init_text(tokens, source_text, global.decl->location);
            if (!init_text.empty()) {
                text += " = " + init_text;
            }

            if (sema::is_constant_expr(*global.decl->init, module_path, program)) {
                if (const auto folded = sema::evaluate_const_value(*global.decl->init, module_path, program, diag)) {
                    if (const auto folded_text = format_const_fold_value(*folded, global.type); folded_text != init_text) {
                        text += " // = " + folded_text;
                    }
                }
            }
            return text;
        }
    }

    auto handle_hover(analysis::ProgramResult &result, const std::string &module_path,
                       const std::string &path, const size_t line, const size_t column) -> nlohmann::json {
        DiagnosticEngine throwaway_diag(*result.source_manager);
        const auto source_file = result.source_manager->load(path, throwaway_diag);
        if (source_file.text.empty()) return nullptr;
        const auto tokens = lexer::tokenize(source_file.text, source_file.filename, throwaway_diag);

        const auto res = resolve_at_tokens(result, module_path, tokens, line, column);

        switch (res.kind) {
        case Resolution::Kind::Symbol: {
            std::string text = describe_symbol(res.name, *res.symbol, result.sema_program, res.module_path);
            if (const auto *global = std::get_if<sema::GlobalSymbol>(res.symbol)) {
                text = append_const_init_detail(std::move(text), *global, res.module_path, tokens, source_file.text, result.sema_program, throwaway_diag);
            }
            return hover_json(text);
        }

        case Resolution::Kind::Local:
            return hover_json("(local) " + res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::Param:
            return hover_json("(param) " + res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::StructField:
        case Resolution::Kind::UnionMember:
            return hover_json(res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::EnumField: {
            std::string text = type_to_string(res.type, result.sema_program, module_path) + "." + res.name;
            if (res.type.kind == sema::TypeKind::Enum) {
                if (const auto *info = result.sema_program.enum_at(res.type.enum_index)) {
                    if (const auto *field = sema::find_enum_field_by_name(*info, res.name)) {
                        text += " = " + std::to_string(field->value);
                    }
                }
            }
            return hover_json(text);
        }

        case Resolution::Kind::Variant: {
            std::string text = type_to_string(res.type, result.sema_program, module_path) + "." + res.name;
            if (res.type.kind == sema::TypeKind::Union) {
                if (const auto *info = result.sema_program.union_at(res.type.union_index)) {
                    for (const auto &variant : info->variants) {
                        if (variant.name != res.name) continue;
                        if (variant.payload_struct_index >= 0) {
                            text += ": " + describe_type_definition(variant.payload_type, result.sema_program, module_path);
                        } else if (variant.payload_type.kind != sema::TypeKind::Invalid && variant.payload_type.kind != sema::TypeKind::Void) {
                            text += ": " + type_to_string(variant.payload_type, result.sema_program, module_path);
                        }
                        break;
                    }
                }
            }
            return hover_json(text);
        }

        case Resolution::Kind::Method: {
            std::string params = res.method && res.method->decl
                                      ? format_params(res.method->decl->params, res.method->param_types, result.sema_program, module_path)
                                      : "";
            static const std::vector<std::string> kNoNames;
            const auto &return_names = res.method && res.method->decl ? res.method->decl->return_names : kNoNames;
            std::string ret = res.method ? join_return_types(res.method->return_types, return_names, result.sema_program, module_path) : "";
            return hover_json("fn " + res.name + "(" + params + ")" + ret);
        }

        case Resolution::Kind::Builtin: {
            const std::string operand_str = res.builtin_operand_type.kind != sema::TypeKind::Invalid
                ? type_to_string(res.builtin_operand_type, result.sema_program, module_path)
                : "";
            std::string text = res.name + "(" + operand_str + ") -> " + type_to_string(res.type, result.sema_program, module_path);
            if (res.builtin_const_value) {
                text += "\n= " + std::to_string(*res.builtin_const_value);
            }
            return hover_json(text);
        }

        case Resolution::Kind::AsmRegister: {
            std::string text = "register " + res.name;
            if (!res.asm_register_family.empty()) {
                text += std::format(" ({}-bit, '{}' family)", res.asm_register_width_bits, res.asm_register_family);
            }
            return hover_json(text);
        }

        default:
            return nullptr;
        }
    }
}
