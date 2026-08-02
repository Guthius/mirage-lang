#include "module_resolver.hpp"

#include "lexer.hpp"
#include "source_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace ast {
    namespace {
        auto load_and_parse(const std::string &canonical_path, SourceManager &source_manager, DiagnosticEngine &diagnostics, Program &program) -> Module {
            std::error_code ec;
            std::filesystem::directory_iterator dir(canonical_path, ec);
            if (ec) {
                diagnostics.report_error(
                    DiagnosticStage::Parser, {},
                    std::format("cannot read module directory '{}'", canonical_path));
                return {};
            }

            std::vector<std::filesystem::path> files;
            for (const auto &entry : dir) {
                if (entry.is_regular_file() && entry.path().extension() == ".mir") {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());

            Module module;
            for (const auto &file : files) {
                const auto source_file = source_manager.load(file.string(), diagnostics);
                // An empty .mir file is legal and contributes nothing. A file that failed to
                // load is not -- but SourceManager::load already reported "cannot open module
                // file" for that case, so the only thing needed here is to not count it.
                if (source_file.text.empty()) continue;
                const auto errors_before = diagnostics.error_count();
                auto tokens = lexer::tokenize(source_file.text, source_file.filename, diagnostics);
                if (diagnostics.error_count() > errors_before) continue;
                program.file_count += 1;
                program.token_count += tokens.size();
                module.push_back(FileAST{
                    // source_file.filename views SourceManager-owned storage (stable for the
                    // life of the compilation — see SourceManager's lifetime contract), the
                    // same storage every token location in this file points into.
                    .file_path = std::string(source_file.filename),
                    .declarations = parse(tokens, diagnostics),
                    .location = SourceLocation{.filename = source_file.filename},
                });
            }
            return module;
        }

        // Walks every file's declarations — including files a '#compile_only_if' will later
        // exclude: inclusion is a sema decision, and excluded files are still fully
        // type-checked, so their imports must resolve (and load) like anyone else's.
        auto find_import_strings(const Module &module) -> std::vector<std::pair<std::string, SourceLocation>> {
            std::vector<std::pair<std::string, SourceLocation>> found;

            for (auto &decl : all_decls(module)) {
                if (auto *var_decl = std::get_if<VarDecl>(&decl); var_decl && var_decl->init) {
                    if (const auto *import_stmt = find_leaf_import(*var_decl->init)) {
                        found.emplace_back(import_stmt->module_name, import_stmt->location);
                    }
                } else if (const auto *bare_import = std::get_if<BareImportDecl>(&decl)) {
                    found.emplace_back(bare_import->path, bare_import->location);
                }
            }

            return found;
        }

        auto is_contained_in(const std::filesystem::path &base, const std::filesystem::path &candidate) -> bool {
            auto base_it = base.begin();
            auto cand_it = candidate.begin();
            for (; base_it != base.end(); ++base_it, ++cand_it) {
                if (cand_it == candidate.end() || *cand_it != *base_it) {
                    return false;
                }
            }
            return true;
        }

        auto resolve_import_path(const std::string &importer_path, const std::string &import_path, const std::string &mirage_path) -> std::string {
            // An absolute import path escapes the module tree entirely. Per
            // fs::path::operator/ an absolute right-hand side DISCARDS the left, so
            // 'import("/etc")' resolved straight to /etc -- the importer's directory was never
            // consulted, and unlike the stdlib fallback below there was no containment check to
            // catch it.
            //
            // Only absolute paths are rejected. Upward '..' traversal stays allowed, and is
            // deliberately not treated as an escape: it is how the corpus's own sibling-module
            // imports work (examples/example_reflection and others reach the shared runtime with
            // 'import("../../runtime/type_info")', and example_attr_init_cycle/b uses
            // 'import("..")'). Constraining that would break working multi-directory projects,
            // which is a language-design decision, not a bug fix.
            if (std::filesystem::path(import_path).is_absolute()) {
                return {};
            }

            auto candidate = std::filesystem::path(importer_path) / import_path;
            if (std::filesystem::is_directory(candidate)) {
                return canonicalize(candidate.string());
            }

            if (mirage_path.empty()) {
                return {};
            }

            auto fallback_candidate = std::filesystem::path(mirage_path) / import_path;
            if (!std::filesystem::is_directory(fallback_candidate)) {
                return {};
            }

            auto canonical_fallback = canonicalize(fallback_candidate.string());
            if (canonical_fallback.empty()) {
                return {};
            }

            if (!is_contained_in(std::filesystem::path(mirage_path), std::filesystem::path(canonical_fallback))) {
                return {};
            }

            return canonical_fallback;
        }

        void visit(const std::string &path, Program &program, SourceManager &source_manager, DiagnosticEngine &diagnostics, const std::string &mirage_path) {
            // try_emplace already reports whether the key was present, so a preceding
            // contains() was a second hash+probe for the same answer.
            auto [it, inserted] = program.modules.try_emplace(path);
            if (!inserted) {
                return;
            }

            it->second = load_and_parse(it->first, source_manager, diagnostics, program);

            for (auto &[import_str, import_location] : find_import_strings(program.modules[path])) {
                auto resolved_path = resolve_import_path(path, import_str, mirage_path);
                if (resolved_path.empty()) {
                    diagnostics.report_error(
                        DiagnosticStage::Parser, import_location,
                        std::format("cannot resolve import path '{}' from '{}'", import_str, path));

                    continue;
                }

                program.module_imports[path][import_str] = resolved_path;

                visit(resolved_path, program, source_manager, diagnostics, mirage_path);
            }
        }
    }

    auto resolve_contained_path(const std::string &base_dir, const std::string &relative_path) -> std::string {
        const std::filesystem::path rel(relative_path);
        if (rel.is_absolute()) {
            return {};
        }

        std::error_code ec;
        const auto candidate = std::filesystem::path(base_dir) / rel;
        const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, ec);
        if (ec || !is_contained_in(std::filesystem::path(base_dir), canonical_candidate)) {
            return {};
        }

        return canonical_candidate.string();
    }

    auto canonicalize(const std::string &path) -> std::string {
        std::error_code error;

        const auto canonical_path = std::filesystem::weakly_canonical(path, error);
        if (error) {
            return {};
        }

        return canonical_path.string();
    }

    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics,
                 const std::string &std_path_override) -> Program {
        Program program;

        const auto canonical = canonicalize(root_module_path);
        if (canonical.empty() || !std::filesystem::is_directory(canonical)) {
            diagnostics.report_error(
                DiagnosticStage::Parser, {},
                std::format("'{}' is not a valid module directory", root_module_path));

            return program;
        }

        program.root_module_path = canonical;

        std::string mirage_path;
        if (!std_path_override.empty()) {
            auto candidate = canonicalize(std_path_override);
            if (!candidate.empty() && std::filesystem::is_directory(candidate)) {
                mirage_path = candidate;
            } else {
                diagnostics.report_error(
                    DiagnosticStage::Parser, {},
                    std::format("'{}' is not a valid standard library directory", std_path_override));

                return program;
            }
        } else if (const char *env_value = std::getenv("MIRAGE_PATH"); env_value != nullptr) {
            auto candidate = canonicalize(env_value);
            if (!candidate.empty() && std::filesystem::is_directory(candidate)) {
                mirage_path = candidate;
            } else {
                // Warn rather than error, mirroring '--std=' above but one severity lower: an
                // explicit flag naming a bad directory is a mistake in this invocation, whereas
                // a stale MIRAGE_PATH in the environment should not stop a build that does not
                // use the standard library at all. Silence was the wrong answer either way --
                // every later 'import("std")' then failed with a generic "cannot resolve import
                // path" that gave no hint the real cause was the environment variable.
                diagnostics.warn(
                    DiagnosticStage::Parser, {},
                    std::format("MIRAGE_PATH is set to '{}', which is not a directory; "
                                "standard-library imports will not resolve", env_value));
            }
        }

        visit(canonical, program, source_manager, diagnostics, mirage_path);

        program.ok = !diagnostics.has_errors();

        return program;
    }
}
