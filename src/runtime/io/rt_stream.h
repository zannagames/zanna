//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_stream.h
// Purpose: Unified stream interface abstracting BinFile and MemStream, providing a common API for
// read/write/seek regardless of the backing storage type.
//
// Key invariants:
//   - Stream type is one of STREAM_TYPE_BINFILE (0) or STREAM_TYPE_MEMSTREAM (1).
//   - The stream wraps its underlying object and forwards all calls transparently.
//   - Open* streams own their wrapped object; From* streams retain a reference
//     without assuming responsibility for explicitly closing an existing file.
//   - Operations on null or closed streams trap, except Close(NULL) is a no-op.
//   - Positions and lengths are byte counts; byte payloads are forwarded
//     exactly and do not impose an integer byte order.
//
// Ownership/Lifetime:
//   - Stream objects are runtime-managed and install a finalizer that releases
//     any still-attached backing object.
//   - Finalizing or closing an Open* stream releases its owned backing object;
//     OpenFile additionally closes the BinFile's native handle.
//   - Destroying or closing a From* wrapper releases its retained reference but
//     does not explicitly close the original object.
//
// Links: src/runtime/io/rt_stream.c (implementation), src/runtime/io/rt_binfile.h,
// src/runtime/io/rt_memstream.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the unified managed Stream wrapper for files and memory.
 * @details Defines backing discriminants, owning constructors, retained
 * wrappers, position/length/EOF properties, byte and Bytes I/O, flushing,
 * close semantics, backing extraction, and memory snapshot conversion.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Stream Type Constants
//=========================================================================

/// @brief Discriminant tag identifying the backing store of a Stream object.
typedef enum {
    RT_STREAM_TYPE_BINFILE = 0,
    RT_STREAM_TYPE_MEMSTREAM = 1,
} rt_stream_type_t;

//=========================================================================
// Stream Creation
//=========================================================================

/// @brief Create an owning stream wrapping a newly opened file.
/// @details Constructor allocation is transactional: if wrapper allocation
///          traps, the newly opened BinFile and native handle are released.
///          Closing or finalizing the returned wrapper explicitly closes the
///          created BinFile before releasing it.
/// @param path Runtime string containing the file-system path.
/// @param mode BinFile mode such as `"r"`, `"w"`, `"rb"`, `"wb"`, `"a"`,
///             or `"r+"`.
/// @return Fresh managed Stream, or NULL after open/allocation failure.
void *rt_stream_open_file(rt_string path, rt_string mode);

/// @brief Create an owning stream wrapping a new in-memory buffer.
/// @details A wrapper-allocation trap releases the fresh MemStream before it
///          propagates, so no partially constructed backing object escapes.
/// @return Fresh managed Stream with an empty growable buffer at position zero.
void *rt_stream_open_memory(void);

/// @brief Create a memory-backed Stream containing a copy of existing bytes.
/// @details The input is not retained; the returned Stream owns independent
///          storage, is positioned at zero, and is unaffected by the input's lifetime.
/// @param bytes Valid Bytes object providing the initial complete payload.
/// @return Fresh managed Stream, or NULL after validation/allocation failure.
void *rt_stream_open_bytes(void *bytes);

/// @brief Wrap and retain an existing BinFile in a Stream.
/// @details The wrapper shares the BinFile's cursor and state with other
///          references. Closing the wrapper does not explicitly close the file.
/// @param binfile Valid BinFile object to retain.
/// @return Owned Stream wrapper; Close releases its reference but does not
///         explicitly close the caller's existing BinFile.
void *rt_stream_from_binfile(void *binfile);

/// @brief Wrap and retain an existing MemStream in a Stream.
/// @details The wrapper shares the MemStream's buffer and cursor with the
///          caller's reference.
/// @param memstream Valid MemStream object to retain.
/// @return Owned Stream wrapper whose Close releases the retained reference.
void *rt_stream_from_memstream(void *memstream);

//=========================================================================
// Stream Properties
//=========================================================================

/// @brief Get the discriminator for the Stream's backing object.
/// @param stream Open Stream handle.
/// @return @ref RT_STREAM_TYPE_BINFILE or @ref RT_STREAM_TYPE_MEMSTREAM;
///         returns -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_type(void *stream);

/// @brief Get the current shared read/write position.
/// @param stream Open Stream handle.
/// @return Current byte offset, or -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_pos(void *stream);

/// @brief Set the absolute shared read/write position.
/// @details Memory streams permit positioning beyond their logical end for a
///          later sparse write; backing-specific validation failures trap.
/// @param stream Open Stream handle.
/// @param pos Non-negative absolute byte offset.
void rt_stream_set_pos(void *stream, int64_t pos);

/// @brief Get the current logical length of the backing data.
/// @param stream Open Stream handle.
/// @return Length in bytes, or -1 after an invalid/closed-stream trap.
int64_t rt_stream_get_len(void *stream);

/// @brief Test whether the current byte position is at or beyond the length.
/// @details This position-based contract is identical for both backing types
///          and does not expose BinFile's sticky host EOF flag.
/// @param stream Open Stream handle.
/// @return 1 at or beyond EOF and 0 before EOF; returns 1 after an
///         invalid/closed-stream trap.
int8_t rt_stream_is_eof(void *stream);

//=========================================================================
// Stream Operations
//=========================================================================

/// @brief Read up to a requested number of bytes from the current position.
/// @details Allocates no more than the bytes measured from the current position
///          to EOF, even when @p count is much larger. A concurrently changing
///          file may still produce a shorter result.
/// @param stream Open Stream handle.
/// @param count Maximum number of bytes to consume; negative values trap.
/// @return Fresh owned Bytes containing the data read; zero count and EOF
///         produce a fresh empty object.
void *rt_stream_read(void *stream, int64_t count);

/// @brief Read all bytes from the current position through the measured end.
/// @details A concurrently changing file may produce a shorter right-sized result.
/// @param stream Open Stream handle.
/// @return Fresh owned Bytes containing all remaining data, or a fresh empty
///         object at EOF.
void *rt_stream_read_all(void *stream);

/// @brief Write an entire Bytes payload at the current position.
/// @param stream Open Stream handle.
/// @param bytes Valid Bytes object whose complete payload is written.
void rt_stream_write(void *stream, void *bytes);

/// @brief Read one unsigned byte and advance the position.
/// @param stream Open Stream handle.
/// @return Value from 0 through 255, or -1 at EOF or after an
///         invalid/closed-stream trap.
int64_t rt_stream_read_byte(void *stream);

/// @brief Write one unsigned byte and advance the position.
/// @param stream Open Stream handle.
/// @param byte Value in the inclusive range 0 through 255; other values trap.
void rt_stream_write_byte(void *stream, int64_t byte);

/// @brief Flush pending writes for a file-backed Stream.
/// @details Memory-backed Streams require no flush and treat this as a no-op.
/// @param stream Open Stream handle.
void rt_stream_flush(void *stream);

/// @brief Detach and release the Stream's backing object.
/// @details NULL and repeated closes are no-ops. OpenFile wrappers explicitly
///          close their BinFile and may propagate a close error; From*
///          wrappers only release their retained reference.
/// @param stream Stream handle to close; may be NULL.
void rt_stream_close(void *stream);

//=========================================================================
// Conversion
//=========================================================================

/// @brief Get a retained reference to the underlying BinFile.
/// @details The retained result remains valid after the Stream is closed or finalized.
/// @param stream Open file-backed Stream handle.
/// @return Owned BinFile reference that the caller must release; traps if this
///         is not a file-backed Stream.
void *rt_stream_as_binfile(void *stream);

/// @brief Get a retained reference to the underlying MemStream.
/// @details The retained result remains valid after the Stream is closed or finalized.
/// @param stream Open memory-backed Stream handle.
/// @return Owned MemStream reference that the caller must release; traps if
///         this is not a memory-backed Stream.
void *rt_stream_as_memstream(void *stream);

/// @brief Copy the complete contents of a memory-backed Stream into Bytes.
/// @details Snapshotting does not change the Stream's current position.
/// @param stream Open memory-backed Stream handle.
/// @return Fresh owned Bytes copy; traps if this is not a memory-backed Stream.
void *rt_stream_to_bytes(void *stream);

#ifdef __cplusplus
}
#endif
