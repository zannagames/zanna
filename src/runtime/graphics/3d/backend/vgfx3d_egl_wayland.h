//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_egl_wayland.h
// Purpose: Expose header-free dynamic EGL bindings for Wayland and headless pbuffer surfaces.
// Key invariants: No EGL or wayland-egl development headers or link dependency are required.
// Ownership/Lifetime: A binding owns its EGL display/context/surface and optional wl_egl_window.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_egl_wayland.h
 * @brief Declares the header-free dynamic EGL adapter for Wayland and headless surfaces.
 *
 * Consumers pass opaque Wayland display and surface pointers, avoiding compile-time EGL and
 * wayland-egl headers or link dependencies. A successful binding owns its EGL display, context,
 * window surface, and wl_egl_window until released with vgfx3d_egl_wayland_destroy().
 */

#pragma once

#include <stdint.h>

/// Opaque owner of one EGL/OpenGL context and its optional native Wayland window resources.
typedef struct vgfx3d_egl_wayland vgfx3d_egl_wayland_t;

/// @brief Probe whether the core EGL library and pbuffer entry points are available.
/// @return Non-zero when headless bindings can be attempted, otherwise zero; cached process-wide.
int vgfx3d_egl_available(void);
/// @brief Probe whether all required EGL and wayland-egl libraries and symbols are available.
/// @return Non-zero when bindings can be created, otherwise zero; the result is cached.
int vgfx3d_egl_wayland_available(void);
/// @brief Resolve an OpenGL or EGL extension symbol through the loaded adapter.
/// @param name Null-terminated symbol name.
/// @return Resolved function pointer, or null for an invalid name, unavailable adapter, or missing
///         symbol.
void *vgfx3d_egl_wayland_get_proc(const char *name);
/// @brief Create and make current an EGL OpenGL context for a native Wayland surface.
/// @param display Borrowed native @c wl_display pointer.
/// @param surface Borrowed native @c wl_surface pointer.
/// @param width Positive initial window width in pixels.
/// @param height Positive initial window height in pixels.
/// @return Newly allocated binding owned by the caller, or null on validation, loading, allocation,
///         or EGL initialization failure.
vgfx3d_egl_wayland_t *vgfx3d_egl_wayland_create(void *display,
                                                void *surface,
                                                int32_t width,
                                                int32_t height);
/// @brief Create and make current a surfaceless EGL OpenGL context backed by a pbuffer.
/// @details Prefers the Mesa/KHR surfaceless display route and falls back to EGL's default
///   display. No native display server or hidden window is required.
/// @param width Positive pbuffer width in pixels.
/// @param height Positive pbuffer height in pixels.
/// @return Newly allocated binding owned by the caller, or null on initialization failure.
vgfx3d_egl_wayland_t *vgfx3d_egl_headless_create(int32_t width, int32_t height);
/// @brief Make a binding's context current for both drawing and reading.
/// @param binding Borrowed initialized binding.
/// @return Non-zero on success, otherwise zero for an incomplete binding or EGL failure.
int vgfx3d_egl_wayland_make_current(vgfx3d_egl_wayland_t *binding);
/// @brief Present the binding's EGL window-surface back buffer.
/// @param binding Borrowed initialized binding.
/// @return Non-zero when presentation succeeds, otherwise zero.
int vgfx3d_egl_wayland_swap(vgfx3d_egl_wayland_t *binding);
/// @brief Request a swap interval while preserving Zanna-owned Wayland dispatch.
/// @details The implementation intentionally forces interval zero and ignores @p interval so EGL
///          cannot block while internally dispatching the caller's Wayland display queue.
/// @param binding Borrowed initialized binding.
/// @param interval Requested interval retained for API compatibility.
/// @return Non-zero when EGL accepts interval zero, otherwise zero.
int vgfx3d_egl_wayland_set_swap_interval(vgfx3d_egl_wayland_t *binding, int32_t interval);
/// @brief Resize the binding's native wl_egl_window.
/// @param binding Borrowed binding whose window is resized.
/// @param width Positive new width in pixels.
/// @param height Positive new height in pixels.
void vgfx3d_egl_wayland_resize(vgfx3d_egl_wayland_t *binding, int32_t width, int32_t height);
/// @brief Destroy a binding and every EGL and wayland-egl resource it owns.
/// @param binding Owned binding to release; null is a no-op.
void vgfx3d_egl_wayland_destroy(vgfx3d_egl_wayland_t *binding);
