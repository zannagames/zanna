//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file_path.h
// Purpose: Internal helpers for translating BASIC OPEN mode enumerations to fopen mode strings and
// POSIX flags, extracting validated UTF-8 paths from runtime strings.
//
// Key invariants:
//   - rt_file_mode_string returns NULL for invalid mode enumerations.
//   - rt_file_mode_to_flags resets flags_out to zero before parsing and publishes nonzero flags
//     only after successful validation.
//   - Path extraction validates that the input is non-null and contains no embedded NUL bytes.
//   - Runtime-string byte/path pointers are borrowed views; Windows UTF-8-to-wide conversion is
//     the only helper here that returns a caller-freed allocation.
//
// Ownership/Lifetime:
//   - Pointers returned by string/path view helpers are non-owning and must not be freed.
//   - rt_file_mode_string returns immutable static storage.
//   - On Windows, rt_file_path_utf8_to_wide returns heap storage that the caller frees; the
//     reverse conversion returns a runtime-managed string.
//   - All ZannaString inputs are borrowed, not retained.
//
// Links: src/runtime/io/rt_file_path.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Convert BASIC OPEN mode enumeration to the corresponding mode string.
/// @details Maps INPUT, OUTPUT, APPEND, BINARY, and RANDOM to immutable `fopen`-style spellings.
/// @param mode BASIC OPEN mode value.
/// @return Pointer to immutable static mode text, or NULL when @p mode is unsupported.
const char *rt_file_mode_string(int32_t mode);

/// @brief Parse an fopen-style @p mode string into POSIX open(2) flags.
/// @details Accepts an `r`, `w`, or `a` base followed by `+`, `b`, and/or `t` modifiers. BASIC
///          BINARY/RANDOM modes imply creation, and platform-specific close-on-exec/binary flags
///          are added as needed. @p flags_out is reset to zero before validation.
/// @param mode Null-terminated mode string to parse.
/// @param basic_mode BASIC OPEN mode enumerator when known; use `RT_F_UNSPECIFIED` otherwise.
/// @param[out] flags_out Required destination for the resolved flag bits.
/// @return 1 when the complete mode string is valid and flags were produced; otherwise 0.
int8_t rt_file_mode_to_flags(const char *mode, int32_t basic_mode, int *flags_out);

/// @brief Extract a filesystem path pointer from a runtime string.
/// @details Rejects invalid handles and embedded NUL bytes. The borrowed result remains valid only
///          while @p path and its backing storage remain alive.
/// @param path Runtime string containing the path; may be NULL.
/// @param[out] out_path Optional destination for the borrowed UTF-8 pointer; cleared on entry.
/// @return 1 when @p path is a valid path string; otherwise 0.
int8_t rt_file_path_from_vstr(const ZannaString *path, const char **out_path);

/// @brief Produce a byte view for a runtime string suitable for writing to a file.
/// @details No validation of encoding or embedded NUL bytes is performed. A zero return can mean
///          either an invalid handle or a valid empty string, so callers requiring validation
///          should use rt_file_string_require_view().
/// @param s Runtime string; may be NULL.
/// @param[out] data_out Optional destination for the borrowed bytes; cleared on entry.
/// @return Number of bytes referenced by the view.
size_t rt_file_string_view(const ZannaString *s, const uint8_t **data_out);

/// @brief Produce a byte view for a required runtime string, trapping on invalid handles.
/// @details The returned pointer is borrowed from @p s and no encoding validation is performed.
/// @param s Runtime string; must be a valid string handle.
/// @param[out] data_out Optional destination for the borrowed bytes; cleared on entry.
/// @param context Trap message used when the string handle is invalid; may be NULL.
/// @return Number of bytes referenced by the view.
size_t rt_file_string_require_view(const ZannaString *s,
                                   const uint8_t **data_out,
                                   const char *context);

#ifdef _WIN32
/// @brief Convert a UTF-8 path to a wide Windows path.
/// @details Conversion is strict and rejects malformed UTF-8 rather than substituting characters.
/// @param utf8 Null-terminated UTF-8 path.
/// @return Newly allocated wide string for the caller to free, or NULL on failure.
wchar_t *rt_file_path_utf8_to_wide(const char *utf8);

/// @brief Convert a wide Windows path to a runtime UTF-8 string.
/// @details Conversion is strict. Null input, invalid wide text, allocation failure, and
///          conversion failure all produce the shared empty string.
/// @param wide Null-terminated wide path.
/// @return Runtime-managed UTF-8 string, or the shared empty string on failure.
rt_string rt_file_path_wide_to_string(const wchar_t *wide);
#endif

#ifdef __cplusplus
}
#endif
