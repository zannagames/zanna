//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_linereader.c
// Purpose: Implements line-by-line text file reading for the Zanna.IO.LineReader
//          class. Reads text files one line at a time with correct handling of
//          all three common line ending styles: LF (Unix), CR (classic Mac),
//          and CRLF (Windows).
//
// Key invariants:
//   - LF (\n), CR (\r), and CRLF (\r\n) are all recognized as line terminators.
//   - Returned line strings do not include the line terminator character(s).
//   - The EOF flag is set after the last line is consumed; subsequent reads
//     return a fresh empty string.
//   - A one-character peek buffer is used to detect CRLF without double-reads.
//   - The closed flag prevents double-close and operations on a closed reader.
//   - The GC finalizer closes the FILE* if the caller forgets to call Close.
//
// Ownership/Lifetime:
//   - LineReader objects are heap-allocated; the GC calls the finalizer on free.
//   - Each returned rt_string line is a fresh allocation owned by the caller.
//
// Links: src/runtime/io/rt_linereader.h (public API),
//        src/runtime/io/rt_linewriter.h (complementary text file writer)
//
//===----------------------------------------------------------------------===//

#include "rt_linereader.h"

#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Use 64-bit seek/tell to support files larger than 2 GB on Windows
// where `long` (and thus ftell/fseek) is only 32 bits even on 64-bit builds.
#if defined(_WIN32)
#define lr_fseek(fp, off, whence) _fseeki64((fp), (off), (whence))
#define lr_ftell(fp) _ftelli64((fp))
#else
#define lr_fseek(fp, off, whence) fseeko((fp), (off_t)(off), (whence))
#define lr_ftell(fp) ftello((fp))
#endif

/// @brief LineReader implementation structure.
typedef struct rt_linereader_impl {
    FILE *fp;       ///< File pointer.
    int8_t eof;     ///< EOF flag.
    int8_t closed;  ///< Closed flag.
    int peeked;     ///< Peeked character (-1 if none, or 0-255).
    int has_peeked; ///< Whether we have a peeked character.
} rt_linereader_impl;

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

/// @brief Validate and unwrap an opaque LineReader receiver.
/// @details Checks both the runtime class identifier and complete payload size before casting.
///          Invalid receivers trap with @p context or a generic fallback.
/// @param obj Borrowed opaque runtime receiver.
/// @param context Trap diagnostic for an invalid receiver; may be NULL.
/// @return Pointer to the validated inline implementation payload, or NULL as trap-control
///         fallback.
static rt_linereader_impl *linereader_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_LINEREADER_CLASS_ID, sizeof(rt_linereader_impl))) {
        rt_trap(context ? context : "LineReader: invalid reader");
        return NULL;
    }
    return (rt_linereader_impl *)obj;
}

/// @brief Preserve a recovered trap message in caller-owned storage.
/// @param buffer Destination buffer that remains valid after recovery is cleared.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when the trap subsystem has no current text.
static void linereader_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Allocate a LineReader object while retaining ownership of an open file.
/// @details Managed object allocation reports OOM through the runtime trap
///          recovery mechanism. This helper keeps the native `FILE *` in a
///          volatile owner slot so every non-local exit closes it before the
///          original diagnostic is propagated. A returning trap hook and a
///          direct NULL allocation receive the same cleanup treatment.
/// @param fp Open native stream whose ownership transfers on success.
/// @return Fresh LineReader payload, or NULL after closing @p fp and trapping.
static rt_linereader_impl *linereader_alloc_or_close(FILE *fp) {
    FILE *volatile owned_fp = fp;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        linereader_save_trap_error(
            saved_error, sizeof(saved_error), "LineReader.Open: memory allocation failed");
        rt_trap_clear_recovery();
        fclose((FILE *)owned_fp);
        rt_trap(saved_error);
        return NULL;
    }

    rt_linereader_impl *lr = (rt_linereader_impl *)rt_obj_new_i64(
        RT_LINEREADER_CLASS_ID, (int64_t)sizeof(rt_linereader_impl));
    rt_trap_clear_recovery();
    if (!lr) {
        fclose(fp);
        rt_trap("LineReader.Open: memory allocation failed");
        return NULL;
    }
    return lr;
}

/// @brief Convert an owned temporary byte buffer to a managed runtime string.
/// @details String construction may trap while allocating either its wrapper
///          or backing payload. The recovery boundary frees @p buffer on every
///          exit, including non-local OOM propagation, so successful reads do
///          not leak their staging allocation when result construction fails.
/// @param buffer Malloc-owned bytes to copy; ownership always transfers here.
/// @param len Number of valid bytes in @p buffer.
/// @param fallback Diagnostic used if no recovered trap message is available.
/// @return Fresh runtime string, or NULL after cleanup and trap propagation.
static rt_string linereader_string_from_owned_buffer(char *buffer,
                                                     size_t len,
                                                     const char *fallback) {
    char *volatile owned_buffer = buffer;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        linereader_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        free((void *)owned_buffer);
        rt_trap(saved_error);
        return NULL;
    }

    rt_string result = rt_string_from_bytes(buffer, len);
    rt_trap_clear_recovery();
    free((void *)owned_buffer);
    if (!result) {
        rt_trap(fallback);
        return NULL;
    }
    return result;
}

/// @brief Finalizer callback invoked when a LineReader is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// LineReader object becomes unreachable. It ensures that the underlying
/// operating system file handle is properly closed to prevent resource leaks.
///
/// The finalizer is a safety net - well-written programs should call
/// rt_linereader_close explicitly when done reading. However, if the program
/// forgets to close the reader or an exception occurs, this finalizer ensures
/// the file is eventually closed when the object is collected.
///
/// @param obj Pointer to the LineReader object being finalized. May be NULL (no-op).
///
/// @note This function is idempotent - calling it on an already-closed reader is safe.
/// @note The finalizer does not raise errors; it silently closes the file if open.
///
/// @see rt_linereader_close For explicit closure
/// @see rt_obj_set_finalizer For how finalizers are registered
static void rt_linereader_finalize(void *obj) {
    if (!obj)
        return;
    rt_linereader_impl *lr = (rt_linereader_impl *)obj;
    if (lr->fp && !lr->closed) {
        fclose(lr->fp);
        lr->fp = NULL;
        lr->closed = 1;
    }
}

/// @brief Opens a text file for line-by-line reading.
///
/// Creates a new LineReader object connected to the specified file path. The file
/// is opened in binary mode so newline bytes are handled uniformly by this module.
/// The returned LineReader provides convenient
/// methods for reading lines, characters, or the entire file content.
///
/// The LineReader is managed by Zanna's garbage collector and will automatically
/// close when collected if not explicitly closed.
///
/// **Line ending handling:**
/// The LineReader handles all common line ending formats:
/// - LF (`\n`): Unix/Linux/macOS standard
/// - CR (`\r`): Classic Mac OS
/// - CRLF (`\r\n`): Windows/DOS standard
///
/// When reading lines, the line ending characters are stripped from the result.
///
/// **Usage example:**
/// ```
/// Dim reader = LineReader.Open("data.txt")
/// While Not reader.EOF()
///     Dim line = reader.Read()
///     Print line
/// Wend
/// reader.Close()
/// ```
///
/// @param path Zanna string containing the file path. Must not be NULL.
///             Path is interpreted according to the OS (relative or absolute).
///
/// @return A pointer to a new LineReader object on success. On failure, traps
///         with an error message and returns NULL. Failure reasons include:
///         - NULL path string
///         - Invalid path string
///         - File cannot be opened (doesn't exist, no permission, etc.)
///         - Memory allocation failure
///
/// @note The LineReader reads the file sequentially - there is no seek operation.
/// @note Files are opened in binary mode; LF, CR, and CRLF recognition is explicit.
/// @note Thread safety: Not thread-safe. Each thread should have its own LineReader.
///
/// @see rt_linereader_close For closing the reader
/// @see rt_linereader_read For reading lines
/// @see rt_linereader_read_all For reading the entire file
void *rt_linereader_open(rt_string path) {
    if (!path) {
        rt_trap("LineReader.Open: null path");
        return NULL;
    }

    const char *path_str = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &path_str) || !path_str) {
        rt_trap("LineReader.Open: invalid path");
        return NULL;
    }

    FILE *fp = rt_file_stdio_open_utf8(path_str, "rb");
    if (!fp) {
        rt_trap("LineReader.Open: failed to open file");
        return NULL;
    }

    rt_linereader_impl *lr = linereader_alloc_or_close(fp);
    if (!lr)
        return NULL;

    lr->fp = fp;
    lr->eof = 0;
    lr->closed = 0;
    lr->peeked = -1;
    lr->has_peeked = 0;
    rt_obj_set_finalizer(lr, rt_linereader_finalize);

    return lr;
}

/// @brief Explicitly closes a LineReader, releasing the underlying file handle.
///
/// Closes the file associated with this LineReader object. After calling close,
/// any subsequent read operations on this LineReader will trap with an error.
///
/// It is good practice to explicitly close readers when done, rather than relying
/// on the garbage collector:
/// - Immediate release of OS file handle resources
/// - Avoids potential resource exhaustion with many open files
/// - Makes program behavior more predictable
///
/// @param obj Pointer to a LineReader object. If NULL, this function is a no-op.
///
/// @note This function is idempotent - calling close on an already-closed
///       LineReader does nothing (no error).
/// @note After closing, the LineReader object still exists in memory but is
///       unusable. It will be freed when the garbage collector runs.
/// @note Thread safety: Not thread-safe. Don't close a reader while another
///       thread is using it.
///
/// @see rt_linereader_open For opening files
void rt_linereader_close(void *obj) {
    if (!obj)
        return;

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return;
    if (lr->fp && !lr->closed) {
        fclose(lr->fp);
        lr->fp = NULL;
        lr->closed = 1;
    }
}

/// @brief Internal helper: gets the next character, consuming any peeked character first.
///
/// This function provides a unified character-reading interface that handles
/// the internal peek buffer. When rt_linereader_peek_char or the CR/LF handling
/// logic needs to "put back" a character, it goes into the peek buffer. This
/// function checks that buffer first before reading from the file.
///
/// @param lr Pointer to the LineReader implementation. Must not be NULL.
///
/// @return The next character (0-255), or EOF if end of file is reached.
///
/// @note This is an internal function, not part of the public API.
static int lr_getc(rt_linereader_impl *lr) {
    if (lr->has_peeked) {
        lr->has_peeked = 0;
        return lr->peeked;
    }
    return fgetc(lr->fp);
}

/// @brief Reads the next line from the file.
///
/// Reads characters from the current file position until a line ending (LF, CR,
/// or CRLF) is encountered or end-of-file is reached. The line ending characters
/// are consumed but NOT included in the returned string.
///
/// **Line ending handling in detail:**
/// - LF (`\n`): Consumed, line ends
/// - CR (`\r`): Consumed, line ends. If followed by LF, that's consumed too.
/// - CRLF (`\r\n`): Both characters consumed as a single line ending
/// - CR + other: CR consumed as line end, other char available for next read
///
/// **Memory management:**
/// The function uses a dynamically growing buffer (starting at 256 bytes) to
/// handle lines of any length. The returned string is a new Zanna string that
/// the caller owns.
///
/// **EOF behavior:**
/// - If EOF is reached with content, returns that content and sets EOF flag
/// - If EOF is reached with no content (already at EOF), returns empty string
/// - Use rt_linereader_eof to check if more lines are available
///
/// **Example usage:**
/// ```
/// Dim reader = LineReader.Open("data.txt")
/// While Not reader.EOF()
///     Dim line = reader.Read()
///     If line <> "" Then Print line
/// Wend
/// reader.Close()
/// ```
///
/// @param obj Pointer to a LineReader object. Must not be NULL and reader must be open.
///
/// @return A Zanna string containing the line content (without line ending).
///         Returns an empty string if at EOF or on error.
///         Traps if obj is NULL or reader is closed.
///
/// @note Very long lines may cause significant memory allocation.
/// @note Empty lines (just a line ending) return an empty string but EOF is not set.
/// @note Thread safety: Not thread-safe for the same LineReader object.
///
/// @see rt_linereader_read_char For character-by-character reading
/// @see rt_linereader_read_all For reading the entire remaining file
/// @see rt_linereader_eof For checking end-of-file status
rt_string rt_linereader_read(void *obj) {
    if (!obj) {
        rt_trap("LineReader.Read: null reader");
        return rt_string_from_bytes("", 0);
    }

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return rt_string_from_bytes("", 0);
    if (!lr->fp || lr->closed) {
        rt_trap("LineReader.Read: reader is closed");
        return rt_string_from_bytes("", 0);
    }

    // Dynamic buffer for the line
    size_t cap = 256;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        rt_trap("LineReader.Read: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    int c;
    while ((c = lr_getc(lr)) != EOF) {
        if (c == '\n') {
            // LF or end of CRLF - line complete
            break;
        } else if (c == '\r') {
            // CR - check for CRLF
            int next = fgetc(lr->fp);
            if (next == EOF && ferror(lr->fp)) {
                free(buf);
                rt_trap("LineReader.Read: read failed");
                return rt_string_from_bytes("", 0);
            }
            if (next != '\n' && next != EOF) {
                // Standalone CR, put back the next char
                lr->peeked = next;
                lr->has_peeked = 1;
            }
            // Either way, line is complete
            break;
        } else {
            // Regular character - add to buffer
            if (len >= cap - 1) {
                // Guard against unbounded growth from files with no newlines.
#define RT_LINEREADER_MAX_LINE (256 * 1024 * 1024)
                if (cap >= RT_LINEREADER_MAX_LINE) {
                    free(buf);
                    rt_trap("LineReader.Read: line length exceeds maximum (256 MiB)");
                    return rt_string_from_bytes("", 0);
                }
                cap *= 2;
                char *new_buf = (char *)realloc(buf, cap);
                if (!new_buf) {
                    free(buf);
                    rt_trap("LineReader.Read: memory allocation failed");
                    return rt_string_from_bytes("", 0);
                }
                buf = new_buf;
            }
            buf[len++] = (char)c;
        }
    }

    if (c == EOF && len == 0) {
        if (ferror(lr->fp)) {
            free(buf);
            rt_trap("LineReader.Read: read failed");
            return rt_string_from_bytes("", 0);
        }
        // EOF with no content - set EOF flag
        lr->eof = 1;
        free(buf);
        return rt_string_from_bytes("", 0);
    }

    if (c == EOF) {
        if (ferror(lr->fp)) {
            free(buf);
            rt_trap("LineReader.Read: read failed");
            return rt_string_from_bytes("", 0);
        }
        // Got content but hit EOF
        lr->eof = 1;
    } else if (!lr->has_peeked) {
        int next = lr_getc(lr);
        if (next == EOF) {
            if (ferror(lr->fp)) {
                free(buf);
                rt_trap("LineReader.Read: read failed");
                return rt_string_from_bytes("", 0);
            }
            lr->eof = 1;
        } else {
            lr->peeked = next;
            lr->has_peeked = 1;
            lr->eof = 0;
        }
    }

    return linereader_string_from_owned_buffer(
        buf, len, "LineReader.Read: string allocation failed");
}

/// @brief Reads a single character from the file.
///
/// Reads and consumes one character from the current file position, advancing
/// the position by one byte. This provides byte-level access to the file content,
/// useful for implementing custom parsing or reading binary-like data.
///
/// Unlike rt_linereader_read, this function does NOT interpret line endings -
/// CR and LF are returned as their byte values (13 and 10 respectively).
///
/// @param obj Pointer to a LineReader object. Must not be NULL and reader must be open.
///
/// @return The character value (0-255) on success, or -1 if:
///         - End of file is reached (EOF flag is also set)
///         Traps and returns -1 if obj is NULL or reader is closed.
///
/// @note The EOF flag is set when end-of-file is encountered.
/// @note Thread safety: Not thread-safe for the same LineReader object.
///
/// @see rt_linereader_peek_char For reading without consuming
/// @see rt_linereader_read For line-oriented reading
int64_t rt_linereader_read_char(void *obj) {
    if (!obj) {
        rt_trap("LineReader.ReadChar: null reader");
        return -1;
    }

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return -1;
    if (!lr->fp || lr->closed) {
        rt_trap("LineReader.ReadChar: reader is closed");
        return -1;
    }

    int c = lr_getc(lr);
    if (c == EOF) {
        if (ferror(lr->fp)) {
            rt_trap("LineReader.ReadChar: read failed");
            return -1;
        }
        lr->eof = 1;
        return -1;
    }

    return (int64_t)(unsigned char)c;
}

/// @brief Peeks at the next character without consuming it.
///
/// Returns the next character that would be read, but does not advance the
/// file position. The peeked character will be returned again on the next
/// call to rt_linereader_read_char or consumed during rt_linereader_read.
///
/// Multiple calls to PeekChar without intervening reads return the same character.
///
/// **Use cases:**
/// - Look-ahead parsing (check next char before deciding how to proceed)
/// - Conditional reading (peek, then read only if condition is met)
/// - Implementing tokenizers that need to see the next character
///
/// **Example:**
/// ```
/// Dim c = reader.PeekChar()
/// If c = Asc("[") Then
///     ' Start of bracketed section
///     reader.ReadChar()  ' consume the '['
///     ParseBracketedContent(reader)
/// End If
/// ```
///
/// @param obj Pointer to a LineReader object. Must not be NULL and reader must be open.
///
/// @return The next character value (0-255) without consuming it, or -1 if:
///         - End of file is reached (EOF flag is also set)
///         Traps and returns -1 if obj is NULL or reader is closed.
///
/// @note The peeked character is stored internally; peeking multiple times
///       without reading returns the same value.
/// @note Thread safety: Not thread-safe for the same LineReader object.
///
/// @see rt_linereader_read_char For reading and consuming a character
int64_t rt_linereader_peek_char(void *obj) {
    if (!obj) {
        rt_trap("LineReader.PeekChar: null reader");
        return -1;
    }

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return -1;
    if (!lr->fp || lr->closed) {
        rt_trap("LineReader.PeekChar: reader is closed");
        return -1;
    }

    if (lr->has_peeked) {
        return (int64_t)lr->peeked;
    }

    int c = fgetc(lr->fp);
    if (c == EOF) {
        if (ferror(lr->fp)) {
            rt_trap("LineReader.PeekChar: read failed");
            return -1;
        }
        lr->eof = 1;
        return -1;
    }

    lr->peeked = c;
    lr->has_peeked = 1;
    return (int64_t)(unsigned char)c;
}

/// @brief Reads the entire remaining file content as a single string.
///
/// Reads all remaining bytes from the current file position to the end of
/// file and returns them as a Zanna string. This is useful when you need
/// the complete file content at once, such as for:
/// - Loading configuration files
/// - Reading templates
/// - Processing small text files entirely in memory
///
/// After this call, the EOF flag is always set (the entire file has been consumed).
///
/// **Memory considerations:**
/// The entire remaining content is loaded into memory. For very large files,
/// this may consume significant memory. Consider using line-by-line reading
/// for large files or streaming processing.
///
/// **Example:**
/// ```
/// Dim reader = LineReader.Open("config.json")
/// Dim content = reader.ReadAll()
/// reader.Close()
/// Dim config = JSON.Parse(content)
/// ```
///
/// @param obj Pointer to a LineReader object. Must not be NULL and reader must be open.
///
/// @return A Zanna string containing all remaining file content. Returns an
///         empty string if already at EOF or on error.
///         Traps if obj is NULL, reader is closed, or allocation fails.
///
/// @note Any previously peeked character is included at the start of the result.
/// @note The EOF flag is always set after this call.
/// @note Line endings are preserved as-is (no normalization).
/// @note Thread safety: Not thread-safe for the same LineReader object.
///
/// @see rt_linereader_read For line-by-line reading
/// @see rt_linereader_eof For checking end-of-file status
rt_string rt_linereader_read_all(void *obj) {
    if (!obj) {
        rt_trap("LineReader.ReadAll: null reader");
        return rt_string_from_bytes("", 0);
    }

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return rt_string_from_bytes("", 0);
    if (!lr->fp || lr->closed) {
        rt_trap("LineReader.ReadAll: reader is closed");
        return rt_string_from_bytes("", 0);
    }

    // Get current position and file size (64-bit safe)
    int64_t pos = lr_ftell(lr->fp);
    if (pos < 0) {
        rt_trap("LineReader.ReadAll: tell failed");
        return rt_string_from_bytes("", 0);
    }

    if (lr_fseek(lr->fp, 0, SEEK_END) != 0) {
        rt_trap("LineReader.ReadAll: seek failed");
        return rt_string_from_bytes("", 0);
    }
    int64_t end = lr_ftell(lr->fp);
    if (end < 0) {
        rt_trap("LineReader.ReadAll: tell failed");
        return rt_string_from_bytes("", 0);
    }
    if (lr_fseek(lr->fp, pos, SEEK_SET) != 0) {
        rt_trap("LineReader.ReadAll: seek failed");
        return rt_string_from_bytes("", 0);
    }

    int64_t remaining64 = end > pos ? end - pos : 0;
    if ((uint64_t)remaining64 > (uint64_t)SIZE_MAX) {
        rt_trap("LineReader.ReadAll: file too large");
        return rt_string_from_bytes("", 0);
    }
    size_t remaining = (size_t)remaining64;

    // Account for any peeked character
    size_t extra = lr->has_peeked ? 1 : 0;
    if (remaining > SIZE_MAX - extra) {
        rt_trap("LineReader.ReadAll: file too large");
        return rt_string_from_bytes("", 0);
    }
    size_t total = remaining + extra;

    if (total == 0) {
        lr->eof = 1;
        return rt_string_from_bytes("", 0);
    }

    char *buf = (char *)malloc(total);
    if (!buf) {
        rt_trap("LineReader.ReadAll: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t offset = 0;

    // Add peeked character first
    if (lr->has_peeked) {
        buf[offset++] = (char)lr->peeked;
        lr->has_peeked = 0;
    }

    // Read the rest. The size was measured above, so a short clean read means
    // the file changed while we were reading it.
    size_t read_total = 0;
    while (read_total < remaining) {
        size_t read = fread(buf + offset + read_total, 1, remaining - read_total, lr->fp);
        if (read == 0) {
            free(buf);
            if (ferror(lr->fp))
                rt_trap("LineReader.ReadAll: read failed");
            else
                rt_trap("LineReader.ReadAll: file changed while reading");
            return rt_string_from_bytes("", 0);
        }
        read_total += read;
    }
    total = offset + read_total;

    lr->eof = 1;

    return linereader_string_from_owned_buffer(
        buf, total, "LineReader.ReadAll: string allocation failed");
}

/// @brief Checks whether the end of file has been reached.
///
/// Returns true if a previous read operation encountered the end of the file.
/// The EOF flag is set when:
/// - rt_linereader_read reaches EOF (either with or without content)
/// - rt_linereader_read_char returns -1
/// - rt_linereader_peek_char returns -1
/// - rt_linereader_read_all completes
///
/// **Typical usage pattern:**
/// ```
/// While Not reader.EOF()
///     Dim line = reader.Read()
///     ProcessLine(line)
/// Wend
/// ```
///
/// @param obj Pointer to a LineReader object. May be NULL.
///
/// @return 1 (true) if EOF has been reached, or if obj is NULL, or if reader
///         is closed. Returns 0 (false) if the reader is open and EOF has not
///         been encountered.
///
/// @note The EOF flag is "sticky" - once set, it remains set.
/// @note Thread safety: Not thread-safe for the same LineReader object.
///
/// @see rt_linereader_read For operations that set the EOF flag
int8_t rt_linereader_eof(void *obj) {
    if (!obj)
        return 1;

    rt_linereader_impl *lr = linereader_require(obj, "LineReader: invalid reader");
    if (!lr)
        return 1;
    if (!lr->fp || lr->closed)
        return 1;

    return lr->eof;
}
