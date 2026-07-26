//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_savedata.h
// Purpose: Cross-platform game save/load persistence system. Provides a
//   key-value store (string keys → int64/string values) that serializes
//   to JSON in platform-appropriate directories.
//
// Key invariants:
//   - Keys are unique; setting a key overwrites any previous value.
//   - Keys are nonempty, NUL-free well-formed UTF-8; string values must be well-formed UTF-8.
//   - Save serializes only signed 64-bit integers and strings, then durably replaces the whole
//     JSON file through an exclusive adjacent sidecar.
//   - Load parses into a separate list and replaces current memory only after the complete JSON
//     object validates. A missing save file clears the store and succeeds.
//   - File path is computed from game name using platform conventions.
//
// Ownership/Lifetime:
//   - SaveData objects are runtime-managed opaque handles with a finalizer.
//   - Setters borrow and retain runtime-string references for keys/values; removal and finalization
//     release them.
//   - String getters and path/data-directory queries return caller-owned runtime references.
//
// Links: src/runtime/io/rt_savedata.c (implementation),
//        src/runtime/io/rt_dir.h (directory creation),
//        src/runtime/text/rt_json_stream.h (JSON token parser)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares managed cross-platform JSON save-data persistence.
 * @details Exposes a typed string-key store, mutation and query operations,
 * transactional load, durable atomic save, computed save-file paths, and
 * validated per-application data-directory discovery.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new SaveData instance for the given game name.
/// @details Creates an empty in-memory store; call rt_savedata_load() explicitly to read existing
///          data. The name must contain 1..64 ASCII letters, digits, dash, or underscore bytes.
/// @param game_name Borrowed runtime identifier used to compute the platform save-file path.
/// @return Fresh runtime-managed opaque SaveData handle; invalid input/allocation failure traps.
void *rt_savedata_new(rt_string game_name);

/// @brief Store an integer value.
/// @details Updates memory only and permits replacing a string-valued entry.
/// @param sd Borrowed SaveData handle.
/// @param key Borrowed nonempty, NUL-free UTF-8 key retained by the store as needed.
/// @param value Signed 64-bit value to store.
void rt_savedata_set_int(void *sd, rt_string key, int64_t value);

/// @brief Store a string value.
/// @details Updates memory only and permits replacing an integer-valued entry.
/// @param sd Borrowed SaveData handle.
/// @param key Borrowed nonempty, NUL-free UTF-8 key retained by the store as needed.
/// @param value Borrowed well-formed UTF-8 string retained by the store.
void rt_savedata_set_string(void *sd, rt_string key, rt_string value);

/// @brief Retrieve an integer value, or default if key not found.
/// @param sd Borrowed SaveData handle; NULL selects @p default_val.
/// @param key Borrowed key to query.
/// @param default_val Fallback for absent or string-valued entries.
/// @return Stored integer when present with integer type; otherwise @p default_val.
int64_t rt_savedata_get_int(void *sd, rt_string key, int64_t default_val);

/// @brief Retrieve a string value, or default if key not found.
/// @param sd Borrowed SaveData handle; NULL selects the fallback.
/// @param key Borrowed key to query.
/// @param default_val Borrowed fallback string; NULL selects the shared empty string.
/// @return Caller-owned reference to the stored value, fallback, or shared empty string.
rt_string rt_savedata_get_string(void *sd, rt_string key, rt_string default_val);

/// @brief Write all key-value pairs to the save file.
/// @details Creates parent directories, builds UTF-8 JSON, writes and synchronizes an exclusive
///          sidecar, atomically replaces the destination, and synchronizes its parent directory.
/// @param sd Borrowed SaveData handle.
/// @return 1 on success; 0 on null state or any operational/serialization failure.
int8_t rt_savedata_save(void *sd);

/// @brief Load and transactionally replace key-value pairs from the save file.
/// @details Reads the full file, accepts one top-level object containing only integer/string
///          values, and preserves current memory on malformed or unreadable input. A missing file
///          clears the store and succeeds.
/// @param sd Borrowed SaveData handle.
/// @return 1 on successful replacement or missing-file empty state; otherwise 0.
int8_t rt_savedata_load(void *sd);

/// @brief Check if a key exists.
/// @param sd Borrowed SaveData handle.
/// @param key Borrowed key to query.
/// @return 1 when a byte-identical key exists regardless of value type; otherwise 0.
int8_t rt_savedata_has_key(void *sd, rt_string key);

/// @brief Remove a key and its value.
/// @details Updates memory only; call rt_savedata_save() to persist the removal.
/// @param sd Borrowed SaveData handle.
/// @param key Borrowed key to remove.
/// @return 1 if removed; 0 if absent/invalid.
int8_t rt_savedata_remove(void *sd, rt_string key);

/// @brief Remove all key-value pairs.
/// @details Updates memory only; call rt_savedata_save() to persist an empty object.
/// @param sd Borrowed SaveData handle; NULL is ignored.
void rt_savedata_clear(void *sd);

/// @brief Get the number of stored key-value pairs.
/// @param sd Borrowed SaveData handle; NULL reports zero.
/// @return Number of unique in-memory keys.
int64_t rt_savedata_count(void *sd);

/// @brief Get the computed save file path.
/// @param sd Borrowed SaveData handle.
/// @return Caller-owned runtime string containing the absolute path, or empty for null state.
rt_string rt_savedata_get_path(void *sd);

/// @brief Zanna.IO.Path.DataDir(app): resolve (and create on demand) the
///        per-user writable data directory for an application.
/// @details Uses `%APPDATA%`/roaming fallback on Windows, Application Support on macOS, and
///          absolute `$XDG_DATA_HOME` or `~/.local/share` on other POSIX systems.
/// @param app_name Borrowed 1..64-byte ASCII application folder name containing only letters,
///                 digits, dash, or underscore.
/// @return Caller-owned runtime string containing the absolute directory without trailing
///         separator; resolution or creation failures trap.
rt_string rt_path_data_dir(rt_string app_name);

#ifdef __cplusplus
}
#endif
