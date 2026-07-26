//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_linewriter.c
// Purpose: Implements buffered text file writing for the Zanna.IO.LineWriter
//          class. Supports creating or overwriting files, appending to existing
//          files, writing text with or without newlines, and writing single
//          characters. The newline string is configurable and defaults to the
//          platform-native line ending.
//
// Key invariants:
//   - Open mode creates or truncates; Append mode opens for append-only writes.
//   - The newline string defaults to CRLF on Windows and LF elsewhere.
//   - WriteLn appends the configured newline string after each piece of text.
//   - The closed flag prevents double-close; writing to a closed writer traps.
//   - The GC finalizer flushes and closes the FILE* if the caller forgets Close.
//   - The newline rt_string is retained by the writer and released on finalize.
//
// Ownership/Lifetime:
//   - LineWriter objects are heap-allocated; the GC calls the finalizer on free.
//   - The writer retains a reference to its newline string for its full lifetime.
//
// Links: src/runtime/io/rt_linewriter.h (public API),
//        src/runtime/io/rt_linereader.h (complementary text file reader)
//
//===----------------------------------------------------------------------===//

#include "rt_linewriter.h"

#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_string.h"

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Platform-specific default newline.
#ifdef _WIN32
#define RT_DEFAULT_NEWLINE "\r\n"
#else
#define RT_DEFAULT_NEWLINE "\n"
#endif

/// @brief LineWriter implementation structure.
typedef struct rt_linewriter_impl {
    FILE *fp;          ///< File pointer.
    int8_t closed;     ///< Closed flag.
    rt_string newline; ///< Newline string.
} rt_linewriter_impl;

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

/// @brief Validate and unwrap an opaque LineWriter receiver.
/// @details Checks the runtime class identifier and complete inline payload size before casting;
///          invalid receivers trap.
/// @param obj Borrowed opaque runtime receiver.
/// @param context Trap diagnostic for an invalid receiver; may be NULL.
/// @return Pointer to the validated implementation payload, or NULL as trap-control fallback.
static rt_linewriter_impl *linewriter_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_LINEWRITER_CLASS_ID, sizeof(rt_linewriter_impl))) {
        rt_trap(context ? context : "LineWriter: invalid writer");
        return NULL;
    }
    return (rt_linewriter_impl *)obj;
}

/// @brief Accept a null or valid runtime string and reject other object kinds.
/// @details Null is permitted for optional-string callers such as newline reset; operations that
///          require text reject null before invoking this helper.
/// @param text Borrowed runtime string candidate.
/// @param context Trap diagnostic used for a non-string object.
/// @return 1 for null or a valid runtime string handle; otherwise traps and returns 0.
static int linewriter_require_string(rt_string text, const char *context) {
    if (!text || rt_string_is_handle(text))
        return 1;
    rt_trap(context);
    return 0;
}

/// @brief Allocate a LineWriter while retaining all constructor resources.
/// @details The native file and default newline exist before the managed
///          wrapper can be allocated. A local trap boundary owns both until
///          object allocation succeeds, closing the file and releasing the
///          string before rethrowing any allocation trap. This makes Open and
///          Append leak-free for both aborting and returning trap hooks.
/// @param fp Open native output stream whose ownership transfers on success.
/// @param newline Owned default newline reference installed into the result.
/// @return Fresh LineWriter payload, or NULL after cleanup and trap propagation.
static rt_linewriter_impl *linewriter_alloc_or_cleanup(FILE *fp, rt_string newline) {
    FILE *volatile owned_fp = fp;
    rt_string volatile owned_newline = newline;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "LineWriter: memory allocation failed");
        rt_trap_clear_recovery();
        fclose((FILE *)owned_fp);
        rt_string_unref((rt_string)owned_newline);
        rt_trap(saved_error);
        return NULL;
    }

    rt_linewriter_impl *lw = (rt_linewriter_impl *)rt_obj_new_i64(
        RT_LINEWRITER_CLASS_ID, (int64_t)sizeof(rt_linewriter_impl));
    rt_trap_clear_recovery();
    if (!lw) {
        fclose(fp);
        rt_string_unref(newline);
        rt_trap("LineWriter: memory allocation failed");
        return NULL;
    }
    return lw;
}

/// @brief Validate that a runtime string length can be passed to stdio.
/// @param len Signed runtime byte length.
/// @param context Diagnostic emitted for negative or host-size overflow.
/// @return Non-zero when @p len is representable as `size_t`.
static int linewriter_length_fits_host(int64_t len, const char *context) {
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap(context);
        return 0;
    }
    return 1;
}

/// @brief Finalizer callback invoked when a LineWriter is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// LineWriter object becomes unreachable. It performs two cleanup tasks:
///
/// 1. **Closes the file**: If the file is still open, it is closed with fclose,
///    which flushes any buffered data to disk. This ensures no data is lost
///    even if the program forgets to explicitly close the writer.
///
/// 2. **Releases the newline string**: The configurable newline string is
///    reference-counted; the finalizer releases our reference to prevent memory leaks.
///
/// @param obj Pointer to the LineWriter object being finalized. May be NULL (no-op).
///
/// @note This function is idempotent - calling it on an already-closed writer is safe.
/// @note The finalizer does not raise errors; it silently cleans up resources.
/// @note Data is flushed when the file is closed, but for important writes,
///       explicitly flush or close before relying on finalization.
///
/// @see rt_linewriter_close For explicit file closure
/// @see rt_linewriter_flush For flushing without closing
static void rt_linewriter_finalize(void *obj) {
    if (!obj)
        return;
    rt_linewriter_impl *lw = (rt_linewriter_impl *)obj;
    if (lw->fp && !lw->closed) {
        fclose(lw->fp);
        lw->fp = NULL;
        lw->closed = 1;
    }
    if (lw->newline) {
        rt_string_unref(lw->newline);
        lw->newline = NULL;
    }
}

/// @brief Internal helper: opens a file with the specified mode.
///
/// Common implementation for both rt_linewriter_open and rt_linewriter_append.
/// Creates a LineWriter object connected to the file, initialized with:
/// - The file handle from fopen
/// - closed = false
/// - newline = platform default ("\n" on Unix, "\r\n" on Windows)
/// - A finalizer for automatic cleanup
///
/// @param path Zanna string containing the file path. Must not be NULL.
/// @param mode The fopen mode string ("w" for write/truncate, "a" for append).
///
/// @return A pointer to a new LineWriter object on success, or NULL on failure
///         after trapping with an error message.
///
/// @note This is an internal function, not part of the public API.
static void *rt_linewriter_open_mode(rt_string path, const char *mode) {
    if (!path) {
        rt_trap("LineWriter: null path");
        return NULL;
    }

    const char *path_str = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &path_str) || !path_str) {
        rt_trap("LineWriter: invalid path");
        return NULL;
    }

    rt_string newline = rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
    if (!newline) {
        rt_trap("LineWriter: memory allocation failed");
        return NULL;
    }

    FILE *fp = rt_file_stdio_open_utf8(path_str, mode);
    if (!fp) {
        // IO-H-7: include filename and OS error for actionable diagnostics
        char msg[512];
        snprintf(
            msg, sizeof(msg), "LineWriter: failed to open '%s': %s", path_str, strerror(errno));
        rt_string_unref(newline);
        rt_trap(msg);
        return NULL;
    }

    rt_linewriter_impl *lw = linewriter_alloc_or_cleanup(fp, newline);
    if (!lw)
        return NULL;

    lw->fp = fp;
    lw->closed = 0;
    lw->newline = newline;
    rt_obj_set_finalizer(lw, rt_linewriter_finalize);

    return lw;
}

/// @brief Opens a file for writing, creating or truncating it.
///
/// Creates a new LineWriter connected to the specified file. If the file
/// already exists, its contents are truncated (erased). If the file doesn't
/// exist, it is created.
///
/// The LineWriter is managed by Zanna's garbage collector and will automatically
/// close when collected if not explicitly closed.
///
/// **Default newline:**
/// The writer is initialized with a platform-appropriate newline:
/// - Unix/Linux/macOS: `\n` (LF)
/// - Windows: `\r\n` (CRLF)
///
/// Use rt_linewriter_set_newline to change the newline string if needed.
///
/// **Example usage:**
/// ```
/// Dim writer = LineWriter.Open("output.txt")
/// writer.WriteLn("First line")
/// writer.WriteLn("Second line")
/// writer.Close()
/// ```
///
/// @param path Zanna string containing the file path. Must not be NULL.
///
/// @return A pointer to a new LineWriter object on success. On failure, traps
///         with an error message and returns NULL. Failure reasons include:
///         - NULL or invalid path string
///         - File cannot be opened (directory doesn't exist, no permission, etc.)
///         - Memory allocation failure
///
/// @note This opens in write mode ("w") - existing file contents are lost!
/// @note Thread safety: Not thread-safe. Each thread should have its own LineWriter.
///
/// @see rt_linewriter_append For opening without truncating
/// @see rt_linewriter_close For closing the writer
/// @see rt_linewriter_write_ln For writing lines
void *rt_linewriter_open(rt_string path) {
    return rt_linewriter_open_mode(path, "wb");
}

/// @brief Opens a file for appending, creating it if it doesn't exist.
///
/// Creates a new LineWriter connected to the specified file in append mode.
/// All writes go to the end of the file, preserving any existing content.
/// If the file doesn't exist, it is created.
///
/// This is useful for:
/// - Adding entries to log files
/// - Incrementally building output files
/// - Writing to shared files without overwriting
///
/// **Example usage:**
/// ```
/// Dim log = LineWriter.Append("app.log")
/// log.WriteLn(Now() + ": Application started")
/// log.Close()
/// ```
///
/// @param path Zanna string containing the file path. Must not be NULL.
///
/// @return A pointer to a new LineWriter object on success. On failure, traps
///         with an error message and returns NULL. Failure reasons include:
///         - NULL or invalid path string
///         - File cannot be opened (directory doesn't exist, no permission, etc.)
///         - Memory allocation failure
///
/// @note All writes go to end-of-file, even if you try to seek.
/// @note Thread safety: Not thread-safe. Multiple writers to the same file
///       may interleave output in unpredictable ways.
///
/// @see rt_linewriter_open For creating/truncating files
/// @see rt_linewriter_close For closing the writer
void *rt_linewriter_append(rt_string path) {
    return rt_linewriter_open_mode(path, "ab");
}

/// @brief Explicitly closes a LineWriter, flushing and releasing the file.
///
/// Closes the file associated with this LineWriter, flushing any buffered
/// data to disk. After calling close, any subsequent write operations on
/// this LineWriter will trap with an error.
///
/// Benefits of explicit closing:
/// - Ensures all data is written to disk at a known point
/// - Immediately releases the OS file handle
/// - Makes the file available for other processes to open
/// - More predictable than relying on garbage collection
///
/// @param obj Pointer to a LineWriter object. If NULL, this function is a no-op.
///
/// @note This function is idempotent - calling close on an already-closed
///       LineWriter does nothing (no error).
/// @note After closing, the LineWriter object still exists in memory but is
///       unusable. It will be freed when the garbage collector runs.
/// @note The newline string is NOT released by close - only by finalization.
///
/// @see rt_linewriter_open For opening files
/// @see rt_linewriter_flush For flushing without closing
void rt_linewriter_close(void *obj) {
    if (!obj)
        return;

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (lw->fp && !lw->closed) {
        int rc = fclose(lw->fp);
        lw->fp = NULL;
        lw->closed = 1;
        if (rc != 0)
            rt_trap("LineWriter.Close: close failed (disk full or I/O error)");
    }
}

/// @brief Writes a string to the file without a trailing newline.
///
/// Writes the exact content of the string to the file at the current position.
/// No newline is appended - use this for partial line output or when you want
/// precise control over the output format.
///
/// **Example usage:**
/// ```
/// writer.Write("Name: ")
/// writer.Write(name)
/// writer.Write(", Age: ")
/// writer.Write(Str(age))
/// writer.WriteLn("")  ' End the line
/// ```
///
/// @param obj Pointer to a LineWriter object. Must not be NULL and writer must be open.
/// @param text The Zanna string to write. A NULL or non-string value traps.
///
/// @note Empty strings write nothing (no error).
/// @note Data may be buffered by the OS. Use rt_linewriter_flush for immediate writes.
/// @note Traps if obj is NULL or writer is closed.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_write_ln For writing with a newline
/// @see rt_linewriter_write_char For writing single characters
/// @see rt_linewriter_flush For flushing buffered data
void rt_linewriter_write(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("LineWriter.Write: null writer");
        return;
    }

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (!lw->fp || lw->closed) {
        rt_trap("LineWriter.Write: writer is closed");
        return;
    }

    if (!text) {
        rt_trap("LineWriter.Write: null string");
        return;
    }
    if (!linewriter_require_string(text, "LineWriter.Write: invalid string"))
        return;

    const char *data = rt_string_cstr(text);
    int64_t len = rt_str_len(text);
    if (!data) {
        rt_trap("LineWriter.Write: invalid string");
        return;
    }
    if (!linewriter_length_fits_host(len, "LineWriter.Write: invalid string length"))
        return;
    if (len > 0) {
        // IO-C-4: check fwrite return to detect disk-full / I/O errors
        size_t written = fwrite(data, 1, (size_t)len, lw->fp);
        if (written != (size_t)len)
            rt_trap("LineWriter.Write: short write (disk full or I/O error)");
    }
}

/// @brief Writes a string followed by a newline to the file.
///
/// This is the primary method for line-oriented writing. It writes the text
/// content followed by the configured newline string (default is platform-
/// appropriate: "\n" on Unix, "\r\n" on Windows).
///
/// **Usage patterns:**
/// ```
/// ' Write multiple lines
/// writer.WriteLn("Line 1")
/// writer.WriteLn("Line 2")
///
/// ' Write an empty line
/// writer.WriteLn("")
///
/// ' Empty strings write only the newline
/// writer.WriteLn("")
/// ```
///
/// **Newline customization:**
/// The newline written is configurable via rt_linewriter_set_newline. You can:
/// - Use Windows newlines on Unix: `writer.set_NewLine(Chr(13) + Chr(10))`
/// - Use Unix newlines on Windows: `writer.set_NewLine(Chr(10))`
/// - Use custom separators: `writer.set_NewLine("--RECORD--\n")`
///
/// @param obj Pointer to a LineWriter object. Must not be NULL and writer must be open.
/// @param text The Zanna string to write before the newline. Must not be NULL.
///
/// @note Data may be buffered by the OS. Use rt_linewriter_flush for immediate writes.
/// @note Traps if obj is NULL or writer is closed.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_write For writing without a newline
/// @see rt_linewriter_set_newline For changing the newline string
void rt_linewriter_write_ln(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("LineWriter.WriteLn: null writer");
        return;
    }

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (!lw->fp || lw->closed) {
        rt_trap("LineWriter.WriteLn: writer is closed");
        return;
    }

    if (!text) {
        rt_trap("LineWriter.WriteLn: null string");
        return;
    }
    if (!linewriter_require_string(text, "LineWriter.WriteLn: invalid string"))
        return;

    const char *data = rt_string_cstr(text);
    int64_t len = rt_str_len(text);
    if (!data) {
        rt_trap("LineWriter.WriteLn: invalid string");
        return;
    }
    if (!linewriter_length_fits_host(len, "LineWriter.WriteLn: invalid string length"))
        return;
    if (len > 0) {
        // IO-C-4: check fwrite return to detect disk-full / I/O errors
        size_t written = fwrite(data, 1, (size_t)len, lw->fp);
        if (written != (size_t)len)
            rt_trap("LineWriter.WriteLn: short write (disk full or I/O error)");
    }

    // Write newline
    if (lw->newline) {
        const char *nl = rt_string_cstr(lw->newline);
        int64_t nl_len = rt_str_len(lw->newline);
        if (!nl) {
            rt_trap("LineWriter.WriteLn: invalid newline");
            return;
        }
        if (!linewriter_length_fits_host(nl_len, "LineWriter.WriteLn: invalid newline length"))
            return;
        if (nl_len > 0) {
            // IO-C-4: check fwrite return for newline write too
            size_t written = fwrite(nl, 1, (size_t)nl_len, lw->fp);
            if (written != (size_t)nl_len)
                rt_trap("LineWriter.WriteLn: short write on newline (disk full or I/O error)");
        }
    }
}

/// @brief Writes a single character to the file.
///
/// Writes one byte to the current file position. This provides low-level
/// character output for:
/// - Building output character by character
/// - Writing special characters (CR, LF, tab, etc.)
/// - Precise binary-like control over text output
///
/// **Example:**
/// ```
/// ' Write a comma-separated line with specific control
/// writer.Write(name)
/// writer.WriteChar(44)   ' comma
/// writer.WriteChar(32)   ' space
/// writer.Write(value)
/// writer.WriteChar(10)   ' LF
/// ```
///
/// @param obj Pointer to a LineWriter object. Must not be NULL and writer must be open.
/// @param ch The character value to write (0-255). Values outside this range trap.
///
/// @note Accepted values are written as one unsigned byte.
/// @note Traps if obj is NULL or writer is closed.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_write For writing strings
/// @see rt_linewriter_write_ln For writing lines
void rt_linewriter_write_char(void *obj, int64_t ch) {
    if (!obj) {
        rt_trap("LineWriter.WriteChar: null writer");
        return;
    }

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (!lw->fp || lw->closed) {
        rt_trap("LineWriter.WriteChar: writer is closed");
        return;
    }

    if (ch < 0 || ch > 255) {
        rt_trap("LineWriter.WriteChar: character out of range");
        return;
    }

    if (fputc((int)ch, lw->fp) == EOF) {
        rt_trap("LineWriter.WriteChar: write failed (disk full or I/O error)");
        return;
    }
}

/// @brief Flushes buffered data to disk without closing the file.
///
/// Forces data in the C stdio buffer to the host file descriptor. This makes
/// output visible to the operating system and peer readers but does not issue
/// a platform durability primitive such as `fsync`.
///
/// **When to use flush:**
/// - After writing critical data that must not be lost
/// - Before a potentially long-running operation
/// - To make output visible to other processes reading the file
/// - In log files, after each important entry
///
/// **Example:**
/// ```
/// writer.WriteLn("Starting operation...")
/// writer.Flush()   ' Ensure this is on disk before we start
/// DoLongOperation()
/// writer.WriteLn("Operation complete")
/// writer.Flush()
/// ```
///
/// @param obj Pointer to a LineWriter object. If NULL or closed, this is a no-op.
///
/// @note This function does not trap on NULL or closed - it silently returns.
/// @note Even after flush, the OS may still buffer data. For maximum durability,
///       consider OS-specific sync mechanisms.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_close For closing and flushing the file
void rt_linewriter_flush(void *obj) {
    if (!obj)
        return;

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (lw->fp && !lw->closed) {
        if (fflush(lw->fp) != 0) {
            rt_trap("LineWriter.Flush: flush failed (disk full or I/O error)");
            return;
        }
    }
}

/// @brief Gets the current newline string used by WriteLn.
///
/// Returns the newline string that is appended by rt_linewriter_write_ln.
/// By default, this is the platform-appropriate newline:
/// - Unix/Linux/macOS: `\n` (LF, character 10)
/// - Windows: `\r\n` (CRLF, characters 13 and 10)
///
/// @param obj Pointer to a LineWriter object. If NULL, returns the platform default.
///
/// @return A Zanna string containing the current newline sequence. The caller
///         receives a new reference that must be managed appropriately.
///
/// @note This returns a new reference to the newline string, not the internal copy.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_set_newline For changing the newline string
/// @see rt_linewriter_write_ln For writing with the newline
rt_string rt_linewriter_newline(void *obj) {
    if (!obj) {
        return rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
    }

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
    if (lw->closed) {
        rt_trap("LineWriter.get_NewLine: writer is closed");
        return rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
    }
    if (lw->newline) {
        return rt_string_ref(lw->newline);
    }
    return rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
}

/// @brief Sets the newline string used by WriteLn.
///
/// Changes the newline sequence appended by rt_linewriter_write_ln. This allows:
/// - Cross-platform newline control (always Unix-style, always Windows-style)
/// - Custom record separators for structured output
/// - Empty string for no newline in WriteLn (making it behave like Write)
///
/// **Common uses:**
/// ```
/// ' Force Unix newlines on Windows
/// writer.set_NewLine(Chr(10))
///
/// ' Force Windows newlines on Unix
/// writer.set_NewLine(Chr(13) + Chr(10))
///
/// ' Custom record separator
/// writer.set_NewLine("\n---\n")
///
/// ' Reset to platform default
/// writer.set_NewLine(Nothing)
/// ```
///
/// @param obj Pointer to a LineWriter object. Must not be NULL.
/// @param nl The new newline string. If NULL, resets to the platform default.
///           Can be empty string "" to suppress newlines entirely.
///
/// @note This takes a reference to the provided string; the original can be
///       released after this call.
/// @note The old newline string is properly released.
/// @note Traps if obj is NULL.
/// @note Thread safety: Not thread-safe for the same LineWriter object.
///
/// @see rt_linewriter_newline For getting the current newline string
/// @see rt_linewriter_write_ln For writing with the newline
void rt_linewriter_set_newline(void *obj, rt_string nl) {
    if (!obj) {
        rt_trap("LineWriter.set_NewLine: null writer");
        return;
    }

    rt_linewriter_impl *lw = linewriter_require(obj, "LineWriter: invalid writer");
    if (!lw)
        return;
    if (lw->closed) {
        rt_trap("LineWriter.set_NewLine: writer is closed");
        return;
    }

    rt_string next = NULL;
    if (nl) {
        if (!linewriter_require_string(nl, "LineWriter.set_NewLine: invalid newline"))
            return;
        next = rt_string_ref(nl);
    } else {
        next = rt_string_from_bytes(RT_DEFAULT_NEWLINE, strlen(RT_DEFAULT_NEWLINE));
        if (!next) {
            rt_trap("LineWriter.set_NewLine: memory allocation failed");
            return;
        }
    }

    // Release old newline
    if (lw->newline) {
        rt_string_unref(lw->newline);
    }

    lw->newline = next;
}
