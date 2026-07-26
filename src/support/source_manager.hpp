//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/source_manager.hpp
// Purpose: Declares manager for source file identifiers.
// Key invariants: File ID 0 is invalid.
// Ownership/Lifetime: Manager owns file path strings.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the thread-safe registry for source paths and line text.
/// @details `SourceManager` assigns stable nonzero identifiers, normalizes paths
///          for diagnostic display, and lazily caches immutable line vectors.
///          Returned string views remain valid until manager destruction, even
///          when callers replace or invalidate the active source cache.

#pragma once

#include "source_location.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// @brief Tracks mapping from file ids to paths and source locations.
/// @invariant File id 0 is invalid.
/// @ownership Owns stored file path strings.
namespace il::support {

/// @brief Diagnostic text reported when addFile() exhausts the 32-bit id space.
inline constexpr std::string_view kSourceManagerFileIdOverflowMessage =
    "source manager exhausted file identifier space";

/// @brief Friend hook exposing SourceManager internals to white-box tests.
struct SourceManagerTestAccess;

/// @brief Maintains mappings from numeric file identifiers to paths and source text.
/// @details Registration deduplicates equivalent paths and assigns identifiers
///          monotonically from one. Line queries lazily load disk-backed files,
///          while @ref setSource supports virtual or generated input. Public
///          operations synchronize internal tables for concurrent callers.
/// @invariant Identifier zero is never assigned to a registered file.
/// @ownership Owns display paths, disk paths, active line caches, and retired
///            caches retained to preserve previously returned views.
class SourceManager {
  public:
    /// @brief Register file path @p path and return its id.
    /// @param path File system path.
    /// @return New file identifier (>0 on success, 0 on overflow).
    /// @details Returns zero when the identifier space is exhausted and refuses
    ///          to insert the file. Equivalent registered paths reuse their
    ///          existing identifier. Callers surface overflow diagnostics in
    ///          their own reporting context.
    uint32_t addFile(std::string path);

    /// @brief Retrieve path for @p file_id.
    /// @param file_id Identifier returned by addFile().
    /// @return Manager-backed display path, or an empty view for an invalid id.
    /// @note The view remains valid until this SourceManager is destroyed.
    std::string_view getPath(uint32_t file_id) const;

    /// @brief Retrieve a single source line from the given file.
    /// @param file_id 1-based file identifier returned by addFile().
    /// @param line 1-based line number.
    /// @return The source line text (without newline), or empty if unavailable.
    /// @details Lazily loads and caches file contents on first access.
    /// @note The returned view remains valid until this SourceManager is destroyed.
    std::string_view getLine(uint32_t file_id, uint32_t line) const;

    /// @brief Check whether a source line exists even if its text is empty.
    /// @param file_id 1-based file identifier returned by addFile().
    /// @param line 1-based line number.
    /// @return True when the source text for @p file_id contains @p line.
    /// @details This complements @ref getLine, whose empty string_view return value
    ///          is ambiguous between an existing blank line and a missing line.
    [[nodiscard]] bool hasLine(uint32_t file_id, uint32_t line) const;

    /// @brief Drop cached source lines for a file so the next query reloads them.
    /// @param file_id 1-based file identifier returned by addFile().
    /// @details This lets callers recover from an earlier failed disk read or pick
    ///          up updated file contents without replacing the file identifier.
    void invalidateSource(uint32_t file_id) const;

    /// @brief Cache source text for @p file_id without requiring it to exist on disk.
    /// @param file_id Identifier returned by addFile().
    /// @param source Full source text associated with the file id.
    /// @details Replaces the active immutable line cache and retires the old
    ///          cache so previously returned views do not dangle.
    void setSource(uint32_t file_id, std::string source);

  private:
    /// @brief Return whether @p file_id names a registered file.
    /// @param file_id Candidate 1-based file identifier.
    /// @return True when @p file_id is in range for the path storage.
    /// @pre Caller must hold @ref mutex_.
    [[nodiscard]] bool isRegisteredFileId(uint32_t file_id) const noexcept;

    /// @brief Return a path view without acquiring the mutex.
    /// @param file_id Registered 1-based file identifier.
    /// @return Stored display path, or empty when @p file_id is invalid.
    /// @pre Caller must hold @ref mutex_.
    [[nodiscard]] std::string_view getPathLocked(uint32_t file_id) const;

    /// @brief Ensure the line cache for @p file_id exists.
    /// @param file_id Registered 1-based file identifier.
    /// @return Shared cached line vector, or null for invalid ids.
    /// @details May perform disk I/O without holding @ref mutex_ and then publish
    ///          the resulting immutable line vector under the lock.
    [[nodiscard]] std::shared_ptr<const std::vector<std::string>> ensureLineCache(
        uint32_t file_id) const;

    /// @brief Preserve an existing cache before replacing or invalidating it.
    /// @param file_id Registered 1-based file identifier whose cache should retire.
    /// @pre Caller must hold @ref mutex_.
    /// @details Public accessors return string_view values into cached lines.  This
    ///          helper keeps old vectors alive for the manager lifetime so earlier
    ///          views do not dangle after setSource() or invalidateSource().
    void retireLineCacheLocked(uint32_t file_id) const;

    /// @brief Return the cache generation currently associated with @p file_id.
    /// @param file_id Registered 1-based file identifier.
    /// @return Monotonic generation used to reject stale asynchronous loads.
    /// @pre Caller must hold @ref mutex_.
    /// @details Disk source loading intentionally happens without holding the
    ///          manager mutex.  A concurrent invalidateSource() or setSource() can
    ///          therefore happen while a load is in flight.  The generation value
    ///          lets the loader verify, under the lock, that the source metadata it
    ///          observed before I/O is still current before publishing a cache.
    [[nodiscard]] uint64_t lineCacheGenerationLocked(uint32_t file_id) const;

    /// @brief Advance the cache generation for @p file_id.
    /// @param file_id Registered 1-based file identifier.
    /// @pre Caller must hold @ref mutex_.
    /// @details Any in-flight disk load that captured an older generation becomes
    ///          stale and will be discarded instead of overwriting newer cached
    ///          source text or a deliberate invalidation.
    void bumpLineCacheGenerationLocked(uint32_t file_id) const;

    /// Serializes access to path tables and mutable line cache.
    mutable std::mutex mutex_;
    /// Cached file contents split by line, keyed by file_id.
    /// Mutable because getLine() is logically const but lazily populates the cache.
    mutable std::unordered_map<uint32_t, std::shared_ptr<const std::vector<std::string>>>
        lineCache_;
    /// Monotonic source cache generation by file_id, used to discard stale loads.
    mutable std::unordered_map<uint32_t, uint64_t> lineCacheGenerations_;
    /// Retired line caches kept alive so previously returned string_views remain valid.
    mutable std::deque<std::shared_ptr<const std::vector<std::string>>> retiredLineCaches_;
    /// Stored file paths. Index corresponds to file identifier; index 0 is reserved.
    /// Implemented with std::deque to keep string references stable as new files are added.
    std::deque<std::string> files_;

    /// Absolute filesystem paths used for disk I/O; indexes mirror @ref files_.
    std::deque<std::filesystem::path> disk_paths_;

    /// Next identifier to assign; stored as 64-bit to detect overflow safely.
    uint64_t next_file_id_ = 1;

    /// Fast lookup from canonical disk/display key to previously assigned identifier.
    std::unordered_map<std::string, uint32_t> path_to_id_;

    friend struct SourceManagerTestAccess;
};
} // namespace il::support
