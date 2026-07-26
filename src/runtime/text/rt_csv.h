//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_csv.h
// Purpose: CSV parsing and formatting utilities based on RFC 4180 quoting rules, handling quoted
// fields, embedded newlines, escaped double-quotes, and custom delimiters. Deviations from strict
// RFC 4180: multi-row Format emits LF (not CRLF) row endings, unequal field counts are not
// enforced, and parsing accepts LF and CR line endings as extensions.
//
// Key invariants:
//   - Handles quoted fields, escaped quotes (double-quote inside quotes), and newlines in fields.
//   - Default delimiter is comma; custom delimiters are supported.
//   - ParseLine returns a Seq of strings for one CSV row.
//   - Format escapes strings containing the delimiter or quote character automatically.
//
// Ownership/Lifetime:
//   - Returned Seq and string objects are newly allocated; caller must release.
//   - Input strings are borrowed; callers retain ownership.
//
// Links: src/runtime/text/rt_csv.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_csv.h
 * @brief Declares CSV row/document parsing and formatting utilities.
 * @details Default-comma and validated custom-delimiter variants operate on
 *          borrowed runtime text, accept the documented record-ending
 *          extensions, preserve embedded quoted newlines, and return
 *          caller-owned Strings or element-owning nested sequences.
 */

#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse a single CSV line into a Seq of strings.
/// @details Uses comma, accepts quoted embedded line endings and doubled quotes,
///          and rejects trailing content after the first complete record.
/// @param line CSV line to parse; NULL yields an empty Seq.
/// @return Caller-owned element-owning Seq of field strings, or NULL after a
///         syntax/allocation failure.
void *rt_csv_parse_line(rt_string line);

/// @brief Parse a single CSV line with custom delimiter.
/// @param line CSV line to parse; NULL yields an empty Seq.
/// @param delim Delimiter string; must be exactly one byte (empty selects the comma default,
///              anything longer or quote/CR/LF/NUL traps).
/// @return Caller-owned element-owning Seq of field strings, or NULL after a
///         syntax/allocation failure.
void *rt_csv_parse_line_with(rt_string line, rt_string delim);

/// @brief Parse multi-line CSV text into a Seq of Seq of strings.
/// @details Uses comma and accepts CRLF, LF, or CR record endings. Empty physical
///          records produce a one-field row containing an empty string.
/// @param text Multi-line CSV text; NULL or empty yields an empty outer Seq.
/// @return Caller-owned element-owning Seq of row Seqs, or NULL after a
///         syntax/allocation failure.
void *rt_csv_parse(rt_string text);

/// @brief Parse multi-line CSV text with custom delimiter.
/// @param text Multi-line CSV text; NULL or empty yields an empty outer Seq.
/// @param delim Delimiter string; must be exactly one byte (empty selects the comma default,
///              anything longer or quote/CR/LF/NUL traps).
/// @return Caller-owned element-owning Seq of row Seqs, or NULL after a
///         syntax/allocation failure.
void *rt_csv_parse_with(rt_string text, rt_string delim);

/// @brief Format a Seq of strings into a CSV line.
/// @details Uses comma, stringifies non-string runtime values, and quotes fields
///          containing comma, quote, CR, or LF while doubling internal quotes.
/// @param fields Seq of field values; NULL or empty yields an empty string.
/// @return Newly allocated CSV record without a line terminator, or an empty
///         fallback after validation/overflow/allocation failure.
rt_string rt_csv_format_line(void *fields);

/// @brief Format a Seq of strings into a CSV line with custom delimiter.
/// @param fields Seq of field values; NULL or empty yields an empty string.
/// @param delim Delimiter string; must be exactly one byte (empty selects the comma default,
///              anything longer or quote/CR/LF/NUL traps).
/// @return Newly allocated CSV record without a line terminator, or an empty
///         fallback after validation/overflow/allocation failure.
rt_string rt_csv_format_line_with(void *fields, rt_string delim);

/// @brief Format a Seq of Seq of strings into multi-line CSV text.
/// @details Uses comma and appends LF after every row, including the last.
///          Unequal row widths are permitted and non-string fields are stringified.
/// @param rows Seq of row Seqs; NULL or empty yields an empty string.
/// @return Newly allocated CSV text, or an empty fallback after validation,
///         overflow, or allocation failure.
rt_string rt_csv_format(void *rows);

/// @brief Format a Seq of Seq of strings with custom delimiter.
/// @param rows Seq of row Seqs; NULL or empty yields an empty string.
/// @param delim Delimiter string; must be exactly one byte (empty selects the comma default,
///              anything longer or quote/CR/LF/NUL traps).
/// @return Newly allocated LF-terminated CSV text, or an empty fallback after
///         validation, overflow, or allocation failure.
rt_string rt_csv_format_with(void *rows, rt_string delim);

/// @brief Check whether multi-line CSV text is syntactically valid.
/// @details Performs an allocation-free comma-delimited grammar pass accepting
///          CRLF, LF, and CR endings.
/// @param text CSV text; NULL is invalid and empty text is valid.
/// @return 1 if syntactically valid, otherwise 0.
int8_t rt_csv_is_valid(rt_string text);

#ifdef __cplusplus
}
#endif
