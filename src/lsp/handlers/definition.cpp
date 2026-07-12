#include "definition.hpp"

#include "../uri.hpp"
#include "common.hpp"

#include <algorithm>
#include <filesystem>
#include <type_traits>

namespace lsp::handlers {
    namespace {
        auto location_at(const std::string &filename, const size_t line, const size_t column) -> nlohmann::json {
            if (filename.empty()) return nullptr;
            const auto zero_line = line == 0 ? 0 : line - 1;
            const auto zero_column = column == 0 ? 0 : column - 1;
            return {
                {"uri", path_to_uri(filename)},
                {"range", {
                    {"start", {{"line", zero_line}, {"character", zero_column}}},
                    {"end", {{"line", zero_line}, {"character", zero_column}}},
                }},
            };
        }

        auto location_json(const SourceLocation &loc) -> nlohmann::json {
            return location_at(std::string(loc.filename), loc.line, loc.column);
        }

        // Best-effort target for jumping to an import: the module directory
        // has no single declaration site, so we point at the first .mir file
        // in it (sorted, same ordering module_resolver.cpp uses to build the
        // module), position (1, 1).
        auto first_mir_file_in(const std::string &dir) -> std::string {
            std::error_code ec;
            std::vector<std::filesystem::path> files;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mir") {
                    files.push_back(entry.path());
                }
            }
            std::ranges::sort(files);
            return files.empty() ? "" : files.front().string();
        }
    }

    auto handle_definition(analysis::ProgramResult &result, const std::string &module_path, const std::string &path,
                            const size_t line, const size_t column) -> nlohmann::json {
        const auto res = resolve_at(result, module_path, path, line, column);

        switch (res.kind) {
        case Resolution::Kind::Symbol:
            return std::visit(
                [&]<typename T>(const T &sym) -> nlohmann::json {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, sema::ImportSymbol>) {
                        return location_at(first_mir_file_in(sym.module_path), 1, 1);
                    } else if constexpr (std::is_same_v<S, sema::GlobalSymbol> ||
                                          std::is_same_v<S, sema::FunctionSymbol> ||
                                          std::is_same_v<S, sema::ExtFunctionSymbol> ||
                                          std::is_same_v<S, sema::MacroSymbol>) {
                        return sym.decl ? location_json(sym.decl->location) : nullptr;
                    } else if constexpr (std::is_same_v<S, sema::TypeSymbol>) {
                        return location_json(sym.location);
                    } else {
                        return nullptr;
                    }
                },
                *res.symbol);

        case Resolution::Kind::Local:
        case Resolution::Kind::Param:
        case Resolution::Kind::StructField:
        case Resolution::Kind::UnionMember:
        case Resolution::Kind::EnumField:
        case Resolution::Kind::Variant:
        case Resolution::Kind::Method:
            return location_json(res.location);

        default:
            return nullptr;
        }
    }
}
