#include "module_resolver.hpp"

#include "lexer.hpp"
#include "source_manager.hpp"

#include <algorithm>
#include <array>
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

        // One search root, paired with the label the '--print-module-search' trace and the
        // failure diagnostic use for it.
        struct LabelledRoot {
            const std::string *path;
            const char *label;
        };

        // Roots 2-5 in resolution order. Root 1 (the importer's own directory) is handled
        // separately by resolve_import_path because it alone skips the containment check.
        auto ordered_roots(const SearchRoots &roots) -> std::array<LabelledRoot, 5> {
            return {{
                {&roots.root_module, "root module directory"},
                {&roots.cwd, "current working directory"},
                {&roots.compiler_dir, "compiler directory"},
                {&roots.compiler_lib_dir, "compiler directory/../lib/mirage"},
                {&roots.modules_root, "MIRAGE_MODULES_ROOT"},
            }};
        }

        struct ImportResolution {
            std::string path;        // empty on failure
            const char *root_label = "";
        };

        auto resolve_import_path(const std::string &importer_path, const std::string &import_path, const SearchRoots &roots) -> ImportResolution {
            // An absolute import path escapes the module tree entirely. Per
            // fs::path::operator/ an absolute right-hand side DISCARDS the left, so
            // 'import("/etc")' resolved straight to /etc -- the importer's directory was never
            // consulted, and unlike the rooted candidates below there was no containment check
            // to catch it.
            //
            // Only absolute paths are rejected. Upward '..' traversal stays allowed from the
            // IMPORTER's directory (root 1 below), and is deliberately not treated as an
            // escape: it is how the corpus's own sibling-module imports work
            // (examples/example_reflection and others reach the shared runtime with
            // 'import("../../runtime/type_info")', and example_attr_init_cycle/b uses
            // 'import("..")'). Constraining that would break working multi-directory projects,
            // which is a language-design decision, not a bug fix.
            if (std::filesystem::path(import_path).is_absolute()) {
                return {};
            }

            // Root 1: relative to the importing module. No containment check, per above.
            if (auto candidate = std::filesystem::path(importer_path) / import_path;
                std::filesystem::is_directory(candidate)) {
                if (auto canonical = canonicalize(candidate.string()); !canonical.empty()) {
                    return {.path = std::move(canonical), .root_label = "importing module directory"};
                }
            }

            // Roots 2-5, each contained: a path that canonicalizes outside the root it was
            // found under is rejected, so a stdlib-relative '../../..' cannot walk out of the
            // standard library and pick up an unrelated directory.
            for (const auto &[root, label] : ordered_roots(roots)) {
                if (root->empty()) {
                    continue;
                }
                auto candidate = std::filesystem::path(*root) / import_path;
                if (!std::filesystem::is_directory(candidate)) {
                    continue;
                }
                auto canonical = canonicalize(candidate.string());
                if (canonical.empty()) {
                    continue;
                }
                if (!is_contained_in(std::filesystem::path(*root), std::filesystem::path(canonical))) {
                    continue;
                }
                return {.path = std::move(canonical), .root_label = label};
            }

            return {};
        }

        // The "searched: ..." tail of the unresolved-import diagnostic. Naming every root
        // (and saying which are unset) is the difference between a one-line fix and a
        // debugging session -- the old message named neither the roots nor why they failed.
        auto describe_search_roots(const std::string &importer_path, const SearchRoots &roots) -> std::string {
            std::string out = std::format("\n  searched: {} (importing module)", importer_path);
            for (const auto &[root, label] : ordered_roots(roots)) {
                out += std::format("\n            {} ({})", root->empty() ? "<not set>" : *root, label);
            }
            if (roots.legacy_mirage_path_set && roots.modules_root.empty()) {
                // Not a fallback -- MIRAGE_PATH is genuinely not consulted. This note exists
                // only so an environment that worked for months fails legibly instead of
                // mysteriously.
                out += "\n  note: MIRAGE_PATH is set but is no longer consulted; use MIRAGE_MODULES_ROOT";
            }
            return out;
        }

        void visit(const std::string &path, Program &program, SourceManager &source_manager, DiagnosticEngine &diagnostics, const SearchRoots &roots) {
            // try_emplace already reports whether the key was present, so a preceding
            // contains() was a second hash+probe for the same answer.
            auto [it, inserted] = program.modules.try_emplace(path);
            if (!inserted) {
                return;
            }

            it->second = load_and_parse(it->first, source_manager, diagnostics, program);

            for (auto &[import_str, import_location] : find_import_strings(program.modules[path])) {
                auto resolved = resolve_import_path(path, import_str, roots);
                if (resolved.path.empty()) {
                    diagnostics.report_error(
                        DiagnosticStage::Parser, import_location,
                        std::format("cannot resolve import path '{}' from '{}'{}",
                                    import_str, path, describe_search_roots(path, roots)));

                    continue;
                }

                program.module_imports[path][import_str] = resolved.path;
                program.module_search_trace.push_back(ModuleSearchRecord{
                    .importer = path,
                    .import_path = import_str,
                    .resolved_path = resolved.path,
                    .root_label = resolved.root_label,
                });

                visit(resolved.path, program, source_manager, diagnostics, roots);
            }
        }

        // A directory that must exist to be usable as a search root. Returns the canonical
        // form, or an empty string (the "root unavailable" signal) if it is missing or is
        // not a directory. Never diagnoses: an absent cwd/compiler directory is normal, and
        // the explicitly-configured roots are validated by their own callers.
        auto usable_root(const std::string &path) -> std::string {
            if (path.empty()) {
                return {};
            }
            auto canonical = canonicalize(path);
            if (canonical.empty() || !std::filesystem::is_directory(canonical)) {
                return {};
            }
            return canonical;
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

    auto executable_directory(const char *argv0) -> std::string {
        // /proc/self/exe is the authoritative answer on Linux and is unaffected by how the
        // process was invoked (a bare name found on $PATH, a relative path, an exec with a
        // rewritten argv[0]).
        std::error_code ec;
        if (auto exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) {
            return exe.parent_path().string();
        }

        if (argv0 == nullptr || *argv0 == '\0') {
            return {};
        }

        const std::filesystem::path invoked(argv0);
        if (invoked.has_parent_path()) {
            auto canonical = canonicalize(invoked.parent_path().string());
            return std::filesystem::is_directory(canonical) ? canonical : std::string{};
        }

        // A bare name: walk $PATH the way the shell that started us did.
        const char *path_env = std::getenv("PATH");
        if (path_env == nullptr) {
            return {};
        }
        std::string_view rest(path_env);
        while (!rest.empty()) {
            const auto sep = rest.find(':');
            const auto entry = rest.substr(0, sep);
            rest = sep == std::string_view::npos ? std::string_view{} : rest.substr(sep + 1);
            if (entry.empty()) {
                continue;
            }
            auto candidate = std::filesystem::path(entry) / invoked;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                auto canonical = canonicalize(candidate.parent_path().string());
                return std::filesystem::is_directory(canonical) ? canonical : std::string{};
            }
        }
        return {};
    }

    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics,
                 const ResolveOptions &options) -> Program {
        Program program;

        const auto canonical = canonicalize(root_module_path);
        if (canonical.empty() || !std::filesystem::is_directory(canonical)) {
            diagnostics.report_error(
                DiagnosticStage::Parser, {},
                std::format("'{}' is not a valid module directory", root_module_path));

            return program;
        }

        program.root_module_path = canonical;

        SearchRoots roots;
        roots.root_module = canonical;

        std::error_code cwd_error;
        if (const auto cwd = std::filesystem::current_path(cwd_error); !cwd_error) {
            roots.cwd = usable_root(cwd.string());
        }

        if (!options.compiler_dir.empty()) {
            roots.compiler_dir = usable_root(options.compiler_dir);
            // A 'bin/mirage' + 'lib/mirage/core/...' install: probed after the executable's
            // own directory so a development tree (where the binary sits beside nothing) is
            // unaffected, and so 'just install' needs no environment variable at all.
            roots.compiler_lib_dir =
                usable_root((std::filesystem::path(options.compiler_dir) / ".." / "lib" / "mirage").string());
            // Both probes landing on the same directory would double every failure message
            // and trace line for no information.
            if (roots.compiler_lib_dir == roots.compiler_dir) {
                roots.compiler_lib_dir.clear();
            }
        }

        // MIRAGE_PATH was the pre-2026-08 spelling and is deliberately NOT consulted; it is
        // read only to decide whether the failure diagnostic should mention that it is being
        // ignored. See describe_search_roots.
        if (const char *legacy = std::getenv("MIRAGE_PATH"); legacy != nullptr && *legacy != '\0') {
            roots.legacy_mirage_path_set = true;
        }

        if (!options.std_path_override.empty()) {
            auto candidate = usable_root(options.std_path_override);
            if (candidate.empty()) {
                diagnostics.report_error(
                    DiagnosticStage::Parser, {},
                    std::format("'{}' is not a valid standard library directory", options.std_path_override));

                return program;
            }
            roots.modules_root = std::move(candidate);
        } else if (const char *env_value = std::getenv("MIRAGE_MODULES_ROOT"); env_value != nullptr && *env_value != '\0') {
            auto candidate = usable_root(env_value);
            if (!candidate.empty()) {
                roots.modules_root = std::move(candidate);
            } else {
                // Warn rather than error, mirroring '--std=' above but one severity lower: an
                // explicit flag naming a bad directory is a mistake in this invocation, whereas
                // a stale MIRAGE_MODULES_ROOT in the environment should not stop a build that
                // does not use the standard library at all. Silence was the wrong answer either
                // way -- every later 'import("std")' then failed with a generic "cannot resolve
                // import path" that gave no hint the real cause was the environment variable.
                diagnostics.warn(
                    DiagnosticStage::Parser, {},
                    std::format("MIRAGE_MODULES_ROOT is set to '{}', which is not a directory; "
                                "standard-library imports will not resolve", env_value));
            }
        }

        visit(canonical, program, source_manager, diagnostics, roots);

        program.ok = !diagnostics.has_errors();

        return program;
    }
}
