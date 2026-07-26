//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_stream.c
// Purpose: Implements the unified stream abstraction that wraps either a BinFile
//          (disk-backed) or a MemStream (in-memory buffer) behind a common
//          read/write/seek/tell interface used by the Zanna.IO.Stream class.
//
// Key invariants:
//   - A stream wraps exactly one underlying object (BinFile or MemStream).
//   - The type tag (RT_STREAM_TYPE_BINFILE or RT_STREAM_TYPE_MEMSTREAM) is set
//     at construction and never changes.
//   - When owns==1, the stream holds a reference and releases it on close.
//   - Operations other than Close trap on NULL handles; Close(NULL) is a no-op.
//   - Seek positions are byte offsets; negative offsets from SEEK_END are valid.
//
// Ownership/Lifetime:
//   - If the stream owns the wrapped object, it releases it when the stream is
//     closed or garbage collected.
//   - FromBinFile/FromMemStream retain the existing object but do not close it;
//     the original owner remains responsible for an explicit Close where needed.
//
// Links: src/runtime/io/rt_stream.h (public API),
//        src/runtime/io/rt_binfile.h (disk-backed binary file),
//        src/runtime/io/rt_memstream.h (in-memory binary stream)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements a common managed wrapper over BinFile and MemStream.
 * @details Validates backing types, forwards position and I/O operations,
 * right-sizes short file reads, distinguishes newly owned from retained
 * backings, performs transactional wrapper construction, and detaches or
 * explicitly closes resources according to wrapper provenance.
 */

#include "rt_stream.h"
#include "rt_binfile.h"
#include "rt_bytes.h"
#include "rt_io_class_ids.h"
#include "rt_memstream.h"
#include "rt_object.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External trap function (defined in rt_io.c)
#include "rt_trap.h"

/// @copydoc rt_trap_set_recovery()
void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
const char *rt_trap_get_error(void);

//=============================================================================
// Internal Stream Structure
//=============================================================================

/// @brief Internal representation of a Stream handle.
typedef struct {
    int64_t type;          ///< RT_STREAM_TYPE_BINFILE or RT_STREAM_TYPE_MEMSTREAM
    void *wrapped;         ///< The wrapped BinFile or MemStream
    int8_t owns;           ///< Whether this Stream holds a reference on the wrapped object
    int8_t closes_wrapped; ///< Whether Close should close the wrapped object first.
    int8_t closed;         ///< Set to 1 once Close has been called
} stream_impl;

/// @brief Determine whether an object is a structurally valid Stream handle.
/// @details Validation checks the runtime class identifier and verifies that
///          the managed payload is large enough to contain @ref stream_impl;
///          it does not require the stream to remain open.
/// @param obj Candidate runtime object; may be NULL.
/// @return 1 when @p obj is a Stream instance, otherwise 0.
static int stream_is_handle(void *obj) {
    return rt_obj_is_instance(obj, RT_STREAM_CLASS_ID, sizeof(stream_impl)) ? 1 : 0;
}

/// @brief Decrement the refcount on a GC object and free it when it reaches zero.
/// @param obj Managed object reference to release; NULL is a no-op.
static void stream_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic into caller-owned storage.
/// @details The fallback is used when the trap subsystem has no non-empty
///          diagnostic, allowing cleanup code to clear the recovery boundary
///          before safely rethrowing a stable message.
/// @param buffer Destination for the terminated diagnostic string.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Diagnostic to copy when no active trap text is available.
static void stream_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Retain a backing object while preserving transactional wrapper creation.
/// @details If retaining traps, the not-yet-published Stream wrapper is
///          released and the saved diagnostic is rethrown after the local
///          recovery boundary is cleared. A NULL backing requires no retain.
/// @param s Fresh Stream wrapper to release if retaining fails.
/// @param wrapped Existing managed backing object to retain; may be NULL.
/// @param fallback Diagnostic used when the recovered trap has no message.
/// @return 1 after a successful retain or for NULL, otherwise 0 after cleanup
///         and propagation through trap recovery.
static int stream_retain_wrapped_or_cleanup(stream_impl *s, void *wrapped, const char *fallback) {
    if (!wrapped)
        return 1;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        stream_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        stream_release_object(s);
        rt_trap(saved_error);
        return 0;
    }

    rt_obj_retain_maybe(wrapped);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Tear down the stream's reference to its underlying BinFile/MemStream.
///
/// @details Clears `wrapped` and marks the stream closed before invoking a
///          backing close or managed release, so re-entrant cleanup cannot
///          observe a still-attached resource. Fresh OpenFile wrappers close
///          their BinFile; From* wrappers release only their retained reference.
///          Explicit Close propagates a delayed flush/close trap, whereas GC
///          finalization suppresses that error after ensuring the handle and
///          managed reference are released.
/// @param s Stream implementation to detach; NULL is a no-op.
/// @param report_close_error Non-zero for explicit Close, zero for finalization.
static void stream_release_wrapped(stream_impl *s, int report_close_error) {
    if (!s)
        return;

    void *wrapped = s->wrapped;
    int64_t type = s->type;
    int8_t closes_wrapped = s->closes_wrapped;
    s->wrapped = NULL;
    s->closed = 1;
    s->closes_wrapped = 0;
    if (s->owns && closes_wrapped && type == RT_STREAM_TYPE_BINFILE && wrapped) {
        char saved_error[256];
        int close_trapped = 0;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            stream_save_trap_error(
                saved_error, sizeof(saved_error), "Stream.Close: wrapped close failed");
            close_trapped = 1;
        } else {
            rt_binfile_close(wrapped);
        }
        rt_trap_clear_recovery();
        if (s->owns)
            stream_release_object(wrapped);
        if (close_trapped && report_close_error)
            rt_trap(saved_error);
        return;
    }
    if (s->owns)
        stream_release_object(wrapped);
}

/// @brief Release a Bytes object returned from a BinFile read, freeing it when no longer
/// referenced.
/// @param bytes Owned managed Bytes reference; NULL is a no-op.
static void stream_dispose_bytes(void *bytes) {
    if (bytes && rt_obj_release_check0(bytes))
        rt_obj_free(bytes);
}

/// @brief Right-size a read buffer after a possibly-short read.
///
/// @details BinFile reads allocate a buffer sized to the requested count but
///          may return fewer bytes at EOF. This helper slices the valid prefix
///          and eagerly releases the oversized buffer. A local trap boundary
///          guarantees that a failed slice allocation also releases the
///          original before the error propagates. Full reads avoid the copy.
/// @param bytes Owned oversized Bytes object.
/// @param len Number of valid prefix bytes.
/// @return Owned right-sized Bytes, or NULL after cleanup/trap.
static void *stream_shrink_bytes(void *bytes, int64_t len) {
    if (!bytes)
        return rt_bytes_new(0);
    if (len <= 0) {
        stream_dispose_bytes(bytes);
        return rt_bytes_new(0);
    }
    if (rt_bytes_len(bytes) == len)
        return bytes;

    void *volatile owned_bytes = bytes;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        stream_save_trap_error(saved_error, sizeof(saved_error), "Stream.Read: resize failed");
        rt_trap_clear_recovery();
        stream_dispose_bytes((void *)owned_bytes);
        rt_trap(saved_error);
        return NULL;
    }
    void *slice = rt_bytes_slice(bytes, 0, len);
    rt_trap_clear_recovery();
    stream_dispose_bytes((void *)owned_bytes);
    return slice;
}

/// @brief Allocate a BinFile read buffer and keep it owned across all trapping operations.
/// @details Both the backing read and short-read slicing may trap. The active
///          buffer is recorded in a volatile owner slot until ownership moves
///          into @ref stream_shrink_bytes, ensuring every non-local exit frees
///          exactly one allocation. The BinFile cursor retains ordinary fread
///          semantics: it advances by the number of bytes the host read before
///          any reported error.
/// @param binfile Open BinFile backing object.
/// @param count Maximum byte count to read; must be positive.
/// @param fallback Caller-specific diagnostic for allocation/read failure.
/// @return Owned Bytes sized to the actual read, or NULL after cleanup/trap.
static void *stream_read_binfile_bytes(void *binfile, int64_t count, const char *fallback) {
    void *volatile active_bytes = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        stream_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        stream_dispose_bytes((void *)active_bytes);
        rt_trap(saved_error);
        return NULL;
    }

    active_bytes = rt_bytes_new(count);
    if (!active_bytes) {
        rt_trap(fallback);
        rt_trap_clear_recovery();
        return NULL;
    }
    int64_t read = rt_binfile_read(binfile, (void *)active_bytes, 0, count);
    if (read < 0) {
        rt_trap(fallback);
        rt_trap_clear_recovery();
        stream_dispose_bytes((void *)active_bytes);
        return NULL;
    }

    void *oversized = (void *)active_bytes;
    active_bytes = NULL;
    void *result = stream_shrink_bytes(oversized, read);
    rt_trap_clear_recovery();
    return result;
}

/// @brief Allocate and zero-initialize a new stream_impl GC object; traps on OOM.
/// @return Fresh managed Stream payload, or NULL after reporting allocation
///         failure through the runtime trap mechanism.
static stream_impl *stream_alloc(void) {
    stream_impl *s = (stream_impl *)rt_obj_new_i64(RT_STREAM_CLASS_ID, sizeof(stream_impl));
    if (!s) {
        rt_trap("Stream: memory allocation failed");
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    return s;
}

/// @brief Allocate a Stream wrapper while retaining cleanup ownership of a fresh backing object.
/// @details OpenFile/OpenMemory/OpenBytes create the backing object first. If
///          allocation of the wrapper performs a recoverable longjmp, this
///          helper releases that backing object before propagating the trap;
///          its finalizer closes any native file handle. On success ownership
///          remains with the caller, which installs it into the wrapper.
/// @param wrapped Fresh backing object owned by the calling constructor.
/// @param fallback Diagnostic used when no specific recovered error is available.
/// @return Fresh zeroed Stream payload, or NULL after releasing @p wrapped.
static stream_impl *stream_alloc_or_release_owned(void *wrapped, const char *fallback) {
    void *volatile owned_wrapped = wrapped;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        stream_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        stream_release_object((void *)owned_wrapped);
        rt_trap(saved_error);
        return NULL;
    }

    stream_impl *s = stream_alloc();
    rt_trap_clear_recovery();
    if (!s) {
        stream_release_object(wrapped);
        return NULL;
    }
    return s;
}

/// @brief Trap-guarded dereference: returns the `stream_impl` or traps.
///
/// Two failure modes: NULL handle (use-after-new-fail or uninitialized
/// field) traps with the caller-provided context, and already-closed
/// stream (use-after-close) traps with a generic message. Saves every
/// public entry point from having to open-code these two checks.
/// @param stream Candidate Stream handle.
/// @param context Diagnostic used when @p stream is NULL or not a Stream.
/// @return Open Stream payload, or NULL after raising a runtime trap.
static stream_impl *stream_require_open(void *stream, const char *context) {
    if (!stream_is_handle(stream)) {
        rt_trap(context);
        return NULL;
    }
    stream_impl *s = (stream_impl *)stream;
    if (s->closed || !s->wrapped) {
        rt_trap("Stream: stream is closed");
        return NULL;
    }
    return s;
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief GC finalizer: releases the wrapped BinFile/MemStream when the stream object is collected.
/// @details Backing close errors are suppressed because finalizers cannot
///          report them, but the native handle and retained managed reference
///          are still detached and released.
/// @param obj Managed Stream payload being finalized.
static void stream_finalizer(void *obj) {
    stream_impl *s = (stream_impl *)obj;
    stream_release_wrapped(s, 0);
}

//=============================================================================
// Stream Creation
//=============================================================================

/// @brief Open a file-backed Stream using BinFile mode semantics.
/// @details The new Stream owns and explicitly closes the created BinFile.
///          Construction is transactional: failure to allocate the wrapper
///          releases the newly opened backing object and its native handle.
/// @param path Runtime string containing the file-system path.
/// @param mode Runtime string accepted by @ref rt_binfile_open, such as
///             `"r"`, `"w"`, `"rb"`, `"wb"`, `"a"`, or `"r+"`.
/// @return Fresh managed Stream, or NULL after an open/allocation trap.
void *rt_stream_open_file(rt_string path, rt_string mode) {
    void *binfile = rt_binfile_open(path, mode);
    if (!binfile)
        return NULL;

    stream_impl *s =
        stream_alloc_or_release_owned(binfile, "Stream.OpenFile: wrapper allocation failed");
    if (!s)
        return NULL;
    s->type = RT_STREAM_TYPE_BINFILE;
    s->wrapped = binfile;
    s->owns = 1;
    s->closes_wrapped = 1;
    s->closed = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

/// @brief Open a Stream backed by a fresh in-memory buffer (zero-length, growable).
/// @details The Stream owns the new MemStream and releases it on Close or
///          finalization. Wrapper allocation failure releases the backing
///          object before propagating the trap.
/// @return Fresh managed Stream positioned at byte offset zero, or NULL after
///         an allocation trap.
void *rt_stream_open_memory(void) {
    void *memstream = rt_memstream_new();
    if (!memstream)
        return NULL;

    stream_impl *s =
        stream_alloc_or_release_owned(memstream, "Stream.OpenMemory: wrapper allocation failed");
    if (!s)
        return NULL;
    s->type = RT_STREAM_TYPE_MEMSTREAM;
    s->wrapped = memstream;
    s->owns = 1;
    s->closes_wrapped = 0;
    s->closed = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

/// @brief Open a memory-backed Stream containing a copy of existing bytes.
/// @details The input Bytes is validated and copied into a fresh MemStream
///          positioned at zero. It is not retained, so the resulting Stream
///          has independent storage and lifetime.
/// @param bytes Valid Bytes object whose complete payload supplies the initial content.
/// @return Fresh managed Stream, or NULL after validation/allocation failure.
void *rt_stream_open_bytes(void *bytes) {
    void *memstream = rt_memstream_from_bytes(bytes);
    if (!memstream)
        return NULL;

    stream_impl *s =
        stream_alloc_or_release_owned(memstream, "Stream.OpenBytes: wrapper allocation failed");
    if (!s)
        return NULL;
    s->type = RT_STREAM_TYPE_MEMSTREAM;
    s->wrapped = memstream;
    s->owns = 1;
    s->closes_wrapped = 0;
    s->closed = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

/// @brief Wrap an existing BinFile as a Stream, retaining it for the wrapper's lifetime.
/// @details Close or finalization releases the wrapper's retained reference but
///          does not explicitly close the caller-provided BinFile. Operations
///          therefore share the backing file position and state.
/// @param binfile Valid managed BinFile handle to retain.
/// @return Fresh managed Stream, or NULL after validation/retain/allocation failure.
void *rt_stream_from_binfile(void *binfile) {
    if (!rt_binfile_is_handle(binfile)) {
        rt_trap("Stream.FromBinFile: invalid binfile");
        return NULL;
    }

    stream_impl *s = stream_alloc();
    if (!s)
        return NULL;
    s->type = RT_STREAM_TYPE_BINFILE;
    if (!stream_retain_wrapped_or_cleanup(s, binfile, "Stream.FromBinFile: retain failed"))
        return NULL;
    s->wrapped = binfile;
    s->owns = 1;
    s->closes_wrapped = 0;
    s->closed = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

/// @brief Wrap an existing MemStream as a Stream, retaining it for the wrapper's lifetime.
/// @details The wrapper shares the backing buffer and cursor. Close or
///          finalization releases only the reference acquired here.
/// @param memstream Valid managed MemStream handle to retain.
/// @return Fresh managed Stream, or NULL after validation/retain/allocation failure.
void *rt_stream_from_memstream(void *memstream) {
    if (!rt_memstream_is_handle(memstream)) {
        rt_trap("Stream.FromMemStream: invalid memstream");
        return NULL;
    }

    stream_impl *s = stream_alloc();
    if (!s)
        return NULL;
    s->type = RT_STREAM_TYPE_MEMSTREAM;
    if (!stream_retain_wrapped_or_cleanup(s, memstream, "Stream.FromMemStream: retain failed"))
        return NULL;
    s->wrapped = memstream;
    s->owns = 1;
    s->closes_wrapped = 0;
    s->closed = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

//=============================================================================
// Stream Properties
//=============================================================================

/// @brief Return the stream type: RT_STREAM_TYPE_BINFILE or RT_STREAM_TYPE_MEMSTREAM.
/// @param stream Open Stream handle.
/// @return Backing discriminator, or -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_type(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.Type: null stream");
    if (!s)
        return -1;
    return s->type;
}

/// @brief Return the current read/write position within the stream.
/// @details The result is the byte offset reported by the active backing
///          object and is shared by reads and writes.
/// @param stream Open Stream handle.
/// @return Current byte offset, or -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_pos(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.Pos: null stream");
    if (!s)
        return -1;
    if (s->type == RT_STREAM_TYPE_BINFILE) {
        return rt_binfile_pos(s->wrapped);
    } else {
        return rt_memstream_get_pos(s->wrapped);
    }
}

/// @brief Seek to an absolute byte position within the stream.
/// @details File-backed streams delegate to an absolute BinFile seek.
///          Memory-backed streams use MemStream positioning, which permits a
///          position beyond the current logical length for a later sparse write.
/// @param stream Open Stream handle.
/// @param pos Non-negative absolute byte offset.
void rt_stream_set_pos(void *stream, int64_t pos) {
    stream_impl *s = stream_require_open(stream, "Stream.set_Pos: null stream");
    if (!s)
        return;

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        if (rt_binfile_seek(s->wrapped, pos, 0) < 0) {
            rt_trap("Stream.set_Pos: seek failed");
            return;
        }
    } else {
        rt_memstream_set_pos(s->wrapped, pos);
    }
}

/// @brief Return the current logical length of the stream in bytes.
/// @details File-backed length is queried from the BinFile; memory-backed
///          length is the MemStream's logical content length.
/// @param stream Open Stream handle.
/// @return Length in bytes, or -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_len(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.Len: null stream");
    if (!s)
        return -1;
    if (s->type == RT_STREAM_TYPE_BINFILE) {
        return rt_binfile_size(s->wrapped);
    } else {
        return rt_memstream_get_len(s->wrapped);
    }
}

/// @brief Report EOF with one position-based contract for both backings.
/// @details `Stream.Eof` is true exactly when the current position is at or past
///          the end of the stream (`pos >= size`), regardless of file vs memory
///          backing (VDOC-188). Polymorphic Stream code then observes the same
///          end-of-stream state for the same logical position — reading exactly
///          the remaining bytes (including `ReadAll`) leaves `Eof` true on both.
///          It intentionally does NOT use BinFile's sticky feof flag, which only
///          becomes true after a read PASSES the end. (`BinFile.Eof` keeps its
///          own C-stream sticky-flag semantics for direct BinFile use.)
/// @param stream Open Stream handle.
/// @return 1 when the cursor is at or beyond the measured length, otherwise 0;
///         returns 1 after an invalid/closed-stream trap.
int8_t rt_stream_is_eof(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.Eof: null stream");
    if (!s)
        return 1;

    int64_t pos;
    int64_t size;
    if (s->type == RT_STREAM_TYPE_BINFILE) {
        pos = rt_binfile_pos(s->wrapped);
        size = rt_binfile_size(s->wrapped);
    } else {
        pos = rt_memstream_get_pos(s->wrapped);
        size = rt_memstream_get_len(s->wrapped);
    }
    return pos >= size ? 1 : 0;
}

//=============================================================================
// Stream Operations
//=============================================================================

/// @brief Read up to `count` bytes from the current position.
/// @details For both file and memory backings, the allocation request is first
///          clamped to the measured bytes remaining. A caller asking for an
///          arbitrarily large count near EOF therefore cannot force an equally
///          large speculative allocation. The returned Bytes object is sized
///          to the actual read, which may still be shorter if a file changes
///          between the size snapshot and the host read.
/// @param stream Open Stream handle.
/// @param count Maximum number of bytes to consume; negative values trap.
/// @return Fresh owned Bytes containing the data read, including a fresh empty
///         object for zero count or EOF.
void *rt_stream_read(void *stream, int64_t count) {
    stream_impl *s = stream_require_open(stream, "Stream.Read: null stream");
    if (!s)
        return rt_bytes_new(0);
    if (count < 0) {
        rt_trap("Stream.Read: negative count");
        return rt_bytes_new(0);
    }
    if (count == 0)
        return rt_bytes_new(0);

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        int64_t pos = rt_binfile_pos(s->wrapped);
        int64_t size = rt_binfile_size(s->wrapped);
        if (pos < 0 || size < 0 || size < pos) {
            rt_trap("Stream.Read: invalid file position or size");
            return rt_bytes_new(0);
        }
        int64_t remaining = size - pos;
        if (remaining <= 0)
            return rt_bytes_new(0);
        if (count > remaining)
            count = remaining;
        return stream_read_binfile_bytes(s->wrapped, count, "Stream.Read: read failed");
    } else {
        int64_t pos = rt_memstream_get_pos(s->wrapped);
        int64_t len = rt_memstream_get_len(s->wrapped);
        int64_t remaining = len > pos ? len - pos : 0;
        if (remaining <= 0)
            return rt_bytes_new(0);
        if (count > remaining)
            count = remaining;
        return rt_memstream_read_bytes(s->wrapped, count);
    }
}

/// @brief Read every remaining byte from the current position to EOF.
/// @details The current length is measured before allocation. File-backed
///          reads may return a shorter right-sized result if the file changes
///          before the host read completes.
/// @param stream Open Stream handle.
/// @return Fresh owned Bytes containing the remaining data, or a fresh empty
///         object when already at EOF.
void *rt_stream_read_all(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.ReadAll: null stream");
    if (!s)
        return rt_bytes_new(0);

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        // Read remaining bytes from current position
        int64_t pos = rt_binfile_pos(s->wrapped);
        int64_t size = rt_binfile_size(s->wrapped);
        if (pos < 0 || size < 0 || size < pos) {
            rt_trap("Stream.ReadAll: invalid file position or size");
            return rt_bytes_new(0);
        }
        int64_t remaining = size - pos;

        if (remaining <= 0)
            return rt_bytes_new(0);

        return stream_read_binfile_bytes(s->wrapped, remaining, "Stream.ReadAll: read failed");
    } else {
        // Read remaining bytes from MemStream
        int64_t pos = rt_memstream_get_pos(s->wrapped);
        int64_t len = rt_memstream_get_len(s->wrapped);
        int64_t remaining = len - pos;

        if (remaining <= 0)
            return rt_bytes_new(0);

        return rt_memstream_read_bytes(s->wrapped, remaining);
    }
}

/// @brief Write an entire Bytes payload at the current stream position.
/// @details The operation advances the shared cursor by the backing object's
///          write semantics. Invalid Bytes objects trap before any write.
/// @param stream Open Stream handle.
/// @param bytes Valid Bytes object whose complete payload is written.
void rt_stream_write(void *stream, void *bytes) {
    stream_impl *s = stream_require_open(stream, "Stream.Write: null stream");
    if (!s)
        return;
    if (!bytes || !rt_bytes_is_bytes(bytes)) {
        rt_trap("Stream.Write: invalid bytes");
        return;
    }

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        int64_t len = rt_bytes_len(bytes);
        rt_binfile_write(s->wrapped, bytes, 0, len);
    } else {
        rt_memstream_write_bytes(s->wrapped, bytes);
    }
}

/// @brief Read a single byte from the stream at the current position and advance.
/// @details Returns -1 if the stream is at EOF.
/// @param stream Open Stream handle.
/// @return Unsigned byte value in the range 0 through 255, or -1 at EOF or
///         after an invalid/closed-stream trap.
int64_t rt_stream_read_byte(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.ReadByte: null stream");
    if (!s)
        return -1;

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        return rt_binfile_read_byte(s->wrapped);
    } else {
        // MemStream: read u8 or return -1 if at end
        int64_t pos = rt_memstream_get_pos(s->wrapped);
        int64_t len = rt_memstream_get_len(s->wrapped);
        if (pos >= len)
            return -1;
        return rt_memstream_read_u8(s->wrapped);
    }
}

/// @brief Write a single byte to the stream at the current position and advance.
/// @param stream Open Stream handle.
/// @param byte Unsigned byte value in the inclusive range 0 through 255.
void rt_stream_write_byte(void *stream, int64_t byte) {
    stream_impl *s = stream_require_open(stream, "Stream.WriteByte: null stream");
    if (!s)
        return;
    if (byte < 0 || byte > 255) {
        rt_trap("Stream.WriteByte: byte value out of range");
        return;
    }

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        rt_binfile_write_byte(s->wrapped, byte);
    } else {
        rt_memstream_write_u8(s->wrapped, byte);
    }
}

/// @brief Flush pending writes for a file-backed stream.
/// @details Delegates to BinFile flushing for disk-backed streams. Memory
///          streams require no flush and therefore make this operation a no-op.
/// @param stream Open Stream handle.
void rt_stream_flush(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.Flush: null stream");
    if (!s)
        return;

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        rt_binfile_flush(s->wrapped);
    }
    // MemStream doesn't need flushing
}

/// @brief Detach and release the Stream's backing object.
/// @details NULL is accepted as a no-op and repeated closes of a valid Stream
///          are idempotent. A Stream created by OpenFile explicitly closes its
///          BinFile and propagates close errors; From* wrappers only release
///          their retained reference.
/// @param stream Stream handle to close; may be NULL.
void rt_stream_close(void *stream) {
    if (!stream)
        return;
    if (!stream_is_handle(stream)) {
        rt_trap("Stream.Close: invalid stream");
        return;
    }
    stream_impl *s = (stream_impl *)stream;
    stream_release_wrapped(s, 1);
}

//=============================================================================
// Conversion
//=============================================================================

/// @brief Return an owned reference to a file-backed Stream's BinFile.
/// @details The backing object is retained before return, so it remains valid
///          after the Stream closes or is finalized. A memory-backed Stream traps.
/// @param stream Open file-backed Stream handle.
/// @return Retained BinFile reference that the caller must release, or NULL
///         after an invalid-handle or wrong-backing trap.
void *rt_stream_as_binfile(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.AsBinFile: null stream");
    if (!s)
        return NULL;

    if (s->type == RT_STREAM_TYPE_BINFILE) {
        // Return an OWNED reference so the typed backing object survives the
        // Stream being closed/finalized — the previous borrowed return left a
        // dangling handle after Close (VDOC-187). The caller releases it.
        rt_obj_retain_maybe(s->wrapped);
        return s->wrapped;
    }
    rt_trap("Stream.AsBinFile: stream is not file-backed");
    return NULL;
}

/// @brief Return an owned reference to the underlying MemStream (memory-backed Streams only).
/// @details A wrong backing type traps. The reference is retained, so it stays
///          valid after the Stream is closed; the caller releases it.
/// @param stream Open memory-backed Stream handle.
/// @return Retained MemStream reference that the caller must release, or NULL
///         after an invalid-handle or wrong-backing trap.
void *rt_stream_as_memstream(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.AsMemStream: null stream");
    if (!s)
        return NULL;

    if (s->type == RT_STREAM_TYPE_MEMSTREAM) {
        // Return an OWNED reference (see AsBinFile) so the MemStream survives
        // the Stream's close/finalize instead of dangling (VDOC-187).
        rt_obj_retain_maybe(s->wrapped);
        return s->wrapped;
    }
    rt_trap("Stream.AsMemStream: stream is not memory-backed");
    return NULL;
}

/// @brief Snapshot a memory-backed Stream's contents as a Bytes object.
/// @details File-backed Streams trap; use `_set_pos(0)` then `_read_all` instead.
///          Snapshotting does not change the Stream's current position.
/// @param stream Open memory-backed Stream handle.
/// @return Fresh owned Bytes copy of the complete memory buffer, or NULL after
///         an invalid-handle or wrong-backing trap.
void *rt_stream_to_bytes(void *stream) {
    stream_impl *s = stream_require_open(stream, "Stream.ToBytes: null stream");
    if (!s)
        return NULL;

    if (s->type == RT_STREAM_TYPE_MEMSTREAM) {
        return rt_memstream_to_bytes(s->wrapped);
    }
    rt_trap("Stream.ToBytes: stream is not memory-backed");
    return NULL;
}
