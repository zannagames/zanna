//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/support/source_manager.cpp
// Purpose: Implement the diagnostic-aware source file registry used across the
//          compiler pipeline.
// Key invariants: File identifiers are assigned monotonically starting at one;
//                 identifier zero is reserved to represent "unknown" locations.
//                 Path normalisation produces stable, slash-separated strings so
//                 diagnostics do not leak host-specific formatting.
// Ownership/Lifetime: SourceManager owns all stored path strings and hands out
//                     string_view references that remain valid for the lifetime
//                     of the manager instance.  Callers are responsible for
//                     respecting that lifetime.
// Links: docs/internals/codemap.md#support-library, docs/internals/architecture.md#diagnostics
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides the backing store for file identifiers used in diagnostics.
/// @details Front ends hand a `SourceManager` file paths to obtain lightweight
///          integer identifiers.  Those identifiers flow through tokens,
///          diagnostics, and serialized IL artifacts.  Housing the lookup logic
///          in a dedicated translation unit keeps the hot path header minimal
///          while letting this implementation focus on normalization details.

#include "source_manager.hpp"

#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>

namespace il::support {
namespace {
/// @brief Return whether @p path names an in-memory or generated source buffer.
/// @param path Raw path supplied by the caller.
/// @return True when the path uses Zanna's angle-bracket virtual source form.
/// @details Paths such as `<memory>.zia` and `<eval>` are user-facing labels, not
///          filesystem paths.  Treating them as disk-backed would make their
///          identifiers depend on the current working directory and would trigger
///          pointless disk I/O when diagnostics ask for source lines.
bool isVirtualSourcePath(std::string_view path) {
    return !path.empty() && path.front() == '<' && path.find('>') != std::string_view::npos;
}

/// @brief Fold ASCII path casing when the host filesystem convention requires it.
/// @param key Lookup key to mutate in place.
/// @details Windows path comparisons in Zanna are intentionally case-insensitive,
///          but diagnostics should still preserve the user's display spelling.
///          This helper is therefore applied only to internal lookup keys.
void foldPathLookupKeyCase(std::string &key) {
    if constexpr (zanna::platform::kHostWindows) {
        for (char &ch : key) {
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
}

/// @brief Normalise a filesystem path into the canonical representation used by diagnostics.
/// @details The helper constructs a `std::filesystem::path` from the raw input,
///          applies @ref std::filesystem::path::lexically_normal to collapse
///          redundant components, and finally emits the generic (forward-slash
///          separated) representation.  Display spelling and case are preserved;
///          only the internal lookup key is case-folded on case-insensitive hosts.
///          The resulting string is suitable for persistent storage inside the
///          @ref SourceManager.
/// @param path Raw filesystem path supplied by the caller.
/// @return Normalised path string owned by the caller.
std::string normalizePath(std::string path) {
    if (path.empty())
        return "<unknown>";

    std::filesystem::path p = zanna::filesystem::pathFromUtf8(path);
    std::string normalized = zanna::filesystem::genericPathToUtf8(p.lexically_normal());

    return normalized;
}

/// @brief Build the stable filesystem path used for disk reads.
/// @param path Raw path supplied by the caller.
/// @return Absolute, lexically-normal filesystem path when it can be computed.
/// @details Diagnostic display paths stay relative and portable, but disk reads
///          need a path that is independent of later current-working-directory
///          changes. Existing paths are weakly canonicalized to deduplicate
///          symlink spellings where the host filesystem can resolve them.
///          Filesystem errors fall back to the lexical input path so registration
///          remains tolerant of missing paths.
std::filesystem::path makeDiskPath(const std::string &path) {
    if (isVirtualSourcePath(path))
        return {};

    std::filesystem::path p = zanna::filesystem::pathFromUtf8(path);
    if (p.empty())
        return p;

    std::error_code ec;
    std::filesystem::path absolute = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
    if (ec)
        absolute = p;
    std::filesystem::path normalized = absolute.lexically_normal();

    ec.clear();
    std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec && !canonical.empty())
        return canonical.lexically_normal();
    return normalized;
}

/// @brief Build the deduplication key used for registered source paths.
/// @param diskPath Absolute path used for disk I/O, when one exists.
/// @param displayPath Normalized path that will be printed in diagnostics.
/// @return Stable lookup key for @ref SourceManager::path_to_id_.
/// @details Display paths intentionally remain relative and user-facing, but they
///          are not enough to identify files registered from different current
///          working directories.  Disk-backed files therefore dedupe by absolute
///          disk path, while virtual or empty paths fall back to display text.
std::string makePathLookupKey(const std::filesystem::path &diskPath, std::string_view displayPath) {
    std::string key;
    if (!diskPath.empty())
        key = zanna::filesystem::genericPathToUtf8(diskPath.lexically_normal());
    else
        key = std::string(displayPath);
    if (key.empty())
        key = "<unknown>";

    foldPathLookupKeyCase(key);

    return (diskPath.empty() ? "display:" : "disk:") + key;
}

/// @brief Remove a trailing carriage return from a line buffer.
/// @param line Line buffer read from disk or in-memory source.
/// @details Normalizes CRLF text to the same cached representation as LF text.
void stripTrailingCarriageReturn(std::string &line) {
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
}

/// @brief Split source text into cached line strings.
/// @param source Full source text to split on newline characters.
/// @return Source lines without line terminators.
/// @details Mirrors std::getline semantics for trailing newlines while preserving
///          one empty line for an entirely empty source buffer.
std::vector<std::string> splitSourceLines(std::string_view source) {
    std::vector<std::string> lines;
    lines.reserve(static_cast<size_t>(std::count(source.begin(), source.end(), '\n')) + 1);
    std::string current;
    for (char ch : source) {
        if (ch == '\n') {
            stripTrailingCarriageReturn(current);
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() || source.empty()) {
        stripTrailingCarriageReturn(current);
        lines.push_back(std::move(current));
    }
    return lines;
}

/// @brief Read and split source lines from a disk path.
/// @param path Absolute or relative path to open.
/// @return Cached line vector, or std::nullopt when the file cannot be opened.
/// @details The file is opened in binary mode so bytes and diagnostic columns are
///          interpreted consistently across platforms.  Line endings are still
///          normalized by @ref stripTrailingCarriageReturn after std::getline.
std::optional<std::vector<std::string>> loadSourceLinesFromDisk(const std::filesystem::path &path) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;

    std::string buf;
    while (std::getline(f, buf)) {
        stripTrailingCarriageReturn(buf);
        lines.push_back(std::move(buf));
    }
    if (f.bad())
        return std::nullopt;
    if (lines.empty())
        lines.emplace_back();
    return lines;
}

/// @brief Build an immutable cache for absent or unreadable source text.
/// @return Shared empty line vector used as a cached "no source available" marker.
/// @details SourceManager distinguishes "not cached yet" from "known unavailable"
///          by storing an empty vector in the cache map.  This prevents repeated
///          failed disk opens on every diagnostic while preserving hasLine() == false.
std::shared_ptr<const std::vector<std::string>> makeUnavailableLineCache() {
    return std::make_shared<const std::vector<std::string>>();
}
} // namespace

/// @brief Register a file path and assign it a stable identifier.
///
/// @details Normalises @p path using @ref normalizePath before deduplicating it
///          against previously seen entries.  Identifiers start at one so that
///          zero can unambiguously signal "unknown".  When the identifier space
///          would overflow, the helper returns zero so callers can surface a fatal
///          configuration error in their own diagnostic context.  Stored strings live inside the
///          manager's
///          @ref files_ container for the remainder of the manager's lifetime,
///          making it safe to hand out @ref std::string_view references.
///
/// @param path Filesystem path to normalize and store.
/// @return Identifier (>0) representing the stored path, or zero on overflow.
uint32_t SourceManager::addFile(std::string path) {
    std::string normalized = normalizePath(path);
    std::filesystem::path diskPath = makeDiskPath(path);
    std::string lookupKey = makePathLookupKey(diskPath, normalized);

    std::lock_guard lock(mutex_);
    if (auto it = path_to_id_.find(lookupKey); it != path_to_id_.end())
        return it->second;

    if (next_file_id_ > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }

    path_to_id_.reserve(path_to_id_.size() + 1);
    const uint32_t file_id = static_cast<uint32_t>(next_file_id_);
    files_.push_back(std::move(normalized));
    try {
        disk_paths_.push_back(std::move(diskPath));
    } catch (...) {
        files_.pop_back();
        throw;
    }
    try {
        path_to_id_.emplace(std::move(lookupKey), file_id);
    } catch (...) {
        disk_paths_.pop_back();
        files_.pop_back();
        throw;
    }
    ++next_file_id_;
    return file_id;
}

/// @brief Retrieve the canonical path associated with a file identifier.
///
/// @details Performs range checks against the stored vector to guarantee that
///          invalid identifiers (including zero) result in an empty view rather
///          than undefined behaviour.  Successful lookups return an owning
///          manager-backed string view, allowing diagnostics to print the path
///          without copying.  Callers must ensure the @ref SourceManager outlives
///          any view they retain.
///
/// @param file_id 1-based identifier previously returned by addFile().
/// @return Stored path, or empty string view if @p file_id is invalid.
std::string_view SourceManager::getPath(uint32_t file_id) const {
    std::lock_guard lock(mutex_);
    return getPathLocked(file_id);
}

/// @brief Retrieve a single source line, loading and caching the file on demand.
///
/// @details The first request for a given @p file_id reads the entire file from
///          disk and splits it into a per-line cache; subsequent requests are
///          served from that cache.  When the file cannot be opened an empty
///          line vector is cached so repeated lookups do not retry the failing
///          I/O.  Identifier zero, line zero, and out-of-range line numbers all
///          yield an empty view rather than throwing.  The returned view is
///          backed by the cache and stays valid for the manager's lifetime.
///
/// @param file_id 1-based identifier previously returned by addFile().
/// @param line 1-based line number to retrieve.
/// @return The line text without its terminator, or empty view if unavailable.
std::string_view SourceManager::getLine(uint32_t file_id, uint32_t line) const {
    if (file_id == 0 || line == 0)
        return {};

    const auto lines = ensureLineCache(file_id);
    if (!lines || line > lines->size())
        return {};
    return (*lines)[line - 1];
}

/// @brief Check whether a line exists for a registered source file.
///
/// @details Uses the same lazy cache path as @ref getLine but returns true for
///          existing blank lines, letting diagnostic printers render an empty
///          source line instead of treating it as missing context.
///
/// @param file_id 1-based identifier previously returned by addFile().
/// @param line 1-based line number to query.
/// @return True when @p line exists in the cached source text.
bool SourceManager::hasLine(uint32_t file_id, uint32_t line) const {
    if (file_id == 0 || line == 0)
        return false;

    const auto lines = ensureLineCache(file_id);
    return lines && line <= lines->size();
}

/// @brief Invalidate cached source text for a single file identifier.
///
/// @details Erases both successful and failed read caches. The next @ref getLine
///          or @ref hasLine call will try to load the file from disk again unless
///          a caller first seeds replacement text with @ref setSource.
///
/// @param file_id 1-based identifier previously returned by addFile().
void SourceManager::invalidateSource(uint32_t file_id) const {
    if (file_id == 0)
        return;

    std::lock_guard lock(mutex_);
    if (!isRegisteredFileId(file_id))
        return;
    bumpLineCacheGenerationLocked(file_id);
    retireLineCacheLocked(file_id);
}

/// @brief Seed the line cache for a file id directly from in-memory text.
///
/// @details Lets callers associate source text with a @p file_id without the
///          file existing on disk — useful for REPL buffers, generated code, and
///          tests.  The text is split on `'\n'`, with a trailing `'\r'` stripped
///          from each line so Windows CRLF input caches identically to LF input.
///          A final unterminated segment (or an entirely empty @p source) is
///          recorded as a trailing line.  Any previously cached lines for the id
///          are replaced.  Identifier zero is ignored.
///
/// @param file_id Identifier previously returned by addFile().
/// @param source Full source text to split into cached lines.
void SourceManager::setSource(uint32_t file_id, std::string source) {
    auto replacement = std::make_shared<const std::vector<std::string>>(splitSourceLines(source));

    std::lock_guard lock(mutex_);
    if (!isRegisteredFileId(file_id))
        return;

    bumpLineCacheGenerationLocked(file_id);
    retireLineCacheLocked(file_id);
    lineCache_[file_id] = std::move(replacement);
}

/// @brief Check whether a file identifier is in the registered range.
/// @param file_id Candidate 1-based file identifier.
/// @return True when @p file_id maps to stored path metadata.
/// @pre Caller must hold @ref mutex_.
bool SourceManager::isRegisteredFileId(uint32_t file_id) const noexcept {
    return file_id != 0 && file_id <= files_.size();
}

/// @brief Resolve a registered file id to its display path without locking.
/// @param file_id Candidate 1-based file identifier.
/// @return Stored display path, or empty for invalid ids.
/// @pre Caller must hold @ref mutex_.
std::string_view SourceManager::getPathLocked(uint32_t file_id) const {
    if (!isRegisteredFileId(file_id))
        return {};
    return files_[file_id - 1];
}

/// @brief Load or retrieve cached source lines.
/// @param file_id Registered 1-based file identifier.
/// @return Shared cached line vector, or null when @p file_id is invalid.
/// @details Captures the file's cache generation while locked, performs disk I/O
///          without holding the manager mutex, and publishes the immutable result
///          only if no concurrent invalidation or replacement superseded it.
std::shared_ptr<const std::vector<std::string>> SourceManager::ensureLineCache(
    uint32_t file_id) const {
    std::filesystem::path diskPath;
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (!isRegisteredFileId(file_id))
            return {};

        auto it = lineCache_.find(file_id);
        if (it != lineCache_.end())
            return it->second;

        diskPath = disk_paths_[file_id - 1];
        generation = lineCacheGenerationLocked(file_id);
    }

    std::shared_ptr<const std::vector<std::string>> loaded;
    if (diskPath.empty()) {
        loaded = makeUnavailableLineCache();
    } else if (auto loadedLines = loadSourceLinesFromDisk(diskPath)) {
        loaded = std::make_shared<const std::vector<std::string>>(std::move(*loadedLines));
    } else {
        loaded = makeUnavailableLineCache();
    }

    std::lock_guard lock(mutex_);
    if (!isRegisteredFileId(file_id))
        return {};

    if (generation != lineCacheGenerationLocked(file_id)) {
        auto current = lineCache_.find(file_id);
        if (current != lineCache_.end())
            return current->second;
        return {};
    }

    auto it = lineCache_.find(file_id);
    if (it != lineCache_.end())
        return it->second;

    auto inserted = lineCache_.emplace(file_id, std::move(loaded)).first;
    return inserted->second;
}

/// @brief Preserve and erase the current cache for a file id.
/// @param file_id Registered 1-based file identifier.
/// @details Moving the shared line buffer into @ref retiredLineCaches_ keeps
///          previously returned string views valid while allowing future lookups
///          to reload or replace the active cache.
/// @pre Caller must hold @ref mutex_.
void SourceManager::retireLineCacheLocked(uint32_t file_id) const {
    auto it = lineCache_.find(file_id);
    if (it == lineCache_.end())
        return;
    retiredLineCaches_.push_back(std::move(it->second));
    lineCache_.erase(it);
}

/// @brief Read the cache generation for a registered file id.
/// @param file_id Registered 1-based file identifier.
/// @return Current generation, or zero when the file has never been invalidated.
/// @pre Caller must hold @ref mutex_.
uint64_t SourceManager::lineCacheGenerationLocked(uint32_t file_id) const {
    auto it = lineCacheGenerations_.find(file_id);
    return it == lineCacheGenerations_.end() ? 0 : it->second;
}

/// @brief Increment the cache generation for a registered file id.
/// @param file_id Registered 1-based file identifier.
/// @pre Caller must hold @ref mutex_.
/// @details Saturating at the maximum 64-bit value is sufficient because a stale
///          loader only needs to observe inequality with the generation it
///          captured before I/O; wrapping would make a very old load appear fresh.
void SourceManager::bumpLineCacheGenerationLocked(uint32_t file_id) const {
    uint64_t &generation = lineCacheGenerations_[file_id];
    if (generation != std::numeric_limits<uint64_t>::max())
        ++generation;
}
} // namespace il::support
