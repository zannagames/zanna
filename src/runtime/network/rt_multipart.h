//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_multipart.h
// Purpose: Multipart form-data builder/parser for HTTP file uploads.
// Key invariants:
//   - Builder creates conventional CRLF-framed multipart/form-data bodies.
//   - Parser is bounded, strict, and never publishes partial malformed input.
//   - Boundary is randomly generated for each builder instance.
//   - Public receivers are validated by stable class identity before access.
// Ownership/Lifetime:
//   - Multipart objects are GC-managed and own native copies of their parts.
//   - Returned managed values are caller-owned unless their container retains them.
// Links: rt_network_http.c (consumer)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares managed multipart/form-data building and parsing.
 * @details Defines stable object identity, append-only text and file parts,
 * content-type and body serialization, strict bounded parse and Result forms,
 * and caller-owned field and file lookup values.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable private class identity stored in Multipart heap headers.
/// @details Public methods use this tag plus the complete payload size to
///          reject unrelated managed objects before reading native fields.
#define RT_MULTIPART_CLASS_ID INT64_C(-0x720209)

/// @brief Create an empty multipart/form-data builder.
/// @details Generates a random 40-character alphanumeric boundary and installs
///          a finalizer for native part copies.
/// @return Caller-owned Multipart object.
void *rt_multipart_new(void);

/// @brief Append a copied text field and return the builder for chaining.
/// @param mp Valid Multipart receiver.
/// @param name Non-empty String; embedded NUL bytes are rejected.
/// @param value String whose complete byte span, including embedded NULs, is copied.
/// @return @p mp on success or after a returning trap hook observes failure.
void *rt_multipart_add_field(void *mp, rt_string name, rt_string value);

/// @brief Append a copied binary file part and return the builder for chaining.
/// @param mp Valid Multipart receiver.
/// @param name Non-empty field-name String without embedded NUL bytes.
/// @param filename Optional filename; NULL uses `file`.
/// @param data Optional Bytes payload; NULL represents an empty file.
/// @return @p mp on success or after a returning trap hook observes failure.
void *rt_multipart_add_file(void *mp, rt_string name, rt_string filename, void *data);

/// @brief Format the Content-Type value including this builder's boundary.
/// @param mp Valid Multipart receiver.
/// @return Caller-owned String, or an empty String for a NULL receiver.
rt_string rt_multipart_content_type(void *mp);

/// @brief Serialize every part and the closing boundary into one Bytes value.
/// @details Size arithmetic is checked before allocation. Native staging is
///          released before serialization or managed-allocation traps propagate.
/// @param mp Valid Multipart receiver.
/// @return Caller-owned Bytes, or NULL after a returning trap hook observes failure.
void *rt_multipart_build(void *mp);

/// @brief Get the number of appended or parsed parts.
/// @param mp Multipart receiver; NULL reports zero.
/// @return Non-negative part count.
int64_t rt_multipart_count(void *mp);

/// @brief Parse a multipart body given content-type and body bytes.
/// @details Parsing is strict and atomic: malformed, truncated, oversized, or
///          allocation-failing input traps after releasing all partial state.
/// @param content_type String containing a valid boundary parameter.
/// @param body Bytes containing at most 64 MiB of multipart data.
/// @return Caller-owned complete Multipart, never a partial value.
void *rt_multipart_parse(rt_string content_type, void *body);

/// @brief Convert parse failures into `Result.ErrStr`.
/// @param content_type Content-Type String.
/// @param body Candidate Bytes body.
/// @return Caller-owned `Result.Ok(Multipart)` or `Result.ErrStr`.
void *rt_multipart_parse_result(rt_string content_type, void *body);

/// @brief Test whether a non-file field with the given name exists.
/// @param mp Candidate Multipart receiver.
/// @param name Exact field name without embedded NUL bytes.
/// @return One when present, otherwise zero.
int8_t rt_multipart_has_field(void *mp, rt_string name);

/// @brief Test whether a file part with the given name exists.
/// @param mp Candidate Multipart receiver.
/// @param name Exact field name without embedded NUL bytes.
/// @return One when present, otherwise zero.
int8_t rt_multipart_has_file(void *mp, rt_string name);

/// @brief Copy a field value by exact name.
/// @param mp Candidate Multipart receiver.
/// @param name Exact field name.
/// @return Caller-owned String; empty when missing or invalid.
rt_string rt_multipart_get_field(void *mp, rt_string name);

/// @brief Copy a file part's bytes by exact field name.
/// @param mp Candidate Multipart receiver.
/// @param name Exact file field name.
/// @return Caller-owned Bytes; empty when missing or invalid.
void *rt_multipart_get_file(void *mp, rt_string name);

#ifdef __cplusplus
}
#endif
