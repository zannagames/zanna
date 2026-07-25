//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_internal.h
// Purpose: Declares runtime-internal byte-length, allocation, live-handle
// registry, empty-singleton, and UTF-8 traversal helpers shared across string
// and text-format implementation units.
//
// Key invariants:
//   - This header is INTERNAL to the runtime — never include from public APIs.
//   - Stored string length excludes the separate trailing NUL and may count
//     arbitrary bytes, including embedded NUL or malformed UTF-8.
//   - Allocated capacity includes terminator space and is normalized to at
//     least len + 1.
//   - Live-handle registry mutation and lookup are synchronized internally;
//     registry shutdown requires runtime-wide quiescence.
//   - Strict UTF-8 helpers accept only canonical Unicode scalar encodings.
//
// Ownership/Lifetime:
//   - rt_string_alloc returns one owned mutable handle using embedded or
//     heap-backed storage.
//   - rt_empty_string returns a lazily initialized immortal shared singleton.
//   - UTF-8 and length helpers borrow all input spans and string handles.
//
// Links: src/runtime/core/rt_string.h (public API),
//        src/runtime/core/rt_string_ops.c (core operations),
//        src/runtime/core/rt_string_advanced.c (extended operations),
//        src/runtime/core/rt_string_specialized.c (case conversion + distance)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Internal runtime string allocation, registry, and UTF-8 helpers.
#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

/// @brief Report the byte length of a runtime string payload.
/// @details Reads the inline length for literal/embedded handles and the heap
///          header length for heap-backed handles. Null is treated as empty;
///          invalid or corrupt handles trap and fall back to zero if the trap
///          hook returns.
/// @param s Borrowed runtime string; may be NULL.
/// @return Stored payload length excluding the trailing terminator.
size_t rt_string_len_bytes(rt_string s);

/// @brief Allocate a new mutable string with given length and capacity.
/// @details Uses embedded storage when both length and requested capacity fit
///          the small-string limits; otherwise allocates a heap payload and
///          wrapper. Capacity below `len + 1` is raised to that minimum. The
///          payload byte at @p len is initialized to NUL; earlier bytes are not
///          initialized by this API.
/// @param len Initial stored payload length.
/// @param cap Requested total byte capacity including terminator space.
/// @return Owned registered string handle, or `NULL` after overflow/allocation
///         failure.
rt_string rt_string_alloc(size_t len, size_t cap);

/// @brief Return the cached empty string singleton.
/// @details Initializes an immortal heap-backed handle once using an atomic
///          publish race; losing candidates are unregistered and released.
/// @return Shared immortal zero-length string, or `NULL` after allocation or
///         registry failure.
rt_string rt_empty_string(void);

/// @brief Register a live runtime string handle for safe ownership checks.
/// @details Inserts by pointer into the synchronized open-addressed registry.
///          Duplicate insertion and null input are no-ops. Registry growth
///          failure traps after releasing its lock.
/// @param s Live runtime string handle; may be NULL.
void rt_string_register_handle(rt_string s);

/// @brief Remove a string handle from the live-handle registry before free.
/// @details Replaces a matching slot with a tombstone under the registry lock.
///          Null and already-absent handles are ignored.
/// @param s Runtime string handle being destroyed; may be NULL.
void rt_string_unregister_handle(rt_string s);

/// @brief Release registry storage used for live string-handle validation.
/// @details Called during process shutdown after runtime-owned strings have
///          been released. Clears counts and slots but not the registry lock.
///          Subsequent registrations lazily allocate a fresh table.
/// @warning No thread may concurrently register, unregister, or validate a
///          handle while shutdown runs.
void rt_string_registry_shutdown(void);

/// @brief Convert a character (codepoint) position to a byte offset in a UTF-8 string.
/// @details Strictly validates each traversed sequence. Null data or positions
///          at/below one map to zero; positions beyond the codepoint count map
///          to @p byte_len. Invalid UTF-8 traps and returns @p byte_len if the
///          hook returns.
/// @param data Borrowed UTF-8 byte span; may be NULL.
/// @param byte_len Number of available bytes.
/// @param char_pos One-based codepoint position.
/// @return Zero-based byte offset clamped to @p byte_len.
size_t utf8_char_to_byte_offset(const char *data, size_t byte_len, int64_t char_pos);

/// @brief Infer a UTF-8 sequence width from one leading byte.
/// @details Examines only the lead-bit pattern and does not validate following
///          bytes, overlong encodings, surrogate values, or Unicode range.
///          Invalid lead patterns trap.
/// @param c Candidate leading byte.
/// @return Implied width from one through four, or zero after a returning trap.
size_t utf8_char_len(unsigned char c);

/// @brief Validate and measure one canonical UTF-8 sequence.
/// @details Rejects invalid leads/continuations, truncation, overlong forms,
///          UTF-16 surrogates, and scalars above U+10FFFF without trapping.
/// @param data Borrowed span beginning at the candidate lead byte; may be NULL.
/// @param remaining Number of readable bytes in @p data.
/// @return Sequence width from one through four, or zero when invalid.
size_t rt_utf8_strict_step(const char *data, size_t remaining);

/// @brief Validate that a byte span is well-formed UTF-8.
/// @details Rejects invalid lead bytes, truncated/invalid continuation bytes,
///          overlong encodings, surrogate code points, and values above
///          U+10FFFF. Shared by text-format validators (TOML/YAML) so their
///          acceptance matches the standards' UTF-8 stream requirement.
///          Embedded NUL is valid ASCII. A null pointer is accepted only for an
///          empty span, and this predicate never traps.
/// @param data Borrowed span start; may be NULL only when @p len is zero.
/// @param len Number of bytes to validate.
/// @return One when the complete span is valid strict UTF-8; otherwise zero.
int rt_utf8_span_valid(const char *data, size_t len);
