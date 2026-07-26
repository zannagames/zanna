//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/io/rt_dir_page.cpp
// Purpose: Provide bounded, resumable immediate-directory enumeration for
//          Zanna.IO.Dir.Page.
// Key invariants:
//   - One call emits at most 4096 immediate children and never recurses.
//   - Public paging is offset-based; native iterators remain private and bounded.
//   - Runtime maps and sequences retain every object returned to the caller.
// Ownership/Lifetime:
//   - Page result maps are caller-owned runtime objects.
//   - Cached directory iterators are process-local and evicted by LRU policy.
// Links: src/runtime/io/rt_dir.h,
//        docs/adr/0148-bounded-directory-paging.md
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements bounded resumable directory pages with a small cursor cache.
 * @details Normalizes roots, emits structured entry and diagnostic maps,
 * resumes exact offsets from process-local iterators, bounds each page, and
 * evicts least-recently-used cursors while retaining all runtime objects
 * placed in returned maps and sequences.
 */

#include "rt_dir.h"

#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

/** Default maximum number of entries emitted when the caller supplies no useful limit. */
constexpr int64_t kDefaultDirectoryPageSize = 128;
/** Hard per-call entry limit used to keep enumeration bounded. */
constexpr int64_t kMaximumDirectoryPageSize = 4096;
/** Maximum number of resumable native iterators retained process-wide. */
constexpr size_t kDirectoryPageCursorCacheSize = 8;

/**
 * @brief Cached native iterator and logical resume position for one directory.
 * @details Cursor nodes are detached under the cache lock before iteration,
 * then either destroyed or reinserted with a refreshed LRU timestamp.
 */
struct DirectoryPageCursor {
    std::string key;
    fs::directory_iterator iterator;
    fs::directory_iterator end;
    std::error_code error;
    int64_t offset{0};
    bool done{false};
    uint64_t lastUsed{0};
    DirectoryPageCursor *next{nullptr};
};

/** Head of the singly linked process-local cursor cache. */
DirectoryPageCursor *g_directoryPageCursorHead = nullptr;
/** Spin lock protecting cursor-cache links and lookup/eviction decisions. */
std::atomic_flag g_directoryPageCursorLock = ATOMIC_FLAG_INIT;
/** Monotonic counter used to stamp cursor recency. */
std::atomic<uint64_t> g_directoryPageCursorClock{0};

/** RAII owner of one acquisition of @ref g_directoryPageCursorLock. */
struct DirectoryPageCursorLockGuard {
    /// @brief Acquire the process-local cursor-cache spin lock.
    /// @details The protected critical sections only link, unlink, find, or
    /// evict cursor nodes; filesystem iteration occurs after releasing it.
    DirectoryPageCursorLockGuard() {
        while (g_directoryPageCursorLock.test_and_set(std::memory_order_acquire)) {
        }
    }

    /// @brief Release the cursor-cache spin lock.
    ~DirectoryPageCursorLockGuard() {
        g_directoryPageCursorLock.clear(std::memory_order_release);
    }

    /// Lock guards cannot be copied because each instance owns one acquisition.
    DirectoryPageCursorLockGuard(const DirectoryPageCursorLockGuard &) = delete;
    /// Lock guards cannot be copy-assigned.
    DirectoryPageCursorLockGuard &operator=(const DirectoryPageCursorLockGuard &) = delete;
};

/// @brief Copy a runtime string's exact byte span into a C++ string.
/// @param value Borrowed runtime string.
/// @return Byte-preserving string, or an empty string for `NULL`, invalid, or
/// zero-length input.
std::string toStdString(rt_string value) {
    if (!value)
        return {};
    const char *bytes = rt_string_cstr(value);
    const int64_t length = rt_str_len(value);
    if (!bytes || length <= 0)
        return {};
    return std::string(bytes, static_cast<size_t>(length));
}

/// @brief Release one temporary reference to a runtime object.
/// @param object Object reference; `nullptr` is accepted. Storage is reclaimed
/// immediately only when this was the final reference.
void releaseObject(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Store a copied C++ string value under an immortal map key.
/// @param map Runtime Map receiving the value.
/// @param key Static NUL-terminated key.
/// @param value Byte string copied into a fresh runtime string.
void mapSetString(void *map, const char *key, const std::string &value) {
    rt_string managed = rt_string_from_bytes(value.data(), value.size());
    rt_map_set_str(map, rt_const_cstr(key), managed);
    rt_string_unref(managed);
}

/// @brief Append a runtime object to an owning sequence and release the local reference.
/// @param sequence Element-owning runtime Seq.
/// @param value Newly created object whose reference is transferred to the
/// sequence.
void sequencePushOwned(void *sequence, void *value) {
    rt_seq_push(sequence, value);
    releaseObject(value);
}

/// @brief Append a structured `dir.page` diagnostic to a page result.
/// @param diagnostics Element-owning diagnostics Seq.
/// @param message Human-readable diagnostic text.
/// @param path Related path stored in the diagnostic's `file` field.
void pushDiagnostic(void *diagnostics, const std::string &message, const std::string &path) {
    void *diagnostic = rt_map_new();
    mapSetString(diagnostic, "message", message);
    mapSetString(diagnostic, "file", path);
    mapSetString(diagnostic, "code", "dir.page");
    sequencePushOwned(diagnostics, diagnostic);
}

/// @brief Remove a cursor node from the cache list without deleting it.
/// @param cursor Cursor to detach; `nullptr` and absent nodes are no-ops.
void unlinkCursor(DirectoryPageCursor *cursor) {
    if (!cursor)
        return;
    DirectoryPageCursor **slot = &g_directoryPageCursorHead;
    while (*slot) {
        if (*slot == cursor) {
            *slot = cursor->next;
            cursor->next = nullptr;
            return;
        }
        slot = &(*slot)->next;
    }
}

/// @brief Insert a detached cursor at the head of the cache list.
/// @param cursor Cursor to cache; `nullptr` is ignored.
void linkCursor(DirectoryPageCursor *cursor) {
    if (!cursor)
        return;
    cursor->next = g_directoryPageCursorHead;
    g_directoryPageCursorHead = cursor;
}

/// @brief Find a resumable cursor for one canonical directory and exact offset.
/// @param key Canonical generic-string directory key.
/// @param offset Logical next-entry offset requested by the caller.
/// @return Borrowed cached cursor when its key/offset match and it is not done;
/// otherwise `nullptr`.
DirectoryPageCursor *findCursor(const std::string &key, int64_t offset) {
    for (DirectoryPageCursor *cursor = g_directoryPageCursorHead; cursor; cursor = cursor->next) {
        if (!cursor->done && cursor->key == key && cursor->offset == offset)
            return cursor;
    }
    return nullptr;
}

/// @brief Count nodes in the cursor cache.
/// @return Current number of linked cursor objects.
size_t cursorCount() {
    size_t count = 0;
    for (DirectoryPageCursor *cursor = g_directoryPageCursorHead; cursor; cursor = cursor->next) {
        count++;
    }
    return count;
}

/// @brief Enforce the fixed cursor-cache limit using least-recently-used eviction.
/// @details Must be called with the cursor-cache lock held. Evicted cursors are
/// unlinked and deleted, closing their iterator state through RAII.
void evictCursorsIfNeeded() {
    while (cursorCount() > kDirectoryPageCursorCacheSize) {
        DirectoryPageCursor *oldest = nullptr;
        for (DirectoryPageCursor *cursor = g_directoryPageCursorHead; cursor;
             cursor = cursor->next) {
            if (!oldest || cursor->lastUsed < oldest->lastUsed)
                oldest = cursor;
        }
        if (!oldest)
            return;
        unlinkCursor(oldest);
        delete oldest;
    }
}

/// @brief Allocate and open a fresh cursor at directory offset zero.
/// @param key Canonical cache key copied into the cursor.
/// @param path Filesystem directory to enumerate.
/// @param diagnostics Seq receiving allocation or traversal diagnostics.
/// @return Caller-owned cursor on success, or `nullptr` after recording a
/// diagnostic.
DirectoryPageCursor *startCursor(const std::string &key, const fs::path &path, void *diagnostics) {
    auto *cursor = new (std::nothrow) DirectoryPageCursor();
    if (!cursor) {
        pushDiagnostic(diagnostics, "directory page cursor allocation failed", key);
        return nullptr;
    }
    cursor->key = key;
    cursor->lastUsed = g_directoryPageCursorClock.fetch_add(1, std::memory_order_relaxed) + 1;
    cursor->iterator = fs::directory_iterator(path, cursor->error);
    if (cursor->error) {
        pushDiagnostic(diagnostics, "directory traversal failed: " + cursor->error.message(), key);
        delete cursor;
        return nullptr;
    }
    cursor->done = cursor->iterator == cursor->end;
    return cursor;
}

/// @brief Convert one filesystem entry into the public page-entry map shape.
/// @details Status errors classify the entry as `other`; they do not abort the
/// surrounding page.
/// @param entries Element-owning result Seq.
/// @param directoryEntry Native immediate-child entry.
/// @param absolutePath Normalized absolute path reported to the caller.
void emitEntry(void *entries,
               const fs::directory_entry &directoryEntry,
               const fs::path &absolutePath) {
    std::error_code statusError;
    const bool isDirectory = directoryEntry.is_directory(statusError);
    statusError.clear();
    const bool isFile = !isDirectory && directoryEntry.is_regular_file(statusError);

    void *entry = rt_map_new();
    mapSetString(entry, "name", directoryEntry.path().filename().generic_string());
    mapSetString(entry, "path", absolutePath.generic_string());
    mapSetString(entry, "kind", isDirectory ? "directory" : (isFile ? "file" : "other"));
    rt_map_set_bool(entry, rt_const_cstr("isDirectory"), isDirectory ? 1 : 0);
    sequencePushOwned(entries, entry);
}

/// @brief Advance a detached cursor while emitting at most one requested page.
/// @param cursor Exclusively owned cursor removed from the shared cache.
/// @param root Normalized absolute directory root.
/// @param entries Element-owning output Seq.
/// @param requestedOffset First logical entry eligible for emission.
/// @param limit Maximum number of entries to emit.
/// @return Number of entry maps appended to @p entries.
int64_t scanCursor(DirectoryPageCursor *cursor,
                   const fs::path &root,
                   void *entries,
                   int64_t requestedOffset,
                   int64_t limit) {
    if (!cursor || cursor->done)
        return 0;

    int64_t emitted = 0;
    while (!cursor->error && cursor->iterator != cursor->end) {
        const fs::directory_entry current = *cursor->iterator;
        if (cursor->offset >= requestedOffset && emitted < limit) {
            emitEntry(entries, current, (root / current.path().filename()).lexically_normal());
            emitted++;
        }
        cursor->offset++;
        cursor->iterator.increment(cursor->error);
        if (emitted >= limit)
            break;
    }
    cursor->done = cursor->error || cursor->iterator == cursor->end;
    return emitted;
}

} // namespace

extern "C" {

/// @brief Return a bounded, resumable page of immediate directory children.
/// @details Negative offsets become zero. Non-positive limits select 128 and
/// values above 4096 are clamped. The returned map is always populated with
/// page metadata, entries, and diagnostics; invalid roots or caught C++
/// failures set `valid` false rather than escaping an exception across the C
/// ABI. An unfinished iterator is cached under its canonical path and exact
/// next offset.
/// @param pathString Runtime directory path.
/// @param offset Zero-based logical entry offset.
/// @param limit Requested page size.
/// @return Fresh runtime-owned result Map whose nested Seqs and entry Maps are
/// retained by the result.
void *rt_dir_page(rt_string pathString, int64_t offset, int64_t limit) {
    void *result = rt_map_new();
    void *entries = rt_seq_new_owned();
    void *diagnostics = rt_seq_new_owned();
    if (offset < 0)
        offset = 0;
    if (limit <= 0)
        limit = kDefaultDirectoryPageSize;
    if (limit > kMaximumDirectoryPageSize)
        limit = kMaximumDirectoryPageSize;

    rt_map_set_bool(result, rt_const_cstr("valid"), 1);
    mapSetString(result, "path", "");
    rt_map_set_int(result, rt_const_cstr("offset"), offset);
    rt_map_set_int(result, rt_const_cstr("limit"), limit);
    rt_map_set_int(result, rt_const_cstr("emitted"), 0);
    rt_map_set_int(result, rt_const_cstr("nextOffset"), offset);
    rt_map_set_bool(result, rt_const_cstr("done"), 1);
    rt_map_set(result, rt_const_cstr("entries"), entries);
    rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);

    try {
        const std::string input = toStdString(pathString);
        std::error_code error;
        fs::path root = fs::absolute(fs::path(input), error).lexically_normal();
        if (input.empty() || error || !fs::is_directory(root, error)) {
            rt_map_set_bool(result, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics, "directory page root is not a directory", input);
            releaseObject(entries);
            releaseObject(diagnostics);
            return result;
        }

        const std::string key = root.generic_string();
        mapSetString(result, "path", key);
        DirectoryPageCursor *cursor = nullptr;
        {
            DirectoryPageCursorLockGuard lock;
            cursor = findCursor(key, offset);
            if (cursor)
                unlinkCursor(cursor);
        }
        if (!cursor) {
            cursor = startCursor(key, root, diagnostics);
            if (!cursor) {
                rt_map_set_bool(result, rt_const_cstr("valid"), 0);
                releaseObject(entries);
                releaseObject(diagnostics);
                return result;
            }
        }
        std::unique_ptr<DirectoryPageCursor> cursorOwner(cursor);

        while (!cursor->done && cursor->offset < offset) {
            cursor->iterator.increment(cursor->error);
            cursor->offset++;
            if (cursor->error || cursor->iterator == cursor->end)
                cursor->done = true;
        }

        const int64_t emitted = scanCursor(cursor, root, entries, offset, limit);
        const bool done = cursor->done;
        const std::error_code traversalError = cursor->error;
        const int64_t nextOffset = cursor->offset;
        cursor->lastUsed = g_directoryPageCursorClock.fetch_add(1, std::memory_order_relaxed) + 1;
        {
            DirectoryPageCursorLockGuard lock;
            if (!done) {
                linkCursor(cursor);
                (void)cursorOwner.release();
                evictCursorsIfNeeded();
            }
        }

        if (traversalError) {
            pushDiagnostic(
                diagnostics, "directory traversal stopped early: " + traversalError.message(), key);
        }
        rt_map_set_int(result, rt_const_cstr("emitted"), emitted);
        rt_map_set_int(result, rt_const_cstr("nextOffset"), nextOffset);
        rt_map_set_bool(result, rt_const_cstr("done"), done ? 1 : 0);
    } catch (...) {
        rt_map_set_bool(result, rt_const_cstr("valid"), 0);
        rt_seq_clear(entries);
        pushDiagnostic(diagnostics, "directory page failed", toStdString(pathString));
    }

    releaseObject(entries);
    releaseObject(diagnostics);
    return result;
}

} // extern "C"
