//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_async_pipe_writer.h
// Purpose: Declares the bounded asynchronous writer shared by Windows Process
//          and ConPTY input pipes.
//
// Key invariants:
//   - Enqueue copies accepted bytes before returning.
//   - Enqueue never performs a synchronous operating-system write.
//   - Destroy bounds writer shutdown and never raises a runtime trap.
//
// Ownership/Lifetime:
//   - Create returns an owned writer that borrows its target pipe handle.
//   - The target handle must remain open until Destroy returns.
//
// Links: src/runtime/system/rt_async_pipe_writer.c,
//        src/runtime/system/rt_process.c, src/runtime/system/rt_pty.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_RUNTIME_SYSTEM_RT_ASYNC_PIPE_WRITER_H
#define ZANNA_RUNTIME_SYSTEM_RT_ASYNC_PIPE_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct rt_async_pipe_writer rt_async_pipe_writer;

/// @brief Create a bounded writer for a borrowed native Windows pipe handle.
/// @param native_handle Borrowed HANDLE represented without exposing windows.h.
/// @param max_queued_bytes Maximum copied bytes awaiting the writer thread.
/// @return Owned writer, or NULL on unsupported platforms/setup failure.
rt_async_pipe_writer *rt_async_pipe_writer_create(void *native_handle, size_t max_queued_bytes);

/// @brief Copy as many bytes as capacity permits into the writer queue.
/// @return Accepted byte count, zero for empty input, or -1 on failure/full queue.
int64_t rt_async_pipe_writer_enqueue(rt_async_pipe_writer *writer, const char *bytes, size_t len);

/// @brief Stop and release a writer without closing its borrowed pipe handle.
/// @param writer Writer to release; NULL is ignored.
/// @param timeout_ms Maximum Windows join interval.
/// @return 1 when the worker stopped cleanly, otherwise 0.
int rt_async_pipe_writer_destroy(rt_async_pipe_writer *writer, uint32_t timeout_ms);

#endif
