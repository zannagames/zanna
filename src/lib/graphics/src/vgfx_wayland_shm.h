//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_shm.h
// Purpose: Release-tracked wl_shm presentation buffers for the native Wayland backend.
// Key invariants:
//   - A busy buffer is never overwritten before wl_buffer.release.
//   - All width, height, stride, offset, and pool-size arithmetic is checked.
// Ownership/Lifetime:
//   - The presenter owns its mapped storage, pool proxy, and two buffer proxies.
//   - The target wl_surface and connection are borrowed from the shell.
// Links: src/lib/graphics/src/vgfx_wayland_shell.h
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_shell.h"

#include <stddef.h>
#include <stdint.h>

/// @file
/// @brief Declares double-buffered Wayland shared-memory presentation.
/// @details The presenter converts opaque RGBA input into the compositor's XRGB8888 byte
/// layout, waits for `wl_buffer.release` before reusing slots, and coalesces queued frames
/// behind a single `wl_surface.frame` callback.

/// @brief One compositor-visible buffer within a shared-memory presenter.
typedef struct vgfx_wayland_shm_slot {
    /// @brief Owned `wl_buffer` proxy backed by this slot's mapped bytes.
    struct wl_proxy *buffer;

    /// @brief Pointer to this slot's first byte within the presenter's mapping.
    uint8_t *pixels;

    /// @brief Nonzero between surface attachment and `wl_buffer.release`.
    int32_t busy;

    /// @brief Borrowed back-pointer used by release callbacks.
    struct vgfx_wayland_shm_presenter *presenter;
} vgfx_wayland_shm_slot_t;

/// @brief Double-buffered shared-memory presentation state for one surface.
/// @details The presenter owns its mapping, pool, buffer proxies, and outstanding frame
/// callback. @ref shell and @ref surface are borrowed and must outlive the presenter.
typedef struct vgfx_wayland_shm_presenter {
    /// @brief Borrowed shell that owns the connection used by this presenter.
    vgfx_wayland_shell_t *shell;

    /// @brief Borrowed target surface receiving buffer commits.
    struct wl_surface *surface;

    /// @brief Owned `wl_shm_pool` proxy spanning both slots.
    struct wl_proxy *pool;

    /// @brief Writable mapping shared with the compositor.
    void *mapping;

    /// @brief Total mapped byte count for both slots.
    size_t mapping_size;

    /// @brief Byte count occupied by one image.
    size_t buffer_size;

    /// @brief Image width in pixels.
    int32_t width;

    /// @brief Image height in pixels.
    int32_t height;

    /// @brief Number of bytes between consecutive image rows.
    int32_t stride;

    /// @brief Count of frames successfully committed to the surface.
    uint64_t generation;

    /// @brief Owned outstanding `wl_surface.frame` callback, or NULL.
    struct wl_proxy *frame_callback;

    /// @brief Borrowed source pixels for the most recently queued frame.
    const uint8_t *queued_rgba;

    /// @brief Available byte count at @ref queued_rgba.
    size_t queued_rgba_size;

    /// @brief Nonzero when a source frame is waiting to be copied and committed.
    int32_t queued;

    /// @brief The two independently release-tracked presentation buffers.
    vgfx_wayland_shm_slot_t slots[2];
} vgfx_wayland_shm_presenter_t;

/// @brief Allocate two compositor-visible buffers for an xdg shell surface.
/// @details Equivalent to @ref vgfx_wayland_shm_open_surface using `shell->surface`.
/// @param presenter Destination presenter state.
/// @param shell Borrowed initialized shell and target surface owner.
/// @param width Positive image width in pixels.
/// @param height Positive image height in pixels.
/// @param error Optional destination for a stable diagnostic.
/// @param error_size Capacity of @p error in bytes, including its terminator.
/// @return 1 when the mapping, pool, buffers, and listeners are ready; otherwise 0.
int vgfx_wayland_shm_open(vgfx_wayland_shm_presenter_t *presenter,
                          vgfx_wayland_shell_t *shell,
                          int32_t width,
                          int32_t height,
                          char *error,
                          uint32_t error_size);

/// @brief Allocate two compositor-visible buffers for an arbitrary surface on the shell display.
/// @details This is used by synchronized client-decoration subsurfaces; the shell remains the
///          owner of the connection while @p surface is borrowed until presenter close.
/// @param presenter Destination presenter state.
/// @param shell Borrowed shell providing the open connection.
/// @param surface Borrowed target surface on the shell's display.
/// @param width Positive image width in pixels.
/// @param height Positive image height in pixels.
/// @param error Optional destination for a stable diagnostic.
/// @param error_size Capacity of @p error in bytes, including its terminator.
/// @return 1 when the mapping, pool, buffers, and listeners are ready; otherwise 0.
int vgfx_wayland_shm_open_surface(vgfx_wayland_shm_presenter_t *presenter,
                                  vgfx_wayland_shell_t *shell,
                                  struct wl_surface *surface,
                                  int32_t width,
                                  int32_t height,
                                  char *error,
                                  uint32_t error_size);

/// @brief Queue opaque RGBA pixels and commit at compositor frame cadence.
/// @details The source pixels remain borrowed until the next frame callback or presenter close.
/// A newer accepted call replaces any queued source that has not yet been copied.
/// @param presenter Open shared-memory presenter.
/// @param rgba Opaque RGBA8 source image in tightly packed row-major order.
/// @param rgba_size Available bytes at @p rgba.
/// @return 1 when accepted (including a coalesced update), 0 for invalid arguments.
int vgfx_wayland_shm_present(vgfx_wayland_shm_presenter_t *presenter,
                             const uint8_t *rgba,
                             size_t rgba_size);

/// @brief Destroy buffers/pool, unmap storage, and zero the presenter.
/// @details Passing NULL is harmless. The function does not wait for compositor releases
/// because destroying the local proxies ends their use before unmapping the pool storage.
/// @param presenter Presenter state to close, or NULL.
void vgfx_wayland_shm_close(vgfx_wayland_shm_presenter_t *presenter);
