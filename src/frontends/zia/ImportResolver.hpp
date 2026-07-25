//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/ImportResolver.hpp
// Purpose: Declare bounded, cycle-aware Zia file-bind resolution.
// Key invariants:
//   * Processed and in-progress normalized path sets remain disjoint.
//   * The import stack mirrors the active depth-first traversal path.
//   * A fully processed file is never parsed again by one resolver instance.
// Ownership: ImportResolver owns traversal/cache keys while borrowing
//            diagnostics, source management, optional suppression state, and
//            the optional source-provider callback's external environment.
// References: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Recursive import resolver for the Zia frontend.
///
/// @details The ImportResolver handles the `bind` statement in Zia source code.
/// When a module contains `bind SomeModule;`, the resolver locates the
/// corresponding `.zia` file, parses it into an AST, recursively resolves its
/// own imports, and prepends the imported declarations into the importing
/// module's AST. This ensures that all imported symbols (functions, types,
/// constants) are visible during semantic analysis and lowering.
///
/// The resolution algorithm uses a depth-first traversal with cycle handling:
///   1. Normalize the import path to a canonical form.
///   2. Check if the file is already fully processed or currently in-progress
///      and skip that re-entry.
///   3. Mark the file as in-progress and push it onto the import stack.
///   4. Parse the file using the Zia lexer and parser.
///   5. Recursively resolve any imports within the parsed module.
///   6. Prepend the resolved declarations into the importing module.
///   7. Mark the file as fully processed and pop the import stack.
///
/// Safety limits prevent runaway compilation: kMaxImportDepth (50) bounds
/// recursion depth and kMaxImportedFiles (256) bounds total file count.
///
/// @invariant processedFiles_ and inProgressFiles_ are disjoint at all times.
/// @invariant importStack_ mirrors the current recursion path (depth == stack size).
/// @invariant A file in processedFiles_ will never be parsed or processed again.
///
/// Ownership/Lifetime: Stack-allocated, lives for the duration of a single
/// compilation. Holds references to the shared DiagnosticEngine and
/// SourceManager which must outlive this object.
///
/// @see Compiler.hpp — orchestrates parsing, import resolution, sema, lowering.
/// @see AST.hpp — ModuleDecl and other AST node types produced by parsing.
/// @see Parser.hpp — the Zia parser invoked by parseFile().
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST.hpp"
#include "frontends/zia/WarningSuppressions.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::frontends::zia {

/// @brief Resolves and merges Zia imports via recursive file loading.
///
/// @details The resolver loads imported files recursively and prepends imported
///          declarations into the importing module, ensuring imported symbols
///          are available during semantic analysis and lowering. Circular imports
///          are deduplicated by skipping re-entry into files already on the current
///          recursion stack.
class ImportResolver {
  public:
    /// @brief Construct an ImportResolver with shared compiler infrastructure.
    /// @param diag Reference to the diagnostic engine for error/warning reporting.
    ///             Used to emit errors for missing files and depth/count limit
    ///             violations.
    /// @param sm   Reference to the source manager that tracks loaded source files.
    ///             Used to register newly-loaded import files so their content is
    ///             available for error reporting and source location mapping.
    /// @param warningSuppressions Optional accumulator scanned for imported-file
    ///                            warning directives.
    /// @param sourceProvider Optional callback that can provide an in-memory
    ///                       source snapshot instead of reading disk.
    ImportResolver(il::support::DiagnosticEngine &diag,
                   il::support::SourceManager &sm,
                   WarningSuppressions *warningSuppressions = nullptr,
                   std::function<std::optional<std::string>(std::string_view)> sourceProvider = {});

    /// @brief Resolve all imports for @p module.
    /// @details Scans the module's declaration list for import statements, resolves
    ///          each one by loading and parsing the target file, and recursively
    ///          resolves transitive imports. Resolved declarations are prepended
    ///          to the module's declaration list so they precede any code that
    ///          references them.
    /// @param module The root module AST (already parsed).
    /// @param modulePath Filesystem path of the root module (used to resolve
    ///                   relative import paths).
    /// @return True if all imports were resolved successfully, false if any import
    ///         failed (missing file, depth/count exceeded).
    bool resolve(ModuleDecl &module, const std::string &modulePath);

  private:
    /// @brief Maximum recursion depth for nested imports.
    /// @details Prevents stack overflow from deeply-chained import graphs. If an
    ///          import chain exceeds this depth, a diagnostic error is emitted.
    static constexpr size_t kMaxImportDepth = 50;

    /// @brief Maximum total number of imported files per compilation unit.
    /// @details Prevents runaway compilation from pathologically large import
    ///          graphs. Once this limit is reached, further imports are rejected.
    ///          Raised to 256 to accommodate large dogfood projects such as
    ///          Zanna Studio, which legitimately span well over 100 modules.
    static constexpr size_t kMaxImportedFiles = 256;

    /// @brief Convert a relative or symbolic import path to an absolute filesystem path.
    /// @details Takes the import string from the `bind` statement (e.g., "utils/math")
    ///          and resolves it relative to the directory containing the importing file.
    ///          Appends the `.zia` extension if not already present.
    /// @param importPath The import path as written in the source code.
    /// @param importingFile The absolute path of the file containing the import.
    /// @return The resolved absolute filesystem path to the imported module.
    std::string resolveImportPath(const std::string &importPath,
                                  const std::string &importingFile) const;

    /// @brief Normalize a filesystem path to a canonical form for deduplication.
    /// @details Makes the path absolute, applies weak canonicalization when
    ///          possible, and lexically normalizes remaining components.
    /// @param path The filesystem path to normalize.
    /// @return The normalized canonical path string.
    std::string normalizePath(const std::string &path) const;

    /// @brief Load, lex, and parse a Zia source file into a ModuleDecl AST.
    /// @details Obtains contents from the source provider, source cache, or disk;
    ///          registers them with SourceManager; runs the Zia lexer; and invokes
    ///          the Parser to build an AST.
    ///          If the file cannot be read or parsing fails, a diagnostic is emitted
    ///          at @p importLoc (the location of the `bind` statement in the
    ///          importing file).
    /// @param path Absolute filesystem path to the `.zia` file to parse.
    /// @param importLoc Source location of the import statement that triggered
    ///                  this file load (used for error reporting context).
    /// @return A unique_ptr to the parsed ModuleDecl, or nullptr on failure.
    std::unique_ptr<ModuleDecl> parseFile(const std::string &path,
                                          il::support::SourceLoc importLoc);

    /// @brief Recursively process a module's imports at the given depth.
    /// @details Core recursive function. For each import in @p module, resolves the
    ///          path, skips already-processed or currently in-progress files,
    ///          checks depth limits, parses the target file, recursively processes
    ///          its imports, and prepends its declarations.
    /// @param module The module whose imports should be resolved.
    /// @param modulePath Absolute path of @p module (for resolving relative imports).
    /// @param viaImportLoc Source location of the `bind` statement that caused this
    ///                     module to be loaded (SourceLoc() for the root module).
    /// @param depth Current recursion depth (0 for root, incremented per level).
    /// @return True if all imports at this level and below succeeded, false on error.
    bool processModule(ModuleDecl &module,
                       const std::string &modulePath,
                       il::support::SourceLoc viaImportLoc,
                       size_t depth);

    /// @brief Emit a diagnostic error for a detected circular import.
    /// @details Retained for diagnostics/debugging paths. Normal import resolution
    ///          supports circular binds by skipping re-entry, so this method is not
    ///          called for supported import cycles.
    /// @param importLoc Source location of the `bind` statement that closes the cycle.
    /// @param normalizedImportPath The normalized path of the file being re-imported.
    void reportCycle(il::support::SourceLoc importLoc, const std::string &normalizedImportPath);

    /// @brief Diagnostic engine for emitting errors, warnings, and notes.
    il::support::DiagnosticEngine &diag_;

    /// @brief Source manager for loading and tracking source file contents.
    il::support::SourceManager &sm_;

    /// @brief Optional warning suppression accumulator for imported files.
    WarningSuppressions *warningSuppressions_{nullptr};

    /// @brief Optional dirty-buffer provider used by IDE project indexes.
    std::function<std::optional<std::string>(std::string_view)> sourceProvider_{};

    /// @brief Set of fully-processed file paths (normalized).
    /// @details Files in this set have been completely parsed and their declarations
    ///          merged. They will not be processed again on subsequent imports.
    std::set<std::string> processedFiles_;

    /// @brief Set of file paths currently being processed (normalized).
    /// @details Files in this set are on the current recursion stack. If an import
    ///          targets a file in this set, the circular bind is skipped (the
    ///          in-progress file's declarations will be merged when its outer
    ///          processModule call completes).
    std::set<std::string> inProgressFiles_;

    /// @brief Stack of file paths mirroring the current recursion chain.
    /// @details The first entry is the root module, the last is the most recently
    ///          entered import. This supports cycle diagnostics in debugging paths.
    std::vector<std::string> importStack_;

    /// @brief Normalized file path to SourceManager file id.
    /// @details Tracks file ids even for already-processed and in-progress
    /// modules so later binds can preserve the imported file's identity.
    std::unordered_map<std::string, uint32_t> fileIdsByPath_;

    /// @brief Normalized file path to declared module name.
    /// @details Keeps file binds aligned with the imported file's `module`
    /// declaration instead of the source filename, which avoids collisions such
    /// as `brush.zia` and `tools/brush.zia` with different module names.
    std::unordered_map<std::string, std::string> moduleNamesByPath_;
};

} // namespace il::frontends::zia
