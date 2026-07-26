//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_socket_platform_win.c
// Purpose: WinSock adapter implementation for the runtime networking stack.
//
// Key invariants:
//   - One caller performs each WSAStartup attempt; waiters retry if that attempt
//     fails instead of spinning forever on an abandoned in-progress state.
//   - The successful WinSock startup reference is process-lifetime state. Native
//     PE programs may use Zanna's CRT-less entry shim, so this adapter never
//     registers a CRT atexit callback.
//   - Socket readiness waits preserve one GetTickCount64 deadline across
//     WSAEINTR and rebuild select()'s mutable fd/timeval inputs on every retry.
//
// Ownership/Lifetime:
//   - The OS reclaims the process-lifetime WinSock startup reference at process
//     teardown. The adapter does not own sockets except during rt_socket_close().
//
// Links: src/runtime/network/rt_socket_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_socket_platform_win.c
 * @brief Implements the WinSock side of the native socket adapter.
 * @details The adapter performs process-lifetime WinSock initialization,
 *          wraps handle lifecycle and WSA error classification, configures
 *          nonblocking and timeout behavior, and rebuilds mutable select state
 *          while preserving one deadline across interruptions.
 */

#include "rt_socket_platform.h"

#include "rt_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_wsa_init_state = 0; // 0=uninit, 1=in-progress, 2=done

static void rt_socket_set_last_error(int error);

/// @brief Initialize WinSock once for the process.
/// @details Uses a three-state atomic flag so concurrent callers either perform
///          WSAStartup exactly once or wait until the winning caller finishes.
///          The successful startup reference intentionally lasts for the
///          process lifetime. Zanna's native Windows executable can enter
///          through a CRT-less startup shim, where registering a CRT atexit
///          callback corrupts or blocks the uninitialized CRT exit table.
///          Windows releases WinSock process state during process teardown.
void rt_net_init_wsa(void) {
    for (;;) {
        LONG state = InterlockedCompareExchange(&g_wsa_init_state, 0, 0);
        if (state == 2)
            return;
        if (state == 1) {
            if (!SwitchToThread())
                Sleep(0);
            continue;
        }
        if (InterlockedCompareExchange(&g_wsa_init_state, 1, 0) != 0)
            continue;

        WSADATA wsa_data;
        memset(&wsa_data, 0, sizeof(wsa_data));
        int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (result != 0) {
            InterlockedExchange(&g_wsa_init_state, 0);
            rt_trap("Network: WSAStartup failed");
            return;
        }
        if (wsa_data.wVersion != MAKEWORD(2, 2)) {
            WSACleanup();
            InterlockedExchange(&g_wsa_init_state, 0);
            rt_trap("Network: WinSock 2.2 is unavailable");
            return;
        }
        InterlockedExchange(&g_wsa_init_state, 2);
        return;
    }
}

/// @brief Close a WinSock socket handle.
/// @param sock Socket handle returned by socket().
/// @return Native closesocket() result.
int rt_socket_close(socket_t sock) {
    return closesocket(sock);
}

/// @brief Interrupt reads and writes while leaving WinSock handle ownership intact.
/// @param sock Connected WinSock socket handle.
/// @return Result from `shutdown(SD_BOTH)`, or `SOCKET_ERROR` for an invalid handle.
int rt_socket_shutdown_both(socket_t sock) {
    if (sock == INVALID_SOCK) {
        rt_socket_set_last_error(WSAENOTSOCK);
        return SOCKET_ERROR;
    }
    return shutdown(sock, SD_BOTH);
}

/// @brief Return the calling thread's last WinSock error.
/// @return WSAGetLastError() value.
int rt_socket_last_error(void) {
    return WSAGetLastError();
}

/// @brief Classify an interrupted WinSock operation.
/// @param err Native WinSock error code to test.
/// @return true when @p err is WSAEINTR.
bool rt_socket_error_is_interrupted(int err) {
    return err == WSAEINTR;
}

/// @brief Classify non-blocking WinSock retry conditions.
/// @param err Native WinSock error code to test.
/// @return true when @p err is WSAEWOULDBLOCK.
bool rt_socket_error_is_would_block(int err) {
    return err == WSAEWOULDBLOCK;
}

/// @brief Classify an in-progress non-blocking WinSock connect().
/// @param err Native WinSock error code to test.
/// @return true for WinSock's would-block/already/in-progress connect states.
bool rt_socket_error_is_in_progress(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
}

/// @brief Classify a WinSock socket timeout error.
/// @param err Native WinSock error code to test.
/// @return true when @p err is WSAETIMEDOUT.
bool rt_socket_error_is_timeout(int err) {
    return err == WSAETIMEDOUT || err == ETIMEDOUT;
}

/// @brief Classify a WinSock oversized datagram/message error.
/// @param err Native WinSock error code to test.
/// @return true when @p err is WSAEMSGSIZE.
bool rt_socket_error_is_message_too_large(int err) {
    return err == WSAEMSGSIZE;
}

/// @brief Classify the last WinSock receive error as timeout or would-block.
/// @return true when the last WinSock error is WSAETIMEDOUT or WSAEWOULDBLOCK.
bool rt_socket_recv_timed_out(void) {
    int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
}

/// @brief Read the native Windows monotonic tick counter without trapping.
/// @details GetTickCount64 is process-independent, wrap-safe for practical
///          runtime lifetimes, allocation-free, and unaffected by wall-clock
///          adjustment, making it suitable for socket operation deadlines.
/// @return Milliseconds elapsed since system startup.
uint64_t rt_socket_monotonic_ms(void) {
    return (uint64_t)GetTickCount64();
}

/// @brief Detect accept() failures caused by listener shutdown races.
/// @param err Native WinSock error code returned after accept() failed.
/// @return true for expected close/interruption errors during shutdown.
bool rt_socket_accept_interrupted_by_close(int err) {
    return err == WSAENOTSOCK || err == WSAEINVAL || err == WSAEINTR || err == WSAECONNABORTED ||
           err == WSAESHUTDOWN;
}

/// @brief No-op SIGPIPE suppression on Windows.
/// @param sock Socket handle accepted for API symmetry.
void suppress_sigpipe(socket_t sock) {
    (void)sock;
}

/// @brief Toggle non-blocking mode on a WinSock socket.
/// @param sock Socket handle to update.
/// @param nonblocking true enables non-blocking mode, false restores blocking mode.
/// @return true when ioctlsocket(FIONBIO) succeeds.
bool rt_socket_set_nonblocking(socket_t sock, bool nonblocking) {
    u_long mode = nonblocking ? 1u : 0u;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
}

/// @brief Query queued read bytes for a WinSock socket.
/// @param sock Socket handle to inspect.
/// @param bytes_out Receives the queued byte count on success.
/// @return true when ioctlsocket(FIONREAD) succeeds.
bool rt_socket_available_bytes(socket_t sock, int64_t *bytes_out) {
    if (!bytes_out)
        return false;
    *bytes_out = 0;
    u_long bytes_available = 0;
    if (ioctlsocket(sock, FIONREAD, &bytes_available) != 0)
        return false;
    *bytes_out = (int64_t)bytes_available;
    return true;
}

/// @brief Query `SO_ERROR` using WinSock's integer option-length ABI.
/// @param sock Socket handle to inspect.
/// @param error_out Receives zero or the pending WinSock error.
/// @return true when getsockopt succeeds, false otherwise.
bool rt_socket_pending_error(socket_t sock, int *error_out) {
    if (!error_out)
        return false;
    *error_out = 0;
    int pending_error = 0;
    int error_len = (int)sizeof(pending_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&pending_error, &error_len) != 0 ||
        error_len != (int)sizeof(pending_error))
        return false;
    *error_out = pending_error;
    return true;
}

/// @brief Apply SO_RCVTIMEO or SO_SNDTIMEO to a WinSock socket.
/// @param sock Socket handle to configure.
/// @param timeout_ms Timeout in milliseconds; negative values are treated as zero.
/// @param is_recv true selects SO_RCVTIMEO, false selects SO_SNDTIMEO.
/// @return True when setsockopt succeeds; otherwise false.
bool set_socket_timeout(socket_t sock, int timeout_ms, bool is_recv) {
    if (timeout_ms < 0)
        timeout_ms = 0;
    DWORD tv = (DWORD)timeout_ms;
    return setsockopt(sock,
                      SOL_SOCKET,
                      is_recv ? SO_RCVTIMEO : SO_SNDTIMEO,
                      (const char *)&tv,
                      sizeof(tv)) == 0;
}

typedef void(WSAAPI *rt_wsa_set_last_error_fn)(int);

/// @brief Set WinSock's thread-local error without expanding native imports.
/// @details Resolves WSASetLastError dynamically so CRT-less entry paths retain
///          the runtime's constrained native import surface; SetLastError is a
///          defensive fallback when the symbol is unavailable.
/// @param error Native WinSock error code to publish for the current thread.
static void rt_socket_set_last_error(int error) {
    HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    rt_wsa_set_last_error_fn set_error =
        winsock ? (rt_wsa_set_last_error_fn)GetProcAddress(winsock, "WSASetLastError") : NULL;
    if (set_error)
        set_error(error);
    else
        SetLastError((DWORD)error);
}

/// @brief Wait for a WinSock socket to become readable or writable.
/// @details Rebuilds fd_set and timeval for every select() attempt because
///          WinSock may modify both inputs. WSAEINTR is retried against one
///          GetTickCount64 deadline rather than receiving a fresh timeout, so
///          repeated interruption cannot make a finite wait unbounded.
/// @param sock Socket handle to wait on.
/// @param timeout_ms Timeout in milliseconds.
/// @param for_write true waits for write readiness; false waits for read readiness.
/// @return Positive when ready, zero on timeout, negative on error.
int wait_socket(socket_t sock, int timeout_ms, bool for_write) {
    if (timeout_ms < 0) {
        rt_socket_set_last_error(WSAEINVAL);
        return -1;
    }
    if (sock == INVALID_SOCK) {
        rt_socket_set_last_error(WSAENOTSOCK);
        return -1;
    }

    const uint64_t start_ms = rt_socket_monotonic_ms();
    int remaining_ms = timeout_ms;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        struct timeval tv;
        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int result =
            for_write ? select(0, NULL, &fds, NULL, &tv) : select(0, &fds, NULL, NULL, &tv);
        if (result >= 0)
            return result;

        int select_error = rt_socket_last_error();
        if (!rt_socket_error_is_interrupted(select_error))
            return -1;
        if (timeout_ms == 0)
            return 0;

        uint64_t elapsed_ms = rt_socket_monotonic_ms() - start_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms)
            return 0;
        remaining_ms = timeout_ms - (int)elapsed_ms;
    }
}
