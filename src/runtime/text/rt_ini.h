//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_ini.h
// Purpose: Parse, format, inspect, and mutate case-sensitive INI documents
//          represented as section-name to key/value Map hierarchies.
//
// Key invariants:
//   - Returns a Map where keys are section names and values are Maps of key-value pairs.
//   - Sectionless entries at the top of the file are stored under the empty string key ('').
//   - Comment lines starting with ';' or '#' are ignored.
//   - Values are stored as strings; no type inference is performed.
//   - Parsing trims names and values but performs no quoting, escaping, or
//     inline-comment processing.
//   - Formatting emits the default section first; all remaining order follows
//     the implementation-defined order of Map key snapshots.
//
// Ownership/Lifetime:
//   - Returned Maps, Seqs, and strings carry references owned by the caller.
//   - Input strings and container objects are borrowed for each operation.
//
// Links: src/runtime/text/rt_ini.c (implementation),
//        src/runtime/core/rt_string.h (runtime strings),
//        src/runtime/collections/rt_map.h (document representation),
//        src/runtime/collections/rt_seq.h (section snapshots)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ini.h
 * @brief Declares Map-backed INI parsing, formatting, lookup, and mutation.
 * @details The document model maps case-sensitive section names to
 *          case-sensitive String key/value Maps, with `\"\"` representing the
 *          implicit section. Operations return caller-owned snapshots or
 *          Strings while borrowing input text and document containers.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse an INI-format string into a Map of Maps.
/// @details Splits CR, LF, and CRLF lines; ignores blank and whole-line
///          comments; trims section names, keys, and values; and splits values
///          at the first `=`. Duplicate keys retain the last value. Entries
///          before a section header are stored under `""`.
/// @param text Borrowed INI content; null and empty input produce an empty Map.
/// @return Newly allocated root Map owning its section Maps and strings.
void *rt_ini_parse(rt_string text);

/// @brief Format a Map-of-Maps back to INI string format.
/// @details Writes default-section entries first as `key = value`, then each
///          named section after a blank line. Names and values are emitted
///          verbatim without quoting or escaping. Map snapshot order is
///          implementation-defined.
/// @param ini_map Borrowed Map hierarchy returned by `rt_ini_parse`, or null.
/// @return Newly allocated formatted string; null input yields empty text.
rt_string rt_ini_format(void *ini_map);

/// @brief Get a value from a parsed INI map.
/// @details Missing inputs and entries return an empty string, so absence and
///          an explicitly empty value are indistinguishable.
/// @param ini_map Borrowed root INI Map.
/// @param section Borrowed section name; use `""` for the default section.
/// @param key Borrowed key name.
/// @return Caller-owned retained value reference, or a caller-owned empty
///         string when the entry is unavailable.
rt_string rt_ini_get(void *ini_map, rt_string section, rt_string key);

/// @brief Set a value in a parsed INI map (creates section if needed).
/// @details Null root, section, or key arguments are ignored. Existing keys are
///          replaced according to normal Map ownership behavior.
/// @param ini_map Borrowed mutable root INI Map.
/// @param section Borrowed section name.
/// @param key Borrowed key name.
/// @param value Borrowed runtime string value to store.
void rt_ini_set(void *ini_map, rt_string section, rt_string key, rt_string value);

/// @brief Check if a section exists.
/// @param ini_map Borrowed root INI Map.
/// @param section Borrowed section name.
/// @return `1` when the section key exists, otherwise `0`.
int8_t rt_ini_has_section(void *ini_map, rt_string section);

/// @brief Get all section names as a Seq.
/// @details Ordering is implementation-defined. Null input returns an empty
///          newly allocated Seq.
/// @param ini_map Borrowed root INI Map.
/// @return Newly allocated Seq containing copies of all section-name strings.
void *rt_ini_sections(void *ini_map);

/// @brief Remove a key from a section.
/// @details Removing the final key does not remove its now-empty section Map.
/// @param ini_map Borrowed mutable root INI Map.
/// @param section Borrowed section name.
/// @param key Borrowed key name.
/// @return `1` when an entry was removed, otherwise `0`.
int8_t rt_ini_remove(void *ini_map, rt_string section, rt_string key);

#ifdef __cplusplus
}
#endif
