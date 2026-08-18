//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/asset/AssetCompiler.cpp
// Purpose: Build-time asset compilation. Resolves embed/pack directives,
//          enumerates directories, reads file data, and produces ZPAK blobs
//          and standalone .zpak pack files.
//
// Key invariants:
//   - Source paths are resolved relative to ProjectConfig::rootDir.
//   - Symlinks outside the project root are rejected.
//   - Directory entries are recursively enumerated with forward-slash names.
//   - Empty projects produce an empty AssetBundle (no error).
//
// Ownership/Lifetime:
//   - File data is read into temporary vectors and consumed by ZpakWriter.
//   - Output pack files are written to the specified output directory.
//
// Links: ZpakWriter.hpp, AssetCompiler.hpp, project_loader.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements secure, cached compilation of project asset directives.
/// @details The implementation validates project-root containment, fingerprints
///          inputs by metadata and content, bounds process-local caches, creates
///          deterministic ZPAK entries, and can wrap an embedded archive in a
///          native object file using Zanna's internal object writer.

#include "AssetCompiler.hpp"

#include "ZpakWriter.hpp"
#include "codegen/common/objfile/ObjectFileWriter.hpp"
#include "common/Filesystem.hpp"
#include "tools/common/packaging/PkgHash.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "tools/common/project_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace zanna::asset {
namespace {
// Baked VSCN stadiums can legitimately exceed 256 MiB. Keep the compiler's
// source-entry ceiling aligned with the runtime VSCN and async-loader guards.
constexpr std::uintmax_t kMaxAssetFileBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxAssetCacheEntries = 64;
constexpr std::size_t kMaxAssetFileCacheEntries = 128;
constexpr std::uintmax_t kMaxAssetFileCacheBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t kMaxAssetBundleCacheBytes = 512ULL * 1024ULL * 1024ULL;

/// @brief Cached contents of one validated asset file.
struct CachedAssetFile {
    std::uintmax_t size{0};     ///< File size at cache time.
    fs::file_time_type mtime{}; ///< Last-write time at cache time.
    std::string hash;           ///< SHA-256 hex digest of @ref data.
    std::vector<uint8_t> data;  ///< Complete file payload.
};

/// @brief Return a stable cache key for an asset path.
/// @param path Asset filesystem path to canonicalize when possible.
/// @return UTF-8 canonical path, or a lexically normalized fallback.
static std::string assetFileCacheKey(const fs::path &path) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return zanna::filesystem::pathToUtf8(ec ? path.lexically_normal() : canonical);
}

/// @brief Shared cache for asset file payloads read during one process.
/// @return Mutable process-local mapping from path keys to immutable payloads.
static std::unordered_map<std::string, std::shared_ptr<const CachedAssetFile>> &assetFileCache() {
    static std::unordered_map<std::string, std::shared_ptr<const CachedAssetFile>> cache;
    return cache;
}

/// @brief Return the total byte size currently retained by @ref assetFileCache.
/// @return Mutable process-local retained-byte counter.
static std::uintmax_t &assetFileCacheBytes() {
    static std::uintmax_t bytes = 0;
    return bytes;
}

/// @brief Mutex protecting @ref assetFileCache.
/// @return Process-local mutex shared by all asset-cache operations.
static std::mutex &assetFileCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

/// @brief Remove one arbitrary cached asset payload when the cache reaches its cap.
/// @details Updates the retained-byte counter before erasing the selected entry.
/// @pre Caller holds @ref assetFileCacheMutex.
static void evictOneAssetFileCacheEntry() {
    auto &cache = assetFileCache();
    if (!cache.empty()) {
        assetFileCacheBytes() -= cache.begin()->second->data.size();
        cache.erase(cache.begin());
    }
}

/// @brief Read and validate an asset file payload from disk.
/// @param path Validated file path to open in binary mode.
/// @param expectedSize Exact number of bytes expected from the file.
/// @return Byte vector containing the complete payload.
/// @throws std::runtime_error If the file cannot be opened or read completely.
/// @throws std::bad_alloc If storage for the expected payload cannot be allocated.
static std::vector<uint8_t> readAssetFileUncached(const fs::path &path,
                                                  std::uintmax_t expectedSize) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read asset: " + zanna::filesystem::pathToUtf8(path));
    std::vector<uint8_t> data(static_cast<size_t>(expectedSize));
    if (!data.empty())
        in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in)
        throw std::runtime_error("failed while reading asset: " +
                                 zanna::filesystem::pathToUtf8(path));
    return data;
}

/// @brief Return cached asset bytes when size and mtime still match disk.
/// @param path Asset path whose canonical cache entry should be queried.
/// @param size Current file size in bytes.
/// @param mtime Current filesystem modification time.
/// @return Shared immutable cache entry on a metadata match, otherwise null.
static std::shared_ptr<const CachedAssetFile> lookupCachedAssetFile(const fs::path &path,
                                                                    std::uintmax_t size,
                                                                    fs::file_time_type mtime) {
    std::lock_guard<std::mutex> lock(assetFileCacheMutex());
    auto it = assetFileCache().find(assetFileCacheKey(path));
    if (it == assetFileCache().end() || it->second->size != size || it->second->mtime != mtime)
        return {};
    return it->second;
}

/// @brief Store asset bytes and hash for later payload writing.
/// @param path Asset path used to derive the cache key.
/// @param size Validated source size.
/// @param mtime Validated source modification time.
/// @param hash SHA-256 digest of @p data.
/// @param data Complete payload transferred into the cache.
/// @details Replaces an existing path entry and evicts arbitrary entries until
///          both count and retained-byte bounds permit the new payload.
static void rememberCachedAssetFile(const fs::path &path,
                                    std::uintmax_t size,
                                    fs::file_time_type mtime,
                                    std::string hash,
                                    std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(assetFileCacheMutex());
    auto &cache = assetFileCache();
    const std::string key = assetFileCacheKey(path);
    if (auto old = cache.find(key); old != cache.end()) {
        assetFileCacheBytes() -= old->second->data.size();
        cache.erase(old);
    }
    while ((!cache.empty() && cache.size() >= kMaxAssetFileCacheEntries) ||
           (!cache.empty() && assetFileCacheBytes() + data.size() > kMaxAssetFileCacheBytes)) {
        evictOneAssetFileCacheEntry();
    }
    auto entry = std::make_shared<CachedAssetFile>(
        CachedAssetFile{size, mtime, std::move(hash), std::move(data)});
    assetFileCacheBytes() += entry->data.size();
    cache[key] = std::move(entry);
}

/// @brief Hash the full contents of @p path for cache invalidation.
/// @details Asset cache keys must change even when a filesystem preserves size and
///          coarse modification timestamps across an edit. The helper is used only
///          after source validation has enforced the asset size cap, so reading the
///          file here does not introduce an unbounded allocation.
/// @param path Validated asset file to fingerprint.
/// @return Lowercase SHA-256 hexadecimal digest of the complete payload.
static std::string contentHashForFile(const fs::path &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
        throw std::runtime_error("cannot stat asset for cache key: " +
                                 zanna::filesystem::pathToUtf8(path));
    if (size > kMaxAssetFileBytes)
        throw std::runtime_error("asset file too large for cache key: " +
                                 zanna::filesystem::pathToUtf8(path));
    const auto mtime = fs::last_write_time(path, ec);
    if (ec)
        throw std::runtime_error("cannot stat asset mtime for cache key: " +
                                 zanna::filesystem::pathToUtf8(path));
    if (auto cached = lookupCachedAssetFile(path, size, mtime))
        return cached->hash;
    auto data = readAssetFileUncached(path, size);
    std::string hash = data.empty() ? zanna::pkg::sha256Hex(nullptr, 0)
                                    : zanna::pkg::sha256Hex(data.data(), data.size());
    rememberCachedAssetFile(path, size, mtime, hash, std::move(data));
    return hash;
}

/// @brief Append a single file's identity fingerprint to a cache key.
/// @details Folds the normalized path, byte size, and last-write-time ticks of
///          @p path into @p key (using "?" placeholders when stat calls fail), then
///          includes a SHA-256 content hash so edits with unchanged size/mtime still
///          invalidate the asset build cache.
/// @param path File to fingerprint.
/// @param key Cache-key string accumulated in place.
static void appendFileFingerprint(const fs::path &path, std::string &key) {
    std::error_code ec;
    key += zanna::filesystem::pathToUtf8(path.lexically_normal());
    key.push_back('|');
    const auto size = fs::file_size(path, ec);
    key += ec ? std::string{"?"} : std::to_string(size);
    key.push_back('|');
    if (!ec) {
        ec.clear();
        const auto ticks = fs::last_write_time(path, ec).time_since_epoch().count();
        if (!ec)
            key += std::to_string(static_cast<long long>(ticks));
    }
    key.push_back('|');
    key += contentHashForFile(path);
    key.push_back('\n');
}

/// @brief Append a source directive's fingerprint (file or directory) to a key.
/// @details Resolves @p sourcePath against @p rootDir, prefixes the compression
///          mode ("C:"/"U:"), and fingerprints either the single file or, for a
///          directory, every regular file under it using the same safe traversal
///          helper as archive writing so the key and output observe identical
///          symlink/escape decisions.
/// @param rootDir Absolute project root used to resolve @p sourcePath.
/// @param sourcePath Project-relative source path from an embed/pack directive.
/// @param compressed Whether the entry is to be DEFLATE-compressed.
/// @param key Cache-key string accumulated in place.
static void appendSourceFingerprint(const fs::path &rootDir,
                                    const std::string &sourcePath,
                                    bool compressed,
                                    std::string &key) {
    fs::path absPath =
        zanna::pkg::resolvePackageSourcePath(rootDir, sourcePath, "asset source path");
    key += compressed ? "C:" : "U:";
    std::error_code ec;
    if (fs::is_directory(absPath, ec)) {
        std::vector<fs::path> files;
        /// @brief Collect each regular file resolved beneath the asset directory.
        /// @param entry Validated directory entry.
        zanna::pkg::safeDirectoryIterateResolved(
            absPath, rootDir, [&](const zanna::pkg::SafeDirectoryEntry &entry) {
                if (entry.regularFile)
                    files.push_back(entry.resolvedPath);
            });
        /// Order paths by portable UTF-8 spelling for deterministic fingerprints.
        /// @param a First path.
        /// @param b Second path.
        /// @return `true` when @p a has the earlier portable spelling.
        std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) {
            return zanna::filesystem::genericPathToUtf8(a) <
                   zanna::filesystem::genericPathToUtf8(b);
        });
        for (const auto &file : files)
            appendFileFingerprint(file, key);
        return;
    }
    if (ec)
        throw std::runtime_error("cannot inspect asset source path '" + sourcePath +
                                 "': " + ec.message());
    appendFileFingerprint(absPath, key);
}

/// @brief Compute a content-addressed cache key for an asset compilation.
/// @details Combines the project root, output directory, and the fingerprints of
///          every embed entry and pack group (including pack name and
///          compression flag). Two calls with identical inputs and unchanged
///          files on disk produce the same key, allowing compileAssets() to
///          reuse a cached AssetBundle.
/// @param config Project configuration providing embed/pack directives.
/// @param outputDir Directory where pack files would be written.
/// @return A string uniquely identifying this asset build.
static std::string assetCacheKey(const il::tools::common::ProjectConfig &config,
                                 const std::string &outputDir) {
    const fs::path rootDir = zanna::filesystem::pathFromUtf8(config.rootDir);
    std::string key =
        zanna::filesystem::pathToUtf8(rootDir.lexically_normal()) + "\n" + outputDir + "\n";
    for (const auto &entry : config.embedAssets)
        appendSourceFingerprint(rootDir, entry.sourcePath, false, key);
    for (const auto &group : config.packGroups) {
        key += "PACK:" + group.name + ":" + (group.compressed ? "1" : "0") + "\n";
        for (const auto &src : group.sources)
            appendSourceFingerprint(rootDir, src, group.compressed, key);
    }
    return key;
}

/// @brief Estimate retained memory for an AssetBundle stored in the process cache.
/// @param bundle Cached bundle whose owned dynamic payloads should be counted.
/// @return Approximate retained bytes excluding container/object overhead.
static std::uintmax_t retainedBytesForBundle(const AssetBundle &bundle) {
    std::uintmax_t bytes = bundle.embeddedBlob.size();
    for (const auto &path : bundle.packFilePaths)
        bytes += path.size();
    for (const auto &hash : bundle.packFileHashes)
        bytes += hash.size();
    bytes += bundle.packFileSizes.size() * sizeof(std::uintmax_t);
    return bytes;
}

/// @brief Return the total retained bytes for cached AssetBundle values.
/// @return Mutable process-local retained-byte counter.
static std::uintmax_t &assetBundleCacheBytes() {
    static std::uintmax_t bytes = 0;
    return bytes;
}

/// @brief Compute a SHA-256 hash for an already-generated pack file.
/// @details Pack files are expected to be much smaller than the process memory
///          limit enforced by asset validation. Reading here keeps cache
///          validation self-contained and catches same-size external rewrites.
/// @param path Generated pack path to read and hash.
/// @return Lowercase SHA-256 hexadecimal digest of the complete pack.
static std::string hashGeneratedPackFile(const fs::path &path) {
    const auto data = zanna::pkg::readFile(path);
    return data.empty() ? zanna::pkg::sha256Hex(nullptr, 0)
                        : zanna::pkg::sha256Hex(data.data(), data.size());
}

/// @brief Return true when asset compilation should print progress messages.
/// @return True when `ZANNA_ASSET_VERBOSE` is set to a nonempty value other than `"0"`.
static bool assetVerboseEnabled() {
    const char *value = std::getenv("ZANNA_ASSET_VERBOSE");
    return value && value[0] != '\0' && std::string_view(value) != "0";
}

} // namespace

// ─── File reading helper ────────────────────────────────────────────────────

/// @brief Read an entire file into a byte vector.
/// @param path Validated asset file to read.
/// @param out Destination receiving the complete bytes after a successful read.
/// @param err Receives a human-readable failure reason.
/// @return True on success; false with @p err populated on failure.
/// @details Enforces the per-file size cap and reuses a metadata-validated
///          payload cache when possible.
/// @throws std::bad_alloc If digest or cache bookkeeping allocation fails after
///         the bounded file read has completed.
static bool readFile(const fs::path &path, std::vector<uint8_t> &out, std::string &err) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        err = "cannot determine size of: " + zanna::filesystem::pathToUtf8(path);
        return false;
    }
    if (size > kMaxAssetFileBytes) {
        err = "asset file too large: " + zanna::filesystem::pathToUtf8(path) + " (limit: 512 MB)";
        return false;
    }
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) {
        err = "cannot determine modification time of: " + zanna::filesystem::pathToUtf8(path);
        return false;
    }
    if (auto cached = lookupCachedAssetFile(path, size, mtime)) {
        out = cached->data;
        return true;
    }
    try {
        out = readAssetFileUncached(path, size);
    } catch (const std::bad_alloc &) {
        err = "out of memory reading asset: " + zanna::filesystem::pathToUtf8(path);
        return false;
    } catch (const std::exception &ex) {
        err = ex.what();
        return false;
    }
    std::string hash = out.empty() ? zanna::pkg::sha256Hex(nullptr, 0)
                                   : zanna::pkg::sha256Hex(out.data(), out.size());
    rememberCachedAssetFile(path, size, mtime, std::move(hash), out);
    return true;
}

// ─── Directory enumeration ──────────────────────────────────────────────────

/// @brief Collect all files under a directory recursively.
/// @param dir       Absolute path to directory.
/// @param rootDir   Project root for computing relative names.
/// @param entries   Output: pairs of (relative name, absolute path).
/// @param err       Set on error.
/// @return true on success.
static bool enumerateDir(const fs::path &dir,
                         const fs::path &rootDir,
                         std::vector<std::pair<std::string, fs::path>> &entries,
                         std::string &err) {
    try {
        /// @brief Append one validated regular file to the archive-entry list.
        /// @param entry Validated directory entry.
        zanna::pkg::safeDirectoryIterateResolved(
            dir, rootDir, [&](const zanna::pkg::SafeDirectoryEntry &entry) {
                if (!entry.regularFile)
                    return;
                std::error_code ec;
                fs::path relPath = fs::relative(entry.logicalPath, dir, ec);
                if (ec) {
                    throw std::runtime_error("cannot compute relative path for: " +
                                             zanna::filesystem::pathToUtf8(entry.logicalPath));
                }
                entries.push_back(
                    {zanna::filesystem::genericPathToUtf8(relPath), entry.resolvedPath});
            });
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    /// Sort archive names to make recursive directory output deterministic.
    /// @param lhs First name/path pair.
    /// @param rhs Second name/path pair.
    /// @return `true` when @p lhs has the earlier archive name.
    std::sort(entries.begin(), entries.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });
    return true;
}

/// @brief Sanitize and resolve a project-relative asset source path.
/// @details Produces both the sanitized relative path (rejecting traversal/absolute
///          components) and the absolute on-disk path, throwing-to-error from the
///          packaging path helpers so callers receive a uniform failure string.
/// @param sourcePath Project-relative source path from a directive.
/// @param rootDir Absolute project root used for resolution.
/// @param resolvedPath Output: absolute resolved filesystem path.
/// @param cleanPath Output: sanitized relative path (forward-slash form).
/// @param err Set to the failure reason when resolution is rejected.
/// @return true on success; false with @p err set on rejection.
static bool resolveAssetSourcePath(const std::string &sourcePath,
                                   const fs::path &rootDir,
                                   fs::path &resolvedPath,
                                   std::string &cleanPath,
                                   std::string &err) {
    try {
        cleanPath = zanna::pkg::sanitizePackageRelativePath(sourcePath, "asset source path");
        resolvedPath =
            zanna::pkg::resolvePackageSourcePath(rootDir, sourcePath, "asset source path");
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

/// @brief Validate every embed entry and pack group before compilation.
/// @details Resolves each embed and pack source path (rejecting unsafe paths) and
///          validates each pack group name via normalizeExecName so failures are
///          reported up front rather than partway through writing output.
/// @param config Project configuration to validate.
/// @param err Set to the first validation failure encountered.
/// @return true when all sources and pack names are valid; false otherwise.
static bool validateAssetSources(const il::tools::common::ProjectConfig &config, std::string &err) {
    const fs::path rootDir = zanna::filesystem::pathFromUtf8(config.rootDir);
    fs::path resolvedPath;
    std::string cleanPath;
    for (const auto &entry : config.embedAssets) {
        if (!resolveAssetSourcePath(entry.sourcePath, rootDir, resolvedPath, cleanPath, err))
            return false;
    }
    for (const auto &group : config.packGroups) {
        try {
            (void)zanna::pkg::normalizeExecName(group.name);
        } catch (const std::exception &e) {
            err = std::string("invalid asset pack name '") + group.name + "': " + e.what();
            return false;
        }
        for (const auto &src : group.sources) {
            if (!resolveAssetSourcePath(src, rootDir, resolvedPath, cleanPath, err))
                return false;
        }
    }
    return true;
}

// ─── Add entries to a ZpakWriter ─────────────────────────────────────────────

/// @brief Resolve a source path (file or dir) and add entries to a ZpakWriter.
/// @param sourcePath  Path relative to project root.
/// @param rootDir     Absolute project root.
/// @param writer      ZPAK writer to add entries to.
/// @param compress    Whether to compress entries.
/// @param err         Set on error.
/// @return true on success.
static bool addSourceToWriter(const std::string &sourcePath,
                              const fs::path &rootDir,
                              ZpakWriter &writer,
                              bool compress,
                              std::string &err) {
    fs::path absPath;
    std::string cleanPath;
    if (!resolveAssetSourcePath(sourcePath, rootDir, absPath, cleanPath, err))
        return false;

    std::error_code ec;
    if (!fs::exists(absPath, ec)) {
        err = "asset source not found: " + sourcePath + " (resolved to " +
              zanna::filesystem::pathToUtf8(absPath) + ")";
        return false;
    }

    if (fs::is_directory(absPath, ec)) {
        // Enumerate directory recursively.
        std::vector<std::pair<std::string, fs::path>> dirEntries;
        if (!enumerateDir(absPath, rootDir, dirEntries, err))
            return false;
        for (const auto &[name, filePath] : dirEntries) {
            std::vector<uint8_t> data;
            if (!readFile(filePath, data, err))
                return false;
            try {
                writer.addEntry(name, data.data(), data.size(), compress);
            } catch (const std::exception &e) {
                err = e.what();
                return false;
            }
        }
    } else {
        // Single file.
        std::vector<uint8_t> data;
        if (!readFile(absPath, data, err))
            return false;
        // Use the sourcePath as the asset name (forward slashes).
        try {
            writer.addEntry(
                zanna::filesystem::genericPathToUtf8(zanna::filesystem::pathFromUtf8(cleanPath)),
                data.data(),
                data.size(),
                compress);
        } catch (const std::exception &e) {
            err = e.what();
            return false;
        }
    }

    return true;
}

// ─── compileAssets ──────────────────────────────────────────────────────────

/// @brief Compile a project's embed/pack directives into an AssetBundle.
/// @details Validates all sources first, then consults a process-local cache
///          keyed by assetCacheKey() and guarded by a static mutex: a hit whose
///          pack files still match their recorded sizes and hashes is returned
///          without rebuilding. On a miss, embed directives are gathered into a
///          single in-memory ZPAK blob and each pack group is written to
///          `<outputDir>/<project>-<pack>.zpak`; the bounded result cache is then
///          updated before returning.
/// @param config Validated project configuration supplying asset directives.
/// @param outputDir Directory in which standalone pack files are created.
/// @param err Receives the first validation, I/O, hashing, or serialization error.
/// @return Compiled bundle on success, or std::nullopt on failure.
std::optional<AssetBundle> compileAssets(const il::tools::common::ProjectConfig &config,
                                         const std::string &outputDir,
                                         std::string &err) {
    if (!validateAssetSources(config, err))
        return std::nullopt;

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, AssetBundle> cache;
    std::string cacheKey;
    try {
        cacheKey = assetCacheKey(config, outputDir);
    } catch (const std::exception &ex) {
        err = ex.what();
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = cache.find(cacheKey); it != cache.end()) {
            bool packsExist = true;
            if (it->second.packFileSizes.size() != it->second.packFilePaths.size())
                packsExist = false;
            if (it->second.packFileHashes.size() != it->second.packFilePaths.size())
                packsExist = false;
            for (size_t i = 0; packsExist && i < it->second.packFilePaths.size(); ++i) {
                const auto &pack = it->second.packFilePaths[i];
                const fs::path packPath = zanna::filesystem::pathFromUtf8(pack);
                std::error_code ec;
                if (!fs::exists(packPath, ec) || ec) {
                    packsExist = false;
                    break;
                }
                const auto size = fs::file_size(packPath, ec);
                if (ec || size != it->second.packFileSizes[i]) {
                    packsExist = false;
                    break;
                }
                try {
                    if (hashGeneratedPackFile(packPath) != it->second.packFileHashes[i]) {
                        packsExist = false;
                        break;
                    }
                } catch (const std::exception &) {
                    packsExist = false;
                    break;
                }
            }
            if (packsExist)
                return it->second;
        }
    }

    AssetBundle bundle;
    const fs::path rootDir = zanna::filesystem::pathFromUtf8(config.rootDir);

    // ── 1. Process embed directives → ZPAK blob for .rodata ──

    if (!config.embedAssets.empty()) {
        ZpakWriter embedWriter;

        for (const auto &entry : config.embedAssets) {
            if (!addSourceToWriter(entry.sourcePath, rootDir, embedWriter, false, err))
                return std::nullopt;
        }

        if (embedWriter.entryCount() > 0) {
            bundle.embeddedBlob = embedWriter.writeToMemory();
            if (assetVerboseEnabled()) {
                std::cerr << "  embedded " << embedWriter.entryCount() << " asset(s) ("
                          << bundle.embeddedBlob.size() << " bytes)\n";
            }
        }
    }

    // ── 2. Process pack groups → .zpak files ──

    for (const auto &group : config.packGroups) {
        ZpakWriter packWriter;

        for (const auto &src : group.sources) {
            if (!addSourceToWriter(src, rootDir, packWriter, group.compressed, err))
                return std::nullopt;
        }

        if (packWriter.entryCount() == 0)
            continue;

        std::string safeProjectName;
        std::string safeGroupName;
        try {
            safeProjectName = zanna::pkg::normalizeExecName(config.name);
            safeGroupName = zanna::pkg::normalizeExecName(group.name);
        } catch (const std::exception &e) {
            err = e.what();
            return std::nullopt;
        }

        // Output path: <outputDir>/<projectName>-<packName>.zpak
        std::string zpakName = safeProjectName + "-" + safeGroupName + ".zpak";
        const fs::path zpakPath =
            zanna::filesystem::pathFromUtf8(outputDir) / zanna::filesystem::pathFromUtf8(zpakName);
        const std::string zpakPathUtf8 = zanna::filesystem::pathToUtf8(zpakPath);

        if (!packWriter.writeToFile(zpakPathUtf8, err))
            return std::nullopt;

        bundle.packFilePaths.push_back(zpakPathUtf8);
        std::error_code sizeEc;
        const auto zpakSize = fs::file_size(zpakPath, sizeEc);
        if (sizeEc) {
            err = "cannot stat generated asset pack: " + zpakPathUtf8;
            return std::nullopt;
        }
        bundle.packFileSizes.push_back(zpakSize);
        try {
            bundle.packFileHashes.push_back(hashGeneratedPackFile(zpakPath));
        } catch (const std::exception &ex) {
            err = "cannot hash generated asset pack: " + std::string(ex.what());
            return std::nullopt;
        }
        if (assetVerboseEnabled())
            std::cerr << "  packed " << packWriter.entryCount() << " asset(s) into " << zpakName
                      << "\n";
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const std::uintmax_t bundleBytes = retainedBytesForBundle(bundle);
        if (auto old = cache.find(cacheKey); old != cache.end()) {
            assetBundleCacheBytes() -= retainedBytesForBundle(old->second);
            cache.erase(old);
        }
        while (!cache.empty() &&
               (cache.size() >= kMaxAssetCacheEntries ||
                assetBundleCacheBytes() + bundleBytes > kMaxAssetBundleCacheBytes)) {
            /// Evict the lexicographically smallest cache key deterministically.
            /// @param lhs First cache entry.
            /// @param rhs Second cache entry.
            /// @return `true` when @p lhs has the earlier cache key.
            auto victim =
                std::min_element(cache.begin(), cache.end(), [](const auto &lhs, const auto &rhs) {
                    return lhs.first < rhs.first;
                });
            if (victim != cache.end()) {
                assetBundleCacheBytes() -= retainedBytesForBundle(victim->second);
                cache.erase(victim);
            }
        }
        assetBundleCacheBytes() += bundleBytes;
        cache[cacheKey] = bundle;
    }
    return bundle;
}

// ─── writeAssetBlobObject ───────────────────────────────────────────────────

/// @brief Emit a native .o exposing the ZPAK blob as two .rodata symbols.
/// @details Builds an object file (via Zanna's own ObjectFileWriter for the host
///          format/arch, so no external assembler is needed) containing an empty
///          .text and a .rodata section defining `zanna_asset_blob` (the bytes)
///          and `zanna_asset_blob_size` (a uint64 length).
/// @param blob Complete ZPAK bytes to place in read-only data.
/// @param outPath Destination path for the native object file.
/// @param err Receives writer-selection or output failure details.
/// @return True after the object is written successfully; false on failure.
bool writeAssetBlobObject(const std::vector<uint8_t> &blob,
                          const std::string &outPath,
                          std::string &err) {
    using namespace zanna::codegen::objfile;

    // Create an empty .text section (no code).
    CodeSection text;

    // Create .rodata section with blob data and size symbol.
    CodeSection rodata;
    rodata.alignTo(16);
    rodata.defineSymbol("zanna_asset_blob", SymbolBinding::Global, SymbolSection::Rodata);
    rodata.emitBytes(blob.data(), blob.size());
    rodata.alignTo(8);
    rodata.defineSymbol("zanna_asset_blob_size", SymbolBinding::Global, SymbolSection::Rodata);
    rodata.emit64LE(static_cast<uint64_t>(blob.size()));

    // Write using Zanna's own object file writer for the host platform.
    auto writer = createObjectFileWriter(detectHostFormat(), detectHostArch());
    if (!writer) {
        err = "no object file writer for this platform";
        return false;
    }

    std::ostringstream errStream;
    if (!writer->write(outPath, text, rodata, errStream)) {
        err = "failed to write asset blob .o: " + errStream.str();
        return false;
    }

    return true;
}

} // namespace zanna::asset
