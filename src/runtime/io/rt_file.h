//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file.h
// Purpose: File I/O API with BASIC OPEN/CLOSE/INPUT/PRINT# semantics, channel-based access, RtError
// integration, and POSIX file descriptor backing.
//
// Key invariants:
//   - RtFile.fd == -1 means closed or uninitialized.
//   - Channel numbers map to at most one open file at a time.
//   - All operations report errors via RtError out-parameters.
//   - Files are NOT automatically closed; callers must call rt_file_close.
//   - A close attempt always consumes the descriptor, including host-close failures.
//
// Ownership/Lifetime:
//   - RtFile handles are stack-allocated by callers and initialized via rt_file_init.
//   - OS resources are released by rt_file_close.
//   - The channel-based API manages its own internal file table.
//
// Links: src/runtime/io/rt_file.c (implementation), src/runtime/core/rt_error.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares low-level file handles and BASIC channel-oriented file I/O.
 * @details Defines reusable descriptor state, BASIC OPEN modes, structured-
 * error operations for byte, line, seek, and write access, and the legacy
 * runtime channel ABI managed per execution context.
 */
#pragma once

#include "rt_error.h"
#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Lightweight handle representing an open runtime file.
typedef struct RtFile {
    int fd; ///< Underlying POSIX file descriptor or -1 when closed.
} RtFile;

/// @brief Initialize @p file to a closed handle state.
/// @param file Handle to initialize; ignored when NULL.
void rt_file_init(RtFile *file);

/// @brief Open @p path with the specified @p mode string and BASIC semantics.
/// @param file Output handle populated on success.
/// @param path Null-terminated filesystem path.
/// @param mode fopen-style mode string (e.g., "r", "w", "a", variants with '+').
/// @param basic_mode BASIC OPEN mode enumerator when available; pass RT_F_UNSPECIFIED for plain
/// stdio semantics.
/// @param out_err Optional error record receiving failure details.
/// @return 1 on success; 0 when an error is reported via @p out_err.
int8_t rt_file_open(
    RtFile *file, const char *path, const char *mode, int32_t basic_mode, RtError *out_err);

/// @brief Close @p file when open.
/// @details Invalidates the descriptor before invoking the host close operation, so @p file is
///          reusable even when the host reports a close error. Callers must never retry a failed
///          close using a saved raw descriptor number because that number may have been reused.
/// @param file Handle to close; remains valid for reuse after every close attempt.
/// @param out_err Optional error record receiving failure details.
/// @return 1 on success or when already closed; 0 when the host close operation fails.
int8_t rt_file_close(RtFile *file, RtError *out_err);

/// @brief Read a single byte from @p file.
/// @param file Handle to read.
/// @param out_byte Output byte when successful.
/// @param out_err Optional error record receiving failure details.
/// @return 1 when a byte is read; 0 on EOF or error.
int8_t rt_file_read_byte(RtFile *file, uint8_t *out_byte, RtError *out_err);

/// @brief Read a single line terminated by '\n' (newline excluded) from @p file.
/// @param file Handle to read.
/// @param out_line Receives newly allocated runtime string; NULL on failure.
/// @param out_err Optional error record receiving failure details.
/// @return 1 when a line is produced; 0 on EOF or error.
int8_t rt_file_read_line(RtFile *file, rt_string *out_line, RtError *out_err);

/// @brief Seek to @p offset relative to @p origin within @p file.
/// @param file Handle to reposition.
/// @param offset Byte offset to apply.
/// @param origin One of SEEK_SET, SEEK_CUR, or SEEK_END.
/// @param out_err Optional error record receiving failure details.
/// @return 1 on success; 0 on error.
int8_t rt_file_seek(RtFile *file, int64_t offset, int origin, RtError *out_err);

/// @brief Write @p len bytes from @p data to @p file.
/// @param file Handle to write.
/// @param data Buffer containing bytes to write.
/// @param len Number of bytes to write.
/// @param out_err Optional error record receiving failure details.
/// @return 1 when the entire buffer is written; 0 otherwise.
int8_t rt_file_write(RtFile *file, const uint8_t *data, size_t len, RtError *out_err);

/// @brief Enumerates BASIC OPEN modes understood by the runtime wrapper API.
enum RtFileMode {
    RT_F_UNSPECIFIED = -1, ///< Mode not associated with BASIC OPEN semantics.
    RT_F_INPUT = 0,        ///< OPEN ... FOR INPUT
    RT_F_OUTPUT = 1,       ///< OPEN ... FOR OUTPUT
    RT_F_APPEND = 2,       ///< OPEN ... FOR APPEND
    RT_F_BINARY = 3,       ///< OPEN ... FOR BINARY
    RT_F_RANDOM = 4,       ///< OPEN ... FOR RANDOM
};

/// @brief Open @p path for the specified BASIC @p mode on @p channel.
/// @param path Runtime string describing the filesystem path.
/// @param mode Mode enumerator matching BASIC OPEN semantics.
/// @param channel Numeric channel identifier provided by the caller.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_open_err_vstr(ZannaString *path, int32_t mode, int32_t channel);

/// @brief Close the runtime file associated with @p channel when present.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_close_err(int32_t channel);

/// @brief Write @p s to the file bound to @p channel without a trailing newline.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @param s Runtime string to write; NULL or invalid strings return Err_InvalidOperation.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_write_ch_err(int32_t channel, ZannaString *s);

/// @brief Write @p s to the file bound to @p channel followed by a newline.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @param s Runtime string to write; NULL or invalid strings return Err_InvalidOperation.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_println_ch_err(int32_t channel, ZannaString *s);

/// @brief Read a line of text from @p channel into a newly allocated runtime string.
/// @param channel Numeric channel identifier previously passed to OPEN FOR INPUT.
/// @param out Receives allocated string on success; set to NULL on failure.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_line_input_ch_err(int32_t channel, ZannaString **out);

/// @brief Query the file descriptor associated with @p channel.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @param out_fd Receives the file descriptor on success.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_file_channel_fd(int32_t channel, int *out_fd);

/// @brief Read the cached EOF flag for @p channel.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @param out_at_eof Receives the cached EOF state on success.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_file_channel_get_eof(int32_t channel, int8_t *out_at_eof);

/// @brief Update the cached EOF flag for @p channel.
/// @param channel Numeric channel identifier previously passed to OPEN.
/// @param at_eof New EOF state to record.
/// @return 0 on success; error code aligned with @ref Err otherwise.
int32_t rt_file_channel_set_eof(int32_t channel, int8_t at_eof);

#ifdef __cplusplus
}
#endif
