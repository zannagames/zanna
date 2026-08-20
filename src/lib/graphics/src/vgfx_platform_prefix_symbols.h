//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_prefix_symbols.h
// Purpose: List the complete Linux graphics adapter ABI for compile-time namespacing.
// Key invariants: The list matches all global symbols emitted by both Linux adapters.
// Ownership/Lifetime: Included only after defining VGFX_PREFIXED for the full translation unit.
// Links: src/lib/graphics/src/vgfx_platform_linux_auto.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Rename the complete Linux graphics adapter ABI through `VGFX_PREFIXED`.
/// @details This intentionally repeatable mapping header is included by an
///          adapter-specific prefix header before compiling that adapter.
///          Every externally visible clipboard, native-handle, window, input,
///          timing, allocation, and presentation entry point is rewritten to a
///          backend-qualified symbol so Wayland and X11 can coexist in AUTO
///          builds.  The including header must define `VGFX_PREFIXED(name)`.

/// @name Linux adapter ABI symbol mappings
/// @brief Map unqualified adapter exports to backend-qualified link symbols.
/// @{
#define vgfx_clipboard_clear VGFX_PREFIXED(vgfx_clipboard_clear)
#define vgfx_clipboard_get_text VGFX_PREFIXED(vgfx_clipboard_get_text)
#define vgfx_clipboard_has_format VGFX_PREFIXED(vgfx_clipboard_has_format)
#define vgfx_clipboard_set_text VGFX_PREFIXED(vgfx_clipboard_set_text)
#define vgfx_get_native_display VGFX_PREFIXED(vgfx_get_native_display)
#define vgfx_get_native_handles VGFX_PREFIXED(vgfx_get_native_handles)
#define vgfx_get_window_capabilities VGFX_PREFIXED(vgfx_get_window_capabilities)
#define vgfx_get_native_view VGFX_PREFIXED(vgfx_get_native_view)
#define vgfx_platform_aligned_alloc VGFX_PREFIXED(vgfx_platform_aligned_alloc)
#define vgfx_platform_aligned_free VGFX_PREFIXED(vgfx_platform_aligned_free)
#define vgfx_platform_destroy_window VGFX_PREFIXED(vgfx_platform_destroy_window)
#define vgfx_platform_focus VGFX_PREFIXED(vgfx_platform_focus)
#define vgfx_platform_get_display_logical_size VGFX_PREFIXED(vgfx_platform_get_display_logical_size)
#define vgfx_platform_get_display_scale VGFX_PREFIXED(vgfx_platform_get_display_scale)
#define vgfx_platform_get_monitor_size VGFX_PREFIXED(vgfx_platform_get_monitor_size)
#define vgfx_platform_get_position VGFX_PREFIXED(vgfx_platform_get_position)
#define vgfx_platform_hide_cursor VGFX_PREFIXED(vgfx_platform_hide_cursor)
#define vgfx_platform_init_window VGFX_PREFIXED(vgfx_platform_init_window)
#define vgfx_platform_is_focused VGFX_PREFIXED(vgfx_platform_is_focused)
#define vgfx_platform_is_fullscreen VGFX_PREFIXED(vgfx_platform_is_fullscreen)
#define vgfx_platform_is_maximized VGFX_PREFIXED(vgfx_platform_is_maximized)
#define vgfx_platform_is_minimized VGFX_PREFIXED(vgfx_platform_is_minimized)
#define vgfx_platform_maximize VGFX_PREFIXED(vgfx_platform_maximize)
#define vgfx_platform_minimize VGFX_PREFIXED(vgfx_platform_minimize)
#define vgfx_platform_now_ms VGFX_PREFIXED(vgfx_platform_now_ms)
#define vgfx_platform_present VGFX_PREFIXED(vgfx_platform_present)
#define vgfx_platform_process_events VGFX_PREFIXED(vgfx_platform_process_events)
#define vgfx_platform_request_foreground VGFX_PREFIXED(vgfx_platform_request_foreground)
#define vgfx_platform_restore VGFX_PREFIXED(vgfx_platform_restore)
#define vgfx_platform_set_cursor VGFX_PREFIXED(vgfx_platform_set_cursor)
#define vgfx_platform_set_cursor_visible VGFX_PREFIXED(vgfx_platform_set_cursor_visible)
#define vgfx_platform_set_fullscreen VGFX_PREFIXED(vgfx_platform_set_fullscreen)
#define vgfx_platform_set_position VGFX_PREFIXED(vgfx_platform_set_position)
#define vgfx_platform_set_prevent_close VGFX_PREFIXED(vgfx_platform_set_prevent_close)
#define vgfx_platform_set_relative_mouse VGFX_PREFIXED(vgfx_platform_set_relative_mouse)
#define vgfx_platform_set_text_input_enabled VGFX_PREFIXED(vgfx_platform_set_text_input_enabled)
#define vgfx_platform_set_text_input_state VGFX_PREFIXED(vgfx_platform_set_text_input_state)
#define vgfx_platform_set_title VGFX_PREFIXED(vgfx_platform_set_title)
#define vgfx_platform_set_window_size VGFX_PREFIXED(vgfx_platform_set_window_size)
#define vgfx_platform_set_window_min_size VGFX_PREFIXED(vgfx_platform_set_window_min_size)
#define vgfx_platform_show_cursor VGFX_PREFIXED(vgfx_platform_show_cursor)
#define vgfx_platform_sleep_ms VGFX_PREFIXED(vgfx_platform_sleep_ms)
#define vgfx_platform_wait_events VGFX_PREFIXED(vgfx_platform_wait_events)
#define vgfx_platform_wake_events VGFX_PREFIXED(vgfx_platform_wake_events)
#define vgfx_platform_warp_cursor VGFX_PREFIXED(vgfx_platform_warp_cursor)
#define vgfx_platform_yield VGFX_PREFIXED(vgfx_platform_yield)
/// @}
