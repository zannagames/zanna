//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_config.h
/// @file
/// @brief Declares the JSON-backed Zanna.Game.Config runtime bridge.
//
// Purpose: Runtime bridge for Zanna.Game.Config — a typed configuration loader
//   that parses JSON and exposes values via dotted-path lookups with caller-
//   supplied defaults for missing/mistyped keys.
//
// Key invariants:
//   - Config objects are heap-allocated opaque `void *` handles.
//   - Paths are dotted strings (e.g. "audio.volume"); a missing or wrong-typed
//     key yields the supplied default rather than trapping.
//
// Ownership/Lifetime:
//   - rt_config_load / rt_config_from_string return runtime-object references
//     whose finalizer owns and releases the parsed JSON root.
//   - A successful rt_config_get_str returns a fresh caller-owned runtime
//     string. On failure it returns the supplied default pointer unchanged.
//   - The scalar getters and rt_config_has do NOT retain a reference into the
//     parsed JSON tree: existence checks use the non-retaining rt_jsonpath_has,
//     and typed try-getters release resolved nodes internally (VDOC-236).
//
// Links: src/runtime/game/rt_config.c (implementation)
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate opaque Config handles.
#define RT_CONFIG_CLASS_ID INT64_C(-0x510215)

/// @brief Load and parse a JSON configuration file.
/// @param path Runtime string containing the file path.
/// @return A new Config reference, or `NULL` for a null path, a missing or
///         empty file, empty decoded text, parse failure, or allocation
///         failure.
/// @details The missing-file precheck preserves Config's soft-failure contract.
///          Temporary byte and text objects are released before return.
void *rt_config_load(void *path);

/// @brief Parse an in-memory JSON document into a Config.
/// @param json_str Runtime string containing the JSON document.
/// @return A new Config reference, or `NULL` for null input, parse failure, or
///         allocation failure.
/// @details The input string remains caller-owned; the Config owns the parsed
///          JSON root.
void *rt_config_from_string(void *json_str);

/// @brief Read an integer-convertible value at a dotted path.
/// @param cfg Config handle to query.
/// @param path Runtime string containing the dotted JSON path.
/// @param default_val Fallback for null inputs, missing paths, and values that
///        cannot be converted to an integer.
/// @return The converted integer or @p default_val.
/// @details A non-null Config handle with the wrong runtime class raises a trap.
///          The lookup retains no JSON-tree node.
int64_t rt_config_get_int(void *cfg, void *path, int64_t default_val);

/// @brief Read a string-convertible value at a dotted path.
/// @param cfg Config handle to query.
/// @param path Runtime string containing the dotted JSON path.
/// @param default_val Pointer returned unchanged when lookup or conversion
///        fails.
/// @return A fresh caller-owned runtime string on success; otherwise
///         @p default_val with its existing ownership unchanged.
/// @details A non-null Config handle with the wrong runtime class raises a trap.
///          The lookup releases its resolved JSON node internally.
void *rt_config_get_str(void *cfg, void *path, void *default_val);

/// @brief Interpret an integer-convertible value as a boolean.
/// @param cfg Config handle to query.
/// @param path Runtime string containing the dotted JSON path.
/// @param default_val Fallback returned verbatim when lookup or conversion
///        fails.
/// @return `1` for a converted nonzero value, `0` for a converted zero, or
///         @p default_val on failure.
/// @details A non-null Config handle with the wrong runtime class raises a trap.
int8_t rt_config_get_bool(void *cfg, void *path, int8_t default_val);

/// @brief Test whether a value exists at dotted @p path.
/// @param cfg Config handle to query.
/// @param path Runtime string containing the dotted JSON path.
/// @return The JsonPath existence result, or `0` for null inputs or a Config
///         without a JSON root.
/// @details A non-null Config handle with the wrong runtime class raises a trap.
///          The existence check does not retain the resolved node.
int8_t rt_config_has(void *cfg, void *path);

/// @brief Return the borrowed parsed JSON root of a Config (internal/testing).
/// @details Not a registered runtime surface symbol. Exposes the internal JSON
/// tree so tests can verify that query methods do not retain resolved nodes
/// (VDOC-236). The returned pointer is borrowed — the Config still owns it.
/// @param cfg Config handle to inspect.
/// @return The JSON root, or `NULL` for a null Config or a handle with the
///         wrong runtime class.
/// @note Unlike the typed getters, this helper does not trap on a class-ID
///       mismatch.
void *rt_config_json_root(void *cfg);

#ifdef __cplusplus
}
#endif
