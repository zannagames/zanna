//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_binfile.c
// Purpose: Implements binary file stream operations for the Zanna.IO.BinFile
//          class. Supports random-access bulk and single-byte I/O, position and
//          size queries, flushing, and explicit or finalizer-driven closure.
//
// Key invariants:
//   - Open modes: "r" (read-only), "w" (write/truncate), "rw" (read-write),
//     "a" (append). Invalid modes cause a trap.
//   - 64-bit seek/tell are used (fseeko/ftello on POSIX, _fseeki64 on Win32)
//     to support files larger than 2GB.
//   - EOF is set when stdio reports end-of-file and is cleared by a successful
//     seek, a successful later read, or a transition to writing.
//   - The closed flag prevents double-close; operations on a closed file trap.
//
// Ownership/Lifetime:
//   - BinFile objects are heap-allocated; the GC calls the finalizer on collect.
//   - The finalizer flushes and closes the FILE* if not already closed.
//   - Returned rt_bytes from ReadBytes are fresh allocations owned by the caller.
//
// Links: src/runtime/io/rt_binfile.h (public API),
//        src/runtime/io/rt_stream.h (wraps BinFile behind a generic stream),
//        src/runtime/io/rt_memstream.h (in-memory counterpart)
//
//===----------------------------------------------------------------------===//

#include "rt_binfile.h"

#include "rt_bytes.h"
#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_string.h"

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// IO-C-2/C-3: Use 64-bit seek/tell to support files larger than 2GB.
#if defined(_WIN32)
#define binfile_fseek(fp, off, whence) _fseeki64((fp), (off), (whence))
#define binfile_ftell(fp) _ftelli64((fp))
#else
#define binfile_fseek(fp, off, whence) fseeko((fp), (off_t)(off), (whence))
#define binfile_ftell(fp) ftello((fp))
#endif

/// @brief BinFile implementation structure.
typedef struct rt_binfile_impl {
    FILE *fp;       ///< File pointer.
    int8_t eof;     ///< EOF flag.
    int8_t closed;  ///< Closed flag.
    int8_t last_op; ///< Last stdio direction used on update streams.
} rt_binfile_impl;

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

/// @brief Determine whether a pointer is a valid runtime BinFile object.
/// @param obj Candidate pointer; `NULL` and unrelated runtime objects are
/// accepted.
/// @return 1 when class ID and payload size match BinFile; otherwise 0.
int8_t rt_binfile_is_handle(void *obj) {
    return rt_obj_is_instance(obj, RT_BINFILE_CLASS_ID, sizeof(rt_binfile_impl)) ? 1 : 0;
}

/// @brief Validate and unwrap an opaque BinFile handle.
/// @param obj Candidate runtime object.
/// @param context Diagnostic raised for an invalid handle; a generic BinFile
/// message is used when this pointer is `NULL`.
/// @return Internal payload on success, or `NULL` after raising a trap.
static rt_binfile_impl *binfile_require(void *obj, const char *context) {
    if (!rt_binfile_is_handle(obj)) {
        rt_trap(context ? context : "BinFile: invalid file");
        return NULL;
    }
    return (rt_binfile_impl *)obj;
}

/// @brief Allocate a BinFile payload while retaining cleanup ownership of an open `FILE *`.
/// @details Runtime object allocation traps through longjmp on OOM. Installing
///          a local recovery boundary ensures the already-open native stream is
///          closed before that trap is propagated. A trap hook that returns is
///          also handled: a NULL allocation closes @p fp and reports the
///          caller-facing fallback without publishing a half-constructed file.
/// @param fp Open native stream whose ownership transfers on success.
/// @param fallback Diagnostic used when no more specific recovered message exists.
/// @return Fresh uninitialized BinFile payload, or NULL after cleanup/trap.
static rt_binfile_impl *binfile_alloc_or_close(FILE *fp, const char *fallback) {
    FILE *volatile owned_fp = fp;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error, sizeof(saved_error), "%s", error && error[0] ? error : fallback);
        rt_trap_clear_recovery();
        fclose((FILE *)owned_fp);
        rt_trap(saved_error);
        return NULL;
    }

    rt_binfile_impl *bf =
        (rt_binfile_impl *)rt_obj_new_i64(RT_BINFILE_CLASS_ID, (int64_t)sizeof(rt_binfile_impl));
    rt_trap_clear_recovery();
    if (!bf) {
        fclose(fp);
        rt_trap(fallback);
        return NULL;
    }
    return bf;
}

#define BINFILE_OP_NONE 0
#define BINFILE_OP_READ 1
#define BINFILE_OP_WRITE 2

/// @brief Transition the FILE* from write mode to read mode before a read call.
///
/// C stdio requires a "sync point" (fflush, fseek, or fsetpos) between
/// write and read operations on the same stream when opened in
/// update ("r+" / "w+") mode — without one, the read can return
/// buffered data in undefined ways. Tracking the last op locally
/// lets us fflush only when actually switching direction, so
/// sequential reads don't pay the flush cost. Traps on flush
/// failure so the caller never silently reads stale data.
///
/// @param bf Valid, open BinFile payload.
/// @return 1 when the stream is ready for reading, or 0 for invalid state or
/// after raising a synchronization trap.
static int binfile_prepare_read(rt_binfile_impl *bf) {
    if (!bf || !bf->fp)
        return 0;
    if (bf->last_op == BINFILE_OP_WRITE) {
        if (fflush(bf->fp) != 0) {
            rt_trap("BinFile: failed to switch from write to read");
            return 0;
        }
    }
    bf->last_op = BINFILE_OP_READ;
    return 1;
}

/// @brief Transition the FILE* from read mode to write mode before a write call.
///
/// Symmetric counterpart to `binfile_prepare_read`. A zero-distance
/// `fseek(SEEK_CUR)` is the canonical stdio idiom for "discard read
/// buffer, keep position" — cheaper than closing/reopening. Also
/// clears any EOF flag that could otherwise poison the next write's
/// error check.
///
/// @param bf Valid, open BinFile payload.
/// @return 1 when the stream is ready for writing, or 0 for invalid state or
/// after raising a synchronization trap.
static int binfile_prepare_write(rt_binfile_impl *bf) {
    if (!bf || !bf->fp)
        return 0;
    if (bf->last_op == BINFILE_OP_READ) {
        if (binfile_fseek(bf->fp, 0, SEEK_CUR) != 0) {
            rt_trap("BinFile: failed to switch from read to write");
            return 0;
        }
        clearerr(bf->fp);
    }
    bf->last_op = BINFILE_OP_WRITE;
    bf->eof = 0;
    return 1;
}

/// @brief Prepare the FILE* for a seek by flushing any pending writes.
///
/// Unlike the read/write transitions, this doesn't trap on flush
/// failure — returns 0 instead so the caller can convert to a
/// boolean success for user code. After a successful prep the
/// direction tracker resets to NONE since seeking leaves the
/// stream in a neutral state.
///
/// @param bf Valid, open BinFile payload.
/// @return 1 when pending output is flushed and stdio status is cleared;
/// otherwise 0.
static int binfile_prepare_seek(rt_binfile_impl *bf) {
    if (!bf || !bf->fp)
        return 0;
    if (bf->last_op == BINFILE_OP_WRITE && fflush(bf->fp) != 0)
        return 0;
    clearerr(bf->fp);
    bf->last_op = BINFILE_OP_NONE;
    return 1;
}

/// @brief Finalizer callback invoked when a BinFile is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// BinFile object becomes unreachable. It ensures that the underlying operating
/// system file handle is properly closed to prevent resource leaks.
///
/// The finalizer is a safety net - well-written programs should call
/// rt_binfile_close explicitly when done with a file. However, if the program
/// forgets to close the file or an exception occurs, this finalizer ensures
/// the file is eventually closed when the object is collected.
///
/// @param obj Pointer to the BinFile object being finalized. May be NULL (no-op).
///
/// @note This function is idempotent - calling it on an already-closed file is safe.
/// @note The finalizer does not raise errors; it silently closes the file if open.
///
/// @see rt_binfile_close For explicit file closure
/// @see rt_obj_set_finalizer For how finalizers are registered
static void rt_binfile_finalize(void *obj) {
    if (!obj)
        return;
    rt_binfile_impl *bf = (rt_binfile_impl *)obj;
    if (bf->fp && !bf->closed) {
        fclose(bf->fp);
        bf->fp = NULL;
        bf->closed = 1;
    }
}

/// @brief Opens a binary file for reading, writing, or both.
///
/// Creates a new BinFile object connected to the specified file path. The file
/// is opened in binary mode (no newline translation) using the specified access
/// mode. The returned BinFile is managed by Zanna's garbage collector and will
/// automatically close when collected if not explicitly closed.
///
/// **Supported modes:**
/// | Mode  | Description                              | C equivalent |
/// |-------|------------------------------------------|--------------|
/// | "r"   | Read-only, file must exist               | "rb"         |
/// | "w"   | Write-only, creates/truncates file       | "wb"         |
/// | "rw"  | Read/write, file must exist              | "r+b"        |
/// | "a"   | Append-only, creates if doesn't exist    | "ab"         |
///
/// **Usage example (conceptual):**
/// ```
/// Dim bf = BinFile.Open("data.bin", "rw")
/// bf.WriteByte(0x42)
/// bf.Seek(0, 0)       ' Seek to beginning
/// Dim b = bf.ReadByte()
/// bf.Close()
/// ```
///
/// @param path Zanna string containing the file path. Must not be NULL.
///             Path is interpreted according to the OS (relative or absolute).
/// @param mode Zanna string containing the access mode ("r", "w", "rw", or "a").
///             Must not be NULL.
///
/// @return A pointer to a new BinFile object on success. On failure, traps with
///         an error message and returns NULL. Failure reasons include:
///         - NULL path or mode string
///         - Invalid mode string (not one of r/w/rw/a)
///         - File cannot be opened (permissions, doesn't exist for "r"/"rw", etc.)
///         - Memory allocation failure
///
/// @note The BinFile should be closed with rt_binfile_close when done. If not
///       explicitly closed, it will be closed when garbage collected.
/// @note All reads/writes are binary (no character encoding or newline translation).
/// @note Thread safety: Not thread-safe. Each thread should have its own BinFile.
///
/// @see rt_binfile_close For closing the file
/// @see rt_binfile_read For reading bytes into a buffer
/// @see rt_binfile_write For writing bytes from a buffer
void *rt_binfile_open(void *path, void *mode) {
    if (!path || !mode) {
        rt_trap("BinFile.Open: null path or mode");
        return NULL;
    }

    if (!rt_string_is_handle(mode)) {
        rt_trap("BinFile.Open: invalid mode");
        return NULL;
    }

    const char *path_str = NULL;
    const char *mode_str = rt_string_cstr((rt_string)mode);
    int64_t mode_len = rt_str_len((rt_string)mode);

    if (!rt_file_path_from_vstr((const ZannaString *)path, &path_str) || !path_str || !mode_str ||
        mode_len < 0 || (size_t)mode_len != strlen(mode_str)) {
        rt_trap("BinFile.Open: invalid path or mode");
        return NULL;
    }

    // Map mode string to fopen mode
    const char *fmode = NULL;
    if (strcmp(mode_str, "r") == 0 || strcmp(mode_str, "rb") == 0)
        fmode = "rb";
    else if (strcmp(mode_str, "w") == 0 || strcmp(mode_str, "wb") == 0)
        fmode = "wb";
    else if (strcmp(mode_str, "rw") == 0 || strcmp(mode_str, "r+") == 0 ||
             strcmp(mode_str, "rb+") == 0 || strcmp(mode_str, "r+b") == 0)
        fmode = "r+b";
    else if (strcmp(mode_str, "a") == 0 || strcmp(mode_str, "ab") == 0)
        fmode = "ab";
    else {
        rt_trap("BinFile.Open: invalid mode (use r/rb, w/wb, rw/r+, or a/ab)");
        return NULL;
    }

    FILE *fp = rt_file_stdio_open_utf8(path_str, fmode);
    if (!fp) {
        char buf[512];
        snprintf(
            buf, sizeof(buf), "BinFile.Open: failed to open '%s': %s", path_str, strerror(errno));
        rt_trap(buf);
        return NULL;
    }

    rt_binfile_impl *bf = binfile_alloc_or_close(fp, "BinFile.Open: memory allocation failed");
    if (!bf)
        return NULL;

    bf->fp = fp;
    bf->eof = 0;
    bf->closed = 0;
    bf->last_op = BINFILE_OP_NONE;
    rt_obj_set_finalizer(bf, rt_binfile_finalize);

    return bf;
}

/// @brief Explicitly closes a BinFile, releasing the underlying file handle.
///
/// Closes the file associated with this BinFile object, flushing any buffered
/// data to disk. After calling close, any subsequent read/write/seek operations
/// on this BinFile will trap with an error.
///
/// It is good practice to explicitly close files when done, rather than relying
/// on the garbage collector. Benefits of explicit closing:
/// - Immediate release of OS file handle resources
/// - Ensures data is flushed to disk at a known point
/// - Avoids potential resource exhaustion with many open files
/// - Makes program behavior more predictable
///
/// @param obj Pointer to a BinFile object. If NULL, this function is a no-op.
///
/// @note This function is idempotent - calling close on an already-closed
///       BinFile does nothing (no error).
/// @note After closing, the BinFile object still exists in memory but is
///       unusable. It will be freed when the garbage collector runs.
/// @note Thread safety: Not thread-safe. Don't close a file while another
///       thread is using it.
///
/// @see rt_binfile_open For opening files
/// @see rt_binfile_flush For flushing without closing
void rt_binfile_close(void *obj) {
    if (!obj)
        return;
    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Close: invalid file");
    if (!bf)
        return;
    if (bf->fp && !bf->closed) {
        int rc = fclose(bf->fp);
        bf->fp = NULL;
        bf->closed = 1;
        if (rc != 0)
            rt_trap("BinFile.Close: close failed (disk full or I/O error)");
    }
}

/// @brief Reads bytes from the file into a Bytes buffer.
///
/// Reads up to `count` bytes from the current file position into the provided
/// Bytes buffer starting at `offset`. The file position advances by the number
/// of bytes actually read. If end-of-file is reached during the read, the EOF
/// flag is set and fewer bytes than requested may be returned.
///
/// **Bounds checking:**
/// - If `offset` is negative, it is treated as 0
/// - If `offset` is beyond the Bytes buffer length, returns 0 (no read)
/// - If `offset + count` exceeds buffer length, count is clamped to fit
///
/// **Example usage:**
/// ```
/// Dim buf = Bytes.New(1024)
/// Dim n = bf.Read(buf, 0, 1024)  ' Read up to 1024 bytes at offset 0
/// If n < 1024 And bf.EOF() Then
///     ' Reached end of file
/// End If
/// ```
///
/// @param obj Pointer to a BinFile object. Must not be NULL and file must be open.
/// @param bytes Pointer to a Bytes object to receive the data. Must not be NULL.
/// @param offset Starting offset within the Bytes buffer to write data.
///               Negative values are treated as 0.
/// @param count Maximum number of bytes to read. If <= 0, returns 0.
///
/// @return The number of bytes actually read (0 to count). May be less than
///         count if EOF is reached. Returns 0 if:
///         - count <= 0
///         - offset is beyond buffer length
///         - EOF was already reached
///         Traps and returns 0 if obj or bytes is NULL, or file is closed.
///
/// @note The EOF flag is set if end-of-file is encountered during the read.
///       Use rt_binfile_eof to check this flag.
/// @note Thread safety: Not thread-safe for the same BinFile or Bytes object.
///
/// @see rt_binfile_read_byte For reading a single byte
/// @see rt_binfile_write For writing bytes
/// @see rt_binfile_eof For checking end-of-file status
int64_t rt_binfile_read(void *obj, void *bytes, int64_t offset, int64_t count) {
    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Read: invalid file");
    if (!bf)
        return 0;
    if (!bytes || !rt_bytes_is_bytes(bytes)) {
        rt_trap("BinFile.Read: invalid bytes");
        return 0;
    }

    if (!bf->fp || bf->closed) {
        rt_trap("BinFile.Read: file is closed");
        return 0;
    }

    int64_t bytes_len = rt_bytes_len(bytes);
    uint8_t *bytes_data = rt_bytes_data(bytes);
    if (bytes_len < 0) {
        rt_trap("BinFile.Read: invalid bytes");
        return 0;
    }
    if (offset < 0)
        offset = 0;
    if (count <= 0)
        return 0;
    if (offset >= bytes_len)
        return 0;

    // Clamp count to available space without overflowing offset + count.
    if (count > bytes_len - offset)
        count = bytes_len - offset;
    if (count > 0 && !bytes_data) {
        rt_trap("BinFile.Read: invalid bytes");
        return 0;
    }

    if (!binfile_prepare_read(bf))
        return 0;
    size_t read = fread(bytes_data + offset, 1, (size_t)count, bf->fp);

    if (read < (size_t)count && ferror(bf->fp)) {
        rt_trap("BinFile.Read: read failed");
        return -1;
    }

    if (feof(bf->fp))
        bf->eof = 1;
    else if (read > 0)
        bf->eof = 0;

    return (int64_t)read;
}

/// @brief Writes bytes from a Bytes buffer to the file.
///
/// Writes `count` bytes from the provided Bytes buffer starting at `offset`
/// to the current file position. The file position advances by the number
/// of bytes written. For files opened in append mode ("a"), writes always
/// go to the end of the file regardless of the current position.
///
/// **Bounds checking:**
/// - If `offset` is negative, it is treated as 0
/// - If `offset` is beyond the Bytes buffer length, no data is written
/// - If `offset + count` exceeds buffer length, count is clamped to fit
///
/// **Example usage:**
/// ```
/// Dim buf = Bytes.New(4)
/// buf.Set(0, 0x89)  ' PNG magic
/// buf.Set(1, 0x50)
/// buf.Set(2, 0x4E)
/// buf.Set(3, 0x47)
/// bf.Write(buf, 0, 4)  ' Write all 4 bytes
/// ```
///
/// @param obj Pointer to a BinFile object. Must not be NULL and file must be open.
/// @param bytes Pointer to a Bytes object containing the data. Must not be NULL.
/// @param offset Starting offset within the Bytes buffer to read data from.
///               Negative values are treated as 0.
/// @param count Number of bytes to write. If <= 0, does nothing.
///
/// @note This function traps on failure conditions:
///       - NULL obj or bytes
///       - File is closed
///       - Partial write (disk full, I/O error)
///
/// @note Data may be buffered by the OS. Call rt_binfile_flush to ensure
///       data is written to disk, or rt_binfile_close which flushes automatically.
/// @note Thread safety: Not thread-safe for the same BinFile or Bytes object.
///
/// @see rt_binfile_write_byte For writing a single byte
/// @see rt_binfile_read For reading bytes
/// @see rt_binfile_flush For flushing buffered data
void rt_binfile_write(void *obj, void *bytes, int64_t offset, int64_t count) {
    if (!obj) {
        rt_trap("BinFile.Write: null file");
        return;
    }
    if (!bytes || !rt_bytes_is_bytes(bytes)) {
        rt_trap("BinFile.Write: invalid bytes");
        return;
    }

    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Write: invalid file");
    if (!bf)
        return;
    if (!bf->fp || bf->closed) {
        rt_trap("BinFile.Write: file is closed");
        return;
    }

    int64_t bytes_len = rt_bytes_len(bytes);
    const uint8_t *bytes_data = rt_bytes_data_const(bytes);
    if (bytes_len < 0 || (bytes_len > 0 && !bytes_data)) {
        rt_trap("BinFile.Write: invalid bytes");
        return;
    }
    if (offset < 0)
        offset = 0;
    if (count <= 0)
        return;
    if (offset >= bytes_len)
        return;

    // Clamp count to available data without overflowing offset + count.
    if (count > bytes_len - offset)
        count = bytes_len - offset;

    if (!binfile_prepare_write(bf))
        return;
    size_t written = fwrite(bytes_data + offset, 1, (size_t)count, bf->fp);
    if (written < (size_t)count) {
        rt_trap("BinFile.Write: write failed");
    }
}

/// @brief Reads a single byte from the file.
///
/// Reads one byte from the current file position and advances the position
/// by one. This is the simplest read operation, useful for parsing byte-by-byte
/// or reading small amounts of data.
///
/// The byte is returned as a positive integer (0-255). If the end of file is
/// reached, -1 is returned and the EOF flag is set. This allows distinguishing
/// between a valid byte value and end-of-file.
///
/// **Example usage:**
/// ```
/// Dim b = bf.ReadByte()
/// While b >= 0
///     ' Process byte b (0-255)
///     b = bf.ReadByte()
/// Wend
/// ' b is -1 here, reached EOF
/// ```
///
/// @param obj Pointer to a BinFile object. Must not be NULL and file must be open.
///
/// @return The byte value (0-255) on success, or -1 if:
///         - End of file is reached (EOF flag is also set)
///         Traps and returns -1 if obj is NULL or file is closed.
///
/// @note Performance: For reading many bytes, rt_binfile_read into a Bytes
///       buffer is more efficient than calling ReadByte in a loop.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_read For bulk reading into a buffer
/// @see rt_binfile_write_byte For writing a single byte
/// @see rt_binfile_eof For checking end-of-file status
int64_t rt_binfile_read_byte(void *obj) {
    rt_binfile_impl *bf = binfile_require(obj, "BinFile.ReadByte: invalid file");
    if (!bf)
        return -1;
    if (!bf->fp || bf->closed) {
        rt_trap("BinFile.ReadByte: file is closed");
        return -1;
    }

    if (!binfile_prepare_read(bf))
        return -1;
    int c = fgetc(bf->fp);
    if (c == EOF) {
        if (ferror(bf->fp)) {
            rt_trap("BinFile.ReadByte: read failed");
            return -1;
        }
        bf->eof = 1;
        return -1;
    }

    bf->eof = 0;
    return (int64_t)(unsigned char)c;
}

/// @brief Writes a single byte to the file.
///
/// Writes one byte to the current file position and advances the position
/// by one. For files opened in append mode, the byte is written to the end
/// of the file.
///
/// Values outside 0-255 trap; accepted values are written unchanged.
///
/// **Example usage:**
/// ```
/// ' Write a simple header
/// bf.WriteByte(0x89)
/// bf.WriteByte(0x50)
/// bf.WriteByte(0x4E)
/// bf.WriteByte(0x47)
/// ```
///
/// @param obj Pointer to a BinFile object. Must not be NULL and file must be open.
/// @param byte The byte value to write (0 through 255).
///
/// @note This function traps on failure conditions:
///       - NULL obj
///       - File is closed
///       - Write failure (disk full, I/O error)
///
/// @note Performance: For writing many bytes, rt_binfile_write from a Bytes
///       buffer is more efficient than calling WriteByte in a loop.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_write For bulk writing from a buffer
/// @see rt_binfile_read_byte For reading a single byte
void rt_binfile_write_byte(void *obj, int64_t byte) {
    rt_binfile_impl *bf = binfile_require(obj, "BinFile.WriteByte: invalid file");
    if (!bf)
        return;
    if (!bf->fp || bf->closed) {
        rt_trap("BinFile.WriteByte: file is closed");
        return;
    }
    if (byte < 0 || byte > 255) {
        rt_trap("BinFile.WriteByte: byte value out of range");
        return;
    }

    if (!binfile_prepare_write(bf))
        return;
    if (fputc((unsigned char)byte, bf->fp) == EOF) {
        rt_trap("BinFile.WriteByte: write failed");
    }
}

/// @brief Moves the file position to a new location.
///
/// Changes the current read/write position within the file. The new position
/// is calculated based on the `origin` parameter and the `offset`:
///
/// | Origin | Name     | Description                           |
/// |--------|----------|---------------------------------------|
/// | 0      | SEEK_SET | Offset from beginning of file         |
/// | 1      | SEEK_CUR | Offset from current position          |
/// | 2      | SEEK_END | Offset from end of file (often <= 0)  |
///
/// After a successful seek, the EOF flag is cleared. This allows continuing
/// to read after previously reaching end-of-file.
///
/// **Example usage:**
/// ```
/// bf.Seek(0, 0)      ' Go to beginning (origin=SEEK_SET)
/// bf.Seek(100, 0)    ' Go to byte 100 from start
/// bf.Seek(-10, 2)    ' Go to 10 bytes before end
/// bf.Seek(5, 1)      ' Move forward 5 bytes from current position
/// ```
///
/// @param obj Pointer to a BinFile object. Must not be NULL and file must be open.
/// @param offset Number of bytes to move from the origin. May be negative
///               for SEEK_CUR and SEEK_END.
/// @param origin Reference point for the seek: 0=beginning, 1=current, 2=end.
///
/// @return The new absolute file position (bytes from beginning) on success,
///         or -1 if the seek failed (e.g., seeking before start of file).
///         Traps and returns -1 if obj is NULL, file is closed, or origin is invalid.
///
/// @note Seeking beyond EOF is allowed and extends the file on next write.
/// @note Positions are 64-bit on Windows and use the platform's `off_t` range
///       on POSIX; offsets not representable by `off_t` are rejected.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_pos For getting current position without moving
/// @see rt_binfile_size For getting total file size
int64_t rt_binfile_seek(void *obj, int64_t offset, int64_t origin) {
    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Seek: invalid file");
    if (!bf)
        return -1;
    if (!bf->fp || bf->closed) {
        rt_trap("BinFile.Seek: file is closed");
        return -1;
    }

    int whence;
    switch (origin) {
        case 0:
            whence = SEEK_SET;
            break;
        case 1:
            whence = SEEK_CUR;
            break;
        case 2:
            whence = SEEK_END;
            break;
        default:
            rt_trap("BinFile.Seek: invalid origin (use 0, 1, or 2)");
            return -1;
    }

    if (!binfile_prepare_seek(bf))
        return -1;
#if !defined(_WIN32)
    if ((int64_t)(off_t)offset != offset)
        return -1;
#endif
    if (binfile_fseek(bf->fp, offset, whence) != 0) {
        return -1;
    }

    // Clear EOF flag after successful seek
    bf->eof = 0;
    clearerr(bf->fp);

    return (int64_t)binfile_ftell(bf->fp);
}

/// @brief Returns the current file position.
///
/// Gets the current read/write position within the file, measured in bytes
/// from the beginning of the file. This is useful for:
/// - Saving position before a read operation to restore later
/// - Calculating how much data has been read/written
/// - Validating that seeks worked correctly
///
/// @param obj Pointer to a BinFile object. May be NULL (returns -1).
///
/// @return The current position in bytes from the start of the file,
///         or -1 if obj is NULL, file is closed, or an error occurred.
///
/// @note O(1) time complexity - just queries the file handle.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_seek For changing the position
/// @see rt_binfile_size For getting total file size
int64_t rt_binfile_pos(void *obj) {
    if (!obj)
        return -1;

    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Pos: invalid file");
    if (!bf)
        return -1;
    if (!bf->fp || bf->closed)
        return -1;

    if (!binfile_prepare_seek(bf))
        return -1;
    return (int64_t)binfile_ftell(bf->fp);
}

/// @brief Returns the total size of the file in bytes.
///
/// Determines the file size by seeking to the end and reading the position,
/// then restoring the original position. This operation does not modify the
/// logical file position from the caller's perspective.
///
/// For files opened in write or append mode, this returns the current size
/// which may change as data is written.
///
/// **Example usage:**
/// ```
/// Dim size = bf.Size()
/// Dim buf = Bytes.New(size)
/// bf.Seek(0, 0)
/// bf.Read(buf, 0, size)  ' Read entire file
/// ```
///
/// @param obj Pointer to a BinFile object. May be NULL (returns -1).
///
/// @return The total file size in bytes, or -1 if:
///         - obj is NULL
///         - File is closed
///         - An error occurred during the seek operations
///
/// @note This function temporarily modifies the file position internally
///       but restores it before returning.
/// @note The representable range follows `_ftelli64` on Windows and the
///       platform's `off_t` on POSIX.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_pos For getting current position
/// @see rt_binfile_seek For navigation
int64_t rt_binfile_size(void *obj) {
    if (!obj)
        return -1;

    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Size: invalid file");
    if (!bf)
        return -1;
    if (!bf->fp || bf->closed)
        return -1;

    // Save current position (IO-C-3: use 64-bit tell/seek for >2GB files)
    if (!binfile_prepare_seek(bf))
        return -1;
    int64_t pos = (int64_t)binfile_ftell(bf->fp);
    if (pos < 0)
        return -1;

    // Seek to end
    if (binfile_fseek(bf->fp, 0, SEEK_END) != 0)
        return -1;

    int64_t size = (int64_t)binfile_ftell(bf->fp);
    if (size < 0) {
        (void)binfile_fseek(bf->fp, pos, SEEK_SET);
        return -1;
    }

    // Restore position
    if (binfile_fseek(bf->fp, pos, SEEK_SET) != 0) {
        rt_trap("BinFile.Size: failed to restore file position");
        return -1;
    }

    return size;
}

/// @brief Flushes buffered data to disk without closing the file.
///
/// Forces any data that has been written but is still in memory buffers
/// to be written to the underlying storage device. This is useful when:
/// - Ensuring data durability at specific points in the program
/// - Making data visible to other processes reading the same file
/// - Implementing a checkpoint/save mechanism
///
/// Note that even after flush, the OS may still buffer data. For maximum
/// durability, consider using OS-specific sync mechanisms.
///
/// @param obj Pointer to a BinFile object. `NULL` or a closed valid object is
/// a no-op; an unrelated runtime handle traps.
///
/// @note A native flush failure raises a trap; this call does not perform an
/// OS-level durability sync such as `fsync`.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_close For closing and flushing the file
void rt_binfile_flush(void *obj) {
    if (!obj)
        return;

    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Flush: invalid file");
    if (!bf)
        return;
    if (!bf->fp || bf->closed)
        return;

    if (fflush(bf->fp) != 0) {
        rt_trap("BinFile.Flush: flush failed (disk full or I/O error)");
        return;
    }
    bf->last_op = BINFILE_OP_NONE;
}

/// @brief Checks whether the end of file has been reached.
///
/// Returns true if a previous read operation encountered the end of the file.
/// The EOF flag is set when:
/// - rt_binfile_read reads fewer bytes than requested due to EOF
/// - rt_binfile_read_byte returns -1 due to EOF
///
/// The EOF flag is cleared when:
/// - rt_binfile_seek successfully moves the position
/// - a later read successfully obtains data
/// - the stream transitions to writing
///
/// **Example usage:**
/// ```
/// While Not bf.EOF()
///     Dim line = ReadSomething(bf)
///     ProcessLine(line)
/// Wend
/// ```
///
/// @param obj Pointer to a BinFile object. May be NULL.
///
/// @return 1 (true) if EOF has been reached, or if obj is NULL, or if file
///         is closed. Returns 0 (false) if the file is open and EOF has not
///         been encountered.
///
/// @note Repeated reads at the same end position continue to report EOF until
/// the position or stream direction changes.
/// @note Thread safety: Not thread-safe for the same BinFile object.
///
/// @see rt_binfile_read For operations that set the EOF flag
/// @see rt_binfile_seek For clearing the EOF flag
int8_t rt_binfile_eof(void *obj) {
    if (!obj)
        return 1;

    rt_binfile_impl *bf = binfile_require(obj, "BinFile.Eof: invalid file");
    if (!bf)
        return 1;
    if (!bf->fp || bf->closed)
        return 1;

    return bf->eof;
}
