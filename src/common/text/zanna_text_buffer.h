//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/text/zanna_text_buffer.h
// Purpose: C ABI facade for Zanna's shared piece-table text buffer.
// Key invariants: The opaque handle owns all editable text, line-index state,
//                 and undo/redo history; callers exchange byte offsets and
//                 explicit byte spans so embedded NUL bytes are preserved.
// Ownership/Lifetime: Handles returned by zanna_text_buffer_new must be released
//                     with zanna_text_buffer_free. Strings returned by this API
//                     must be released with zanna_text_buffer_free_string.
// Links: src/tui/include/tui/text/text_buffer.hpp,
//        src/common/text/zanna_text_buffer.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file zanna_text_buffer.h
 * @brief Declares the exception-safe C ABI for Zanna's shared text buffer.
 *
 * The API uses opaque handles, explicit byte spans, and caller-visible lengths
 * so C consumers can edit and retrieve data containing embedded NUL bytes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque C handle for the shared Zanna text buffer.
 *
 * @details The concrete implementation is a C++ piece-table based buffer that
 * owns its storage, line index, and edit history. The C ABI intentionally keeps
 * the type incomplete so C callers cannot depend on C++ layout details.
 */
typedef struct zanna_text_buffer zanna_text_buffer_t;

/**
 * @brief Allocate an empty shared text buffer.
 *
 * @details The returned buffer contains a single empty logical line and has no
 * undo or redo history. The function catches allocation failures and never lets
 * C++ exceptions cross the C ABI.
 *
 * @return Newly allocated buffer, or NULL when allocation fails.
 */
zanna_text_buffer_t *zanna_text_buffer_new(void);

/**
 * @brief Release a shared text buffer allocated by zanna_text_buffer_new.
 *
 * @details Passing NULL is allowed and has no effect. All text storage, line
 * index data, and undo/redo history owned by the buffer are destroyed.
 *
 * @param buffer Buffer handle to release.
 */
void zanna_text_buffer_free(zanna_text_buffer_t *buffer);

/**
 * @brief Replace the complete buffer contents with an explicit byte span.
 *
 * @details Loading text resets the piece table, rebuilds the line index, and
 * clears undo/redo history. Embedded NUL bytes are copied into the buffer. A
 * NULL @p bytes pointer is valid only when @p len is zero.
 *
 * @param buffer Buffer to update.
 * @param bytes Source bytes to copy, or NULL when @p len is zero.
 * @param len Number of bytes to read from @p bytes.
 * @return true on success; false for invalid arguments or allocation failure.
 */
bool zanna_text_buffer_load_bytes(zanna_text_buffer_t *buffer, const char *bytes, size_t len);

/**
 * @brief Insert an explicit byte span at a byte offset.
 *
 * @details The insertion updates the piece table and line index atomically. If
 * a transaction is active, the operation is recorded for grouped undo. A NULL
 * @p bytes pointer is valid only when @p len is zero.
 *
 * @param buffer Buffer to mutate.
 * @param pos Zero-based byte offset where the insertion starts.
 * @param bytes Bytes to insert, or NULL when @p len is zero.
 * @param len Number of bytes to insert.
 * @return true on success; false for invalid arguments or allocation failure.
 */
bool zanna_text_buffer_insert_bytes(zanna_text_buffer_t *buffer,
                                    size_t pos,
                                    const char *bytes,
                                    size_t len);

/**
 * @brief Erase a byte range from the buffer.
 *
 * @details The erase operation clamps according to the underlying text buffer's
 * range rules, updates the line index, and records undo data when a transaction
 * is active.
 *
 * @param buffer Buffer to mutate.
 * @param pos Zero-based byte offset where erasure starts.
 * @param len Number of bytes to erase.
 * @return true on success; false for invalid arguments or allocation failure.
 */
bool zanna_text_buffer_erase(zanna_text_buffer_t *buffer, size_t pos, size_t len);

/**
 * @brief Begin a grouped edit transaction.
 *
 * @details Inserts and erases performed before zanna_text_buffer_end_transaction
 * are committed as one undo step. Passing NULL has no effect.
 *
 * @param buffer Buffer whose history should begin a transaction.
 */
void zanna_text_buffer_begin_transaction(zanna_text_buffer_t *buffer);

/**
 * @brief End the active grouped edit transaction.
 *
 * @details If the transaction contains edits, it becomes one undo step. Empty
 * transactions are discarded by the underlying history. Passing NULL has no
 * effect.
 *
 * @param buffer Buffer whose active transaction should be closed.
 */
void zanna_text_buffer_end_transaction(zanna_text_buffer_t *buffer);

/**
 * @brief Undo the most recent committed transaction.
 *
 * @details The buffer's text, line index, and redo stack are updated together.
 *
 * @param buffer Buffer to mutate.
 * @return true when an edit was undone; false when no undo was available or
 *         @p buffer is NULL.
 */
bool zanna_text_buffer_undo(zanna_text_buffer_t *buffer);

/**
 * @brief Redo the most recently undone transaction.
 *
 * @details The buffer's text, line index, and undo stack are updated together.
 *
 * @param buffer Buffer to mutate.
 * @return true when an edit was redone; false when no redo was available or
 *         @p buffer is NULL.
 */
bool zanna_text_buffer_redo(zanna_text_buffer_t *buffer);

/**
 * @brief Return the buffer size in bytes.
 *
 * @param buffer Buffer to query.
 * @return Total byte count, or zero when @p buffer is NULL.
 */
size_t zanna_text_buffer_size(const zanna_text_buffer_t *buffer);

/**
 * @brief Return the number of logical lines in the buffer.
 *
 * @details The shared text buffer reports at least one line, even for empty
 * content, matching conventional editor behavior.
 *
 * @param buffer Buffer to query.
 * @return Logical line count, or zero when @p buffer is NULL.
 */
size_t zanna_text_buffer_line_count(const zanna_text_buffer_t *buffer);

/**
 * @brief Return the byte offset where a logical line starts.
 *
 * @param buffer Buffer to query.
 * @param line_no Zero-based line index.
 * @return Line start byte offset; out-of-range lines return the buffer size.
 */
size_t zanna_text_buffer_line_start(const zanna_text_buffer_t *buffer, size_t line_no);

/**
 * @brief Return the byte offset where a logical line ends.
 *
 * @details The returned end offset excludes a trailing newline. Out-of-range
 * lines return the buffer size.
 *
 * @param buffer Buffer to query.
 * @param line_no Zero-based line index.
 * @return Exclusive line end byte offset.
 */
size_t zanna_text_buffer_line_end(const zanna_text_buffer_t *buffer, size_t line_no);

/**
 * @brief Return the byte length of a logical line.
 *
 * @details The length excludes a trailing newline. Out-of-range lines return
 * zero.
 *
 * @param buffer Buffer to query.
 * @param line_no Zero-based line index.
 * @return Line length in bytes.
 */
size_t zanna_text_buffer_line_length(const zanna_text_buffer_t *buffer, size_t line_no);

/**
 * @brief Duplicate the complete buffer text as a NUL-terminated byte string.
 *
 * @details The returned allocation includes one extra terminating NUL byte, but
 * @p out_len receives the true byte length so embedded NUL content is preserved.
 * Release the result with zanna_text_buffer_free_string.
 *
 * @param buffer Buffer to materialize.
 * @param out_len Optional output for the byte length excluding the terminator.
 * @return Caller-owned string, or NULL on failure.
 */
char *zanna_text_buffer_text_dup(const zanna_text_buffer_t *buffer, size_t *out_len);

/**
 * @brief Duplicate one logical line as a NUL-terminated byte string.
 *
 * @details The returned string excludes a trailing newline. @p out_len receives
 * the byte length excluding the added terminator. Release the result with
 * zanna_text_buffer_free_string.
 *
 * @param buffer Buffer to query.
 * @param line_no Zero-based line index.
 * @param out_len Optional output for the byte length excluding the terminator.
 * @return Caller-owned line string, or NULL on failure.
 */
char *zanna_text_buffer_line_dup(const zanna_text_buffer_t *buffer,
                                 size_t line_no,
                                 size_t *out_len);

/**
 * @brief Release a string returned by this C ABI.
 *
 * @details Passing NULL is allowed and has no effect. This function exists so
 * callers do not need to know which allocator backs returned strings.
 *
 * @param text String allocation to release.
 */
void zanna_text_buffer_free_string(char *text);

#ifdef __cplusplus
}
#endif
