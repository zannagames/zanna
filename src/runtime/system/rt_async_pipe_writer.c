//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_async_pipe_writer.c
// Purpose: Implements bounded copied input queues for synchronous Windows
//          anonymous pipes without blocking Process or ConPTY callers.
//
// Key invariants:
//   - Only the worker thread calls WriteFile.
//   - Queue mutation is serialized by one critical section.
//   - Shutdown cancels a blocked synchronous write before its bounded join.
//
// Ownership/Lifetime:
//   - The writer owns its queue, event, thread, and lock.
//   - The destination pipe is borrowed and remains owned by Process or Pty.
//
// Links: src/runtime/system/rt_async_pipe_writer.h
//
//===----------------------------------------------------------------------===//

#include "rt_async_pipe_writer.h"

#include "rt_platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct rt_async_pipe_writer {
    HANDLE pipe;
    HANDLE thread;
    HANDLE event;
    CRITICAL_SECTION lock;
    char *queue;
    size_t queue_len;
    size_t queue_cap;
    size_t queue_max;
    int lock_initialized;
    int shutdown;
    int failed;
};

static DWORD WINAPI async_pipe_writer_main(LPVOID context) {
    rt_async_pipe_writer *writer = (rt_async_pipe_writer *)context;
    if (!writer)
        return 1;
    for (;;) {
        if (WaitForSingleObject(writer->event, INFINITE) != WAIT_OBJECT_0)
            return 1;
        for (;;) {
            char chunk[4096];
            size_t chunk_len = 0;
            EnterCriticalSection(&writer->lock);
            if (writer->shutdown) {
                LeaveCriticalSection(&writer->lock);
                return 0;
            }
            chunk_len = writer->queue_len < sizeof(chunk) ? writer->queue_len : sizeof(chunk);
            if (chunk_len > 0)
                memcpy(chunk, writer->queue, chunk_len);
            LeaveCriticalSection(&writer->lock);
            if (chunk_len == 0)
                break;

            DWORD written = 0;
            BOOL ok = WriteFile(writer->pipe, chunk, (DWORD)chunk_len, &written, NULL);
            EnterCriticalSection(&writer->lock);
            if (!ok || written == 0 || (size_t)written > writer->queue_len) {
                writer->failed = 1;
                writer->queue_len = 0;
                LeaveCriticalSection(&writer->lock);
                return 1;
            }
            size_t remaining = writer->queue_len - (size_t)written;
            if (remaining > 0)
                memmove(writer->queue, writer->queue + written, remaining);
            writer->queue_len = remaining;
            LeaveCriticalSection(&writer->lock);
        }
    }
}

rt_async_pipe_writer *rt_async_pipe_writer_create(void *native_handle, size_t max_queued_bytes) {
    HANDLE pipe = (HANDLE)native_handle;
    if (!pipe || pipe == INVALID_HANDLE_VALUE || max_queued_bytes == 0)
        return NULL;
    rt_async_pipe_writer *writer = (rt_async_pipe_writer *)calloc(1, sizeof(*writer));
    if (!writer)
        return NULL;
    writer->pipe = pipe;
    writer->queue_max = max_queued_bytes;
    InitializeCriticalSection(&writer->lock);
    writer->lock_initialized = 1;
    writer->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (writer->event)
        writer->thread = CreateThread(NULL, 0, async_pipe_writer_main, writer, 0, NULL);
    if (!writer->event || !writer->thread) {
        (void)rt_async_pipe_writer_destroy(writer, 0);
        return NULL;
    }
    return writer;
}

int64_t rt_async_pipe_writer_enqueue(rt_async_pipe_writer *writer, const char *bytes, size_t len) {
    if (len == 0)
        return 0;
    if (!writer || !bytes || !writer->thread || !writer->event)
        return -1;
    EnterCriticalSection(&writer->lock);
    if (writer->shutdown || writer->failed || !writer->pipe) {
        LeaveCriticalSection(&writer->lock);
        return -1;
    }
    size_t available = writer->queue_max - writer->queue_len;
    size_t accepted = len < available ? len : available;
    if (accepted == 0) {
        LeaveCriticalSection(&writer->lock);
        return -1;
    }
    size_t needed = writer->queue_len + accepted;
    if (needed > writer->queue_cap) {
        size_t capacity = writer->queue_cap ? writer->queue_cap : 4096u;
        while (capacity < needed && capacity <= writer->queue_max / 2u)
            capacity *= 2u;
        if (capacity < needed)
            capacity = needed;
        char *grown = (char *)realloc(writer->queue, capacity);
        if (!grown) {
            LeaveCriticalSection(&writer->lock);
            return -1;
        }
        writer->queue = grown;
        writer->queue_cap = capacity;
    }
    memcpy(writer->queue + writer->queue_len, bytes, accepted);
    writer->queue_len += accepted;
    LeaveCriticalSection(&writer->lock);
    SetEvent(writer->event);
    return (int64_t)accepted;
}

int rt_async_pipe_writer_destroy(rt_async_pipe_writer *writer, uint32_t timeout_ms) {
    if (!writer)
        return 1;
    if (writer->lock_initialized) {
        EnterCriticalSection(&writer->lock);
        writer->shutdown = 1;
        LeaveCriticalSection(&writer->lock);
    }
    if (writer->event)
        SetEvent(writer->event);
    int stopped = 1;
    if (writer->thread) {
        (void)CancelSynchronousIo(writer->thread);
        DWORD wait_ms = timeout_ms == 0 ? 1u : (DWORD)timeout_ms;
        stopped = WaitForSingleObject(writer->thread, wait_ms) == WAIT_OBJECT_0;
        if (!stopped)
            return 0;
        CloseHandle(writer->thread);
    }
    if (writer->event)
        CloseHandle(writer->event);
    free(writer->queue);
    if (writer->lock_initialized)
        DeleteCriticalSection(&writer->lock);
    free(writer);
    return stopped;
}

#else

struct rt_async_pipe_writer {
    int unused;
};

rt_async_pipe_writer *rt_async_pipe_writer_create(void *native_handle, size_t max_queued_bytes) {
    (void)native_handle;
    (void)max_queued_bytes;
    return NULL;
}

int64_t rt_async_pipe_writer_enqueue(rt_async_pipe_writer *writer, const char *bytes, size_t len) {
    (void)writer;
    (void)bytes;
    return len == 0 ? 0 : -1;
}

int rt_async_pipe_writer_destroy(rt_async_pipe_writer *writer, uint32_t timeout_ms) {
    (void)writer;
    (void)timeout_ms;
    return 1;
}

#endif
