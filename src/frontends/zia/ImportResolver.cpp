//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/ImportResolver.cpp
// Purpose: Resolve Zia file binds recursively, parse imported modules, and
//          merge their declarations and transitive bind metadata.
// Key invariants:
//   * Normalized paths identify processed and in-progress files.
//   * Circular re-entry is skipped while the outer traversal completes.
//   * Imported source is bounded before allocation and assigned a SourceManager
//     file ID before lexing.
// Ownership/Lifetime:
//   - Parsed modules and copied sources are owned until declarations move to the root AST.
//   - Diagnostics, source management, and provider callbacks are borrowed.
// Links: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Implementation of the Zia import resolver.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/ImportResolver.hpp"
#include "common/Filesystem.hpp"
#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/Parser.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace il::frontends::zia {

namespace {

/// @brief Maximum imported source bytes accepted from disk before parsing.
/// @details Keeps accidental or malicious huge imports from forcing a large allocation.
constexpr std::uintmax_t kMaxImportedSourceBytes = 64ull * 1024ull * 1024ull;

} // namespace

/// @brief Construct an import resolver.
/// @param diag Diagnostic engine for reporting import errors.
/// @param sm Source manager that owns file ids and source text.
/// @param warningSuppressions Optional per-file warning-suppression scanner (may be null).
/// @param sourceProvider Optional callback returning in-memory source for a path (e.g. unsaved
///        editor buffers); when it returns a value the on-disk file is not read.
ImportResolver::ImportResolver(
    il::support::DiagnosticEngine &diag,
    il::support::SourceManager &sm,
    WarningSuppressions *warningSuppressions,
    std::function<std::optional<std::string>(std::string_view)> sourceProvider)
    : diag_(diag), sm_(sm), warningSuppressions_(warningSuppressions),
      sourceProvider_(std::move(sourceProvider)) {}

/// @brief Resolve all file imports for a root module, merging imported declarations in.
/// @param module The root module AST (modified in place: imported decls are prepended).
/// @param modulePath Filesystem path of the root module.
/// @return True on success; false if any import failed (errors are reported via the diag engine).
/// @details Resets all resolver state, seeds the path→file-id/module-name maps with the root,
///          then recursively processes its binds via processModule().
bool ImportResolver::resolve(ModuleDecl &module, const std::string &modulePath) {
    processedFiles_.clear();
    inProgressFiles_.clear();
    importStack_.clear();
    fileIdsByPath_.clear();
    moduleNamesByPath_.clear();
    fileIdsByPath_[normalizePath(modulePath)] = module.loc.file_id;
    moduleNamesByPath_[normalizePath(modulePath)] = module.name;
    return processModule(module, modulePath, il::support::SourceLoc{}, 0);
}

/// @brief Normalize a path to a stable, absolute, lexically-normalized form.
/// @param path Input path in UTF-8 spelling.
/// @return The canonical path string used as the dedup/cache key for files.
std::string ImportResolver::normalizePath(const std::string &path) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path nativePath = zanna::filesystem::pathFromUtf8(path);
    fs::path absolute = fs::absolute(nativePath, ec);
    if (ec)
        absolute = nativePath;
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    if (!ec)
        return zanna::filesystem::pathToUtf8(canonical.lexically_normal());
    return zanna::filesystem::pathToUtf8(absolute.lexically_normal());
}

/// @brief Resolve an import path relative to the importing file.
/// @param importPath The path as written in the `bind` statement.
/// @param importingFile Path of the file containing the import.
/// @return The resolved path, defaulting to a `.zia` extension when none is given. Absolute
///         import paths are used as-is; relative paths are resolved against the importing dir.
std::string ImportResolver::resolveImportPath(const std::string &importPath,
                                              const std::string &importingFile) const {
    namespace fs = std::filesystem;

    fs::path importingDir = zanna::filesystem::pathFromUtf8(importingFile).parent_path();
    if (importingDir.empty())
        importingDir = ".";

    fs::path importP = zanna::filesystem::pathFromUtf8(importPath);
    fs::path resolved = importP.is_absolute() ? importP : (importingDir / importP);

    if (resolved.extension().empty())
        resolved += ".zia";

    return zanna::filesystem::pathToUtf8(resolved.lexically_normal());
}

/// @brief Read, lex, and parse an imported file into a module AST.
/// @param path Filesystem path of the file to parse.
/// @param importLoc Location of the importing `bind` (for error reporting).
/// @return The parsed module, or nullptr on read/parse error (reported via the diag engine).
/// @details Source text comes from the @c sourceProvider_ callback when available, otherwise a
///          process-wide mtime+size-keyed cache, otherwise a fresh disk read (then cached). The
///          file is registered with the SourceManager, scanned for warning suppressions, and
///          parsed; a SourceManager file-id overflow is reported as an error.
std::unique_ptr<ModuleDecl> ImportResolver::parseFile(const std::string &path,
                                                      il::support::SourceLoc importLoc) {
    struct CachedSource {
        std::filesystem::file_time_type stamp{};
        std::uintmax_t size{0};
        std::string source;
    };

    static std::unordered_map<std::string, CachedSource> sourceCache;
    static std::mutex sourceCacheMutex;
    static constexpr size_t kMaxCachedSources = 256;

    const std::string normalized = normalizePath(path);
    std::error_code stampEc;
    std::error_code sizeEc;
    const std::filesystem::path nativePath = zanna::filesystem::pathFromUtf8(path);
    const auto stamp = std::filesystem::last_write_time(nativePath, stampEc);
    const auto fileSize = std::filesystem::file_size(nativePath, sizeEc);
    const bool canCache = !stampEc && !sizeEc;

    std::string source;
    bool cacheHit = false;
    if (sourceProvider_) {
        if (auto provided = sourceProvider_(normalized)) {
            source = std::move(*provided);
            if (source.size() > kMaxImportedSourceBytes) {
                diag_.report({il::support::Severity::Error,
                              "Imported file is too large: " + path,
                              importLoc,
                              "V1000"});
                return nullptr;
            }
            cacheHit = true;
        }
    }

    if (!cacheHit && canCache) {
        std::lock_guard<std::mutex> lock(sourceCacheMutex);
        auto it = sourceCache.find(normalized);
        if (it != sourceCache.end() && it->second.stamp == stamp && it->second.size == fileSize) {
            source = it->second.source;
            cacheHit = true;
        }
    }

    if (!cacheHit) {
        std::ifstream file(nativePath, std::ios::binary | std::ios::ate);
        if (!file) {
            diag_.report({il::support::Severity::Error,
                          "Failed to open imported file: " + path,
                          importLoc,
                          "V1000"});
            return nullptr;
        }

        const auto size = file.tellg();
        file.seekg(0);
        if (size < 0) {
            diag_.report({il::support::Severity::Error,
                          "Failed to determine imported file size: " + path,
                          importLoc,
                          "V1000"});
            return nullptr;
        }
        const auto sourceSize = static_cast<std::uintmax_t>(static_cast<std::streamoff>(size));
        if (sourceSize > kMaxImportedSourceBytes) {
            diag_.report({il::support::Severity::Error,
                          "Imported file is too large: " + path,
                          importLoc,
                          "V1000"});
            return nullptr;
        }
        source.resize(static_cast<std::size_t>(sourceSize));
        if (sourceSize > 0)
            file.read(source.data(), static_cast<std::streamsize>(sourceSize));
        if (!file) {
            diag_.report({il::support::Severity::Error,
                          "Failed to read imported file: " + path,
                          importLoc,
                          "V1000"});
            return nullptr;
        }
        if (canCache) {
            std::lock_guard<std::mutex> lock(sourceCacheMutex);
            if (sourceCache.size() >= kMaxCachedSources)
                sourceCache.clear();
            sourceCache[normalized] = CachedSource{stamp, fileSize, source};
        }
    }

    uint32_t fileId = sm_.addFile(path);
    if (fileId == 0) {
        diag_.report({il::support::Severity::Error,
                      std::string{il::support::kSourceManagerFileIdOverflowMessage},
                      importLoc,
                      "V-SRC-FILE-ID"});
        return nullptr;
    }
    sm_.setSource(fileId, source);
    fileIdsByPath_[normalized] = fileId;
    if (warningSuppressions_)
        warningSuppressions_->scan(fileId, source);
    Lexer lexer(source, fileId, diag_);
    Parser parser(lexer, diag_);

    auto module = parser.parseModule();
    if (!module || parser.hasError())
        return nullptr;
    moduleNamesByPath_[normalized] = module->name;
    return module;
}

/// @brief Report a circular-import error, including the offending import chain.
/// @param importLoc Location of the import that closes the cycle.
/// @param normalizedImportPath Normalized path of the file being re-entered.
/// @details Reconstructs the chain from @c importStack_ (from the first occurrence of the path
///          to the current top, plus the repeated path) for a readable `a -> b -> a` message.
void ImportResolver::reportCycle(il::support::SourceLoc importLoc,
                                 const std::string &normalizedImportPath) {
    std::string message = "Circular import detected";

    auto it = std::find(importStack_.begin(), importStack_.end(), normalizedImportPath);
    if (it != importStack_.end()) {
        std::string chain;
        for (auto iter = it; iter != importStack_.end(); ++iter) {
            if (!chain.empty())
                chain += " -> ";
            chain += *iter;
        }
        if (!chain.empty())
            chain += " -> " + normalizedImportPath;
        message += ": " + chain;
    }

    diag_.report({il::support::Severity::Error, message, importLoc, "V1000"});
}

/// @brief Recursively resolve a module's file binds and merge their declarations.
/// @param module The module being processed (modified in place).
/// @param modulePath Filesystem path of @p module.
/// @param viaImportLoc Location of the bind that pulled this module in (for errors).
/// @param depth Current recursion depth, bounded by kMaxImportDepth.
/// @return True on success; false on error (depth/file-count limits, parse failure).
/// @details Tracks processed/in-progress files so circular binds are skipped at the import
///          that closes the cycle. Each unprocessed file bind is parsed and processed depth-first;
///          its transitive binds are propagated to @p module (deduplicated, with paths normalized
///          to absolute) so qualified-name resolution works, and its declarations are prepended in
///          import order.
bool ImportResolver::processModule(ModuleDecl &module,
                                   const std::string &modulePath,
                                   il::support::SourceLoc viaImportLoc,
                                   size_t depth) {
    if (depth > kMaxImportDepth) {
        diag_.report({il::support::Severity::Error,
                      "Import depth exceeds maximum (50). Check for circular imports.",
                      viaImportLoc,
                      "V1000"});
        return false;
    }

    std::string normalizedPath = normalizePath(modulePath);
    if (processedFiles_.count(normalizedPath) != 0)
        return true;

    if (inProgressFiles_.count(normalizedPath) != 0)
        return true;

    if (processedFiles_.size() + inProgressFiles_.size() >= kMaxImportedFiles) {
        diag_.report({il::support::Severity::Error,
                      "Too many imported files (limit: " + std::to_string(kMaxImportedFiles) +
                          "). Check for import cycles.",
                      viaImportLoc,
                      "V1000"});
        return false;
    }

    inProgressFiles_.insert(normalizedPath);
    importStack_.push_back(normalizedPath);
    bool allResolved = true;

    // Collect all imported declarations first, then prepend them together.
    // This ensures proper dependency order: if A imports B then C, and C also
    // imports B (already processed), we get [B, C, A] not [C, B, A].
    std::vector<DeclPtr> importedDecls;
    std::unordered_set<std::string> seenFileBinds;
    std::unordered_set<std::string> seenNamespaceBinds;

    /// @brief Builds a deterministic deduplication key for a namespace bind.
    /// @param bind Namespace bind declaration.
    /// @return Key containing path, alias, and selected items.
    auto makeNamespaceBindKey = [](const BindDecl &bind) {
        std::string key = bind.path;
        key.push_back('\n');
        key += bind.alias;
        key.push_back('\n');
        for (const auto &item : bind.specificItems) {
            key += item;
            key.push_back('\n');
        }
        return key;
    };

    /// @brief Builds a deterministic deduplication key for a file bind.
    /// @param bind File bind declaration.
    /// @param canonicalPath Canonical imported path.
    /// @return Key containing path, source file, alias, and selected items.
    auto makeFileBindKey = [](const BindDecl &bind, const std::string &canonicalPath) {
        std::string key = canonicalPath;
        key.push_back('\n');
        key += std::to_string(bind.loc.file_id);
        key.push_back('\n');
        key += bind.alias;
        key.push_back('\n');
        for (const auto &item : bind.specificItems) {
            key += item;
            key.push_back('\n');
        }
        return key;
    };

    for (const auto &existingBind : module.binds) {
        if (existingBind.isNamespaceBind) {
            seenNamespaceBinds.insert(makeNamespaceBindKey(existingBind));
            continue;
        }

        std::string existingResolved = resolveImportPath(existingBind.path, modulePath);
        seenFileBinds.insert(makeFileBindKey(existingBind, normalizePath(existingResolved)));
    }

    // Important: Use index-based iteration because we may add transitive binds
    // to module.binds during processing. Range-based for would cause iterator
    // invalidation when the vector grows.
    for (size_t i = 0; i < module.binds.size(); ++i) {
        auto &bind = module.binds[i];

        // Skip namespace binds (e.g., "bind Zanna.Terminal;") - they are handled
        // by semantic analysis, not file resolution.
        if (bind.isNamespaceBind)
            continue;

        // An empty or blank bind path is a parse-error artifact (e.g. an
        // incomplete "bind X." captured mid-edit by live diagnostics). The
        // parser/sema already report the real error; skip it here so we never
        // fabricate a "<dir>/.zia" import or abort the rest of the module.
        if (bind.path.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;

        std::string bindFilePath = resolveImportPath(bind.path, modulePath);
        std::string normalizedBindPath = normalizePath(bindFilePath);

        if (processedFiles_.count(normalizedBindPath) != 0) {
            if (auto idIt = fileIdsByPath_.find(normalizedBindPath); idIt != fileIdsByPath_.end()) {
                bind.resolvedFileId = idIt->second;
            }
            if (auto nameIt = moduleNamesByPath_.find(normalizedBindPath);
                nameIt != moduleNamesByPath_.end()) {
                bind.resolvedModuleName = nameIt->second;
            }
            continue;
        }

        if (inProgressFiles_.count(normalizedBindPath) != 0) {
            if (auto idIt = fileIdsByPath_.find(normalizedBindPath); idIt != fileIdsByPath_.end()) {
                bind.resolvedFileId = idIt->second;
            }
            if (auto nameIt = moduleNamesByPath_.find(normalizedBindPath);
                nameIt != moduleNamesByPath_.end()) {
                bind.resolvedModuleName = nameIt->second;
            }
            continue;
        }

        auto boundModule = parseFile(bindFilePath, bind.loc);
        if (!boundModule) {
            allResolved = false;
            continue; // parseFile already reported the error; resolve remaining binds
        }

        bind.resolvedFileId = boundModule->loc.file_id;
        bind.resolvedModuleName = boundModule->name;

        if (!processModule(*boundModule, bindFilePath, bind.loc, depth + 1)) {
            allResolved = false;
            continue;
        }

        // Propagate transitive binds to the importing module.
        // This ensures semantic analysis can resolve module-qualified names
        // (e.g., if main imports game, and game imports utils, then main
        // needs to see the utils bind to resolve game's references to utils).
        // We resolve paths to absolute form to avoid re-resolution issues.
        for (const auto &transitiveBind : boundModule->binds) {
            // Namespace binds don't need path resolution - they're handled by Sema
            if (transitiveBind.isNamespaceBind) {
                if (seenNamespaceBinds.insert(makeNamespaceBindKey(transitiveBind)).second) {
                    BindDecl nsBind(transitiveBind.loc, transitiveBind.path);
                    nsBind.alias = transitiveBind.alias;
                    nsBind.isNamespaceBind = true;
                    nsBind.specificItems = transitiveBind.specificItems;
                    module.binds.push_back(nsBind);
                }
                continue;
            }

            // Resolve the transitive bind path relative to its original file
            std::string resolvedPath = resolveImportPath(transitiveBind.path, bindFilePath);
            std::string transitiveNormalizedPath = normalizePath(resolvedPath);

            if (seenFileBinds.insert(makeFileBindKey(transitiveBind, transitiveNormalizedPath))
                    .second) {
                // Store the absolute path so it resolves correctly from any context
                BindDecl absoluteBind(transitiveBind.loc, transitiveNormalizedPath);
                absoluteBind.alias = transitiveBind.alias;
                absoluteBind.isNamespaceBind = transitiveBind.isNamespaceBind;
                absoluteBind.specificItems = transitiveBind.specificItems;
                absoluteBind.resolvedFileId = transitiveBind.resolvedFileId;
                absoluteBind.resolvedModuleName = transitiveBind.resolvedModuleName;
                module.binds.push_back(absoluteBind);
            }
        }

        // Collect this bind's declarations (which include its transitive binds)
        for (auto &decl : boundModule->declarations)
            importedDecls.push_back(std::move(decl));
    }

    // Now prepend all imported declarations before this module's declarations.
    // This maintains proper dependency order: imports come first in import order.
    if (!importedDecls.empty()) {
        std::vector<DeclPtr> combined;
        combined.reserve(importedDecls.size() + module.declarations.size());
        for (auto &decl : importedDecls)
            combined.push_back(std::move(decl));
        for (auto &decl : module.declarations)
            combined.push_back(std::move(decl));
        module.declarations = std::move(combined);
    }

    importStack_.pop_back();
    inProgressFiles_.erase(normalizedPath);
    processedFiles_.insert(normalizedPath);
    return allResolved;
}

} // namespace il::frontends::zia
