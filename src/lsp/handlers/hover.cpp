#include "hover.hpp"

#include "../type_printer.hpp"
#include "common.hpp"

#include <type_traits>

namespace lsp::handlers {
    namespace {
        auto join_return_types(const std::vector<sema::ResolvedType> &returns, const sema::Program &program,
                                const std::string &module_path) -> std::string {
            if (returns.empty()) return "";
            if (returns.size() == 1) return " -> " + type_to_string(returns[0], program, module_path);

            std::string out = " -> (";
            for (size_t i = 0; i < returns.size(); ++i) {
                if (i > 0) out += ", ";
                out += type_to_string(returns[i], program, module_path);
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
                        return "fn " + name + "(" + params + ")" + join_return_types(sym.return_types, program, module_path);
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
                        return "type " + name;
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
    }

    auto handle_hover(analysis::ProgramResult &result, const std::string &module_path,
                       const std::string &path, const size_t line, const size_t column) -> nlohmann::json {
        const auto res = resolve_at(result, module_path, path, line, column);

        switch (res.kind) {
        case Resolution::Kind::Symbol:
            return hover_json(describe_symbol(res.name, *res.symbol, result.sema_program, res.module_path));

        case Resolution::Kind::Local:
            return hover_json("(local) " + res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::Param:
            return hover_json("(param) " + res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::StructField:
        case Resolution::Kind::UnionMember:
            return hover_json(res.name + ": " + type_to_string(res.type, result.sema_program, module_path));

        case Resolution::Kind::EnumField:
            return hover_json(type_to_string(res.type, result.sema_program, module_path) + "." + res.name);

        case Resolution::Kind::Method: {
            std::string params = res.method && res.method->decl
                                      ? format_params(res.method->decl->params, res.method->param_types, result.sema_program, module_path)
                                      : "";
            std::string ret = res.method ? join_return_types(res.method->return_types, result.sema_program, module_path) : "";
            return hover_json("fn " + res.name + "(" + params + ")" + ret);
        }

        default:
            return nullptr;
        }
    }
}
