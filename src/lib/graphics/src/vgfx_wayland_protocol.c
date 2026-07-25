//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_protocol.c
// Purpose: Define stable xdg-shell metadata without requiring wayland-scanner at build time.
// Key invariants:
//   - Request/event names, signatures, order, and since-versions match stable xdg-shell XML.
//   - Registry binding never requests a version newer than both peers understand.
// Ownership/Lifetime: All metadata is immutable process-lifetime storage.
// Links: src/lib/graphics/src/vgfx_wayland_protocol.h
//
//===----------------------------------------------------------------------===//

#include "vgfx_wayland_protocol.h"

/// @file
/// @brief Defines Wayland wire metadata and the small xdg-shell request helpers.
/// @details The immutable tables below mirror protocol XML message order and signatures.
/// They are passed to the dynamically loaded `wl_proxy_marshal_flags` entry point and have
/// process lifetime.

/* Core protocol interfaces are normally provided by generated scanner code. Zanna loads
 * libwayland dynamically, so stable name/version-only descriptors supply the object metadata
 * required by wl_proxy_marshal_flags without adding a link-time dependency. */
static const struct wl_interface g_wl_surface_interface = {"wl_surface", 6, 0, NULL, 0, NULL};
static const struct wl_interface g_wl_seat_interface = {"wl_seat", 8, 0, NULL, 0, NULL};
static const struct wl_interface g_wl_output_interface = {"wl_output", 4, 0, NULL, 0, NULL};
static const struct wl_interface g_wl_pointer_interface = {"wl_pointer", 8, 0, NULL, 0, NULL};
static const struct wl_interface g_wl_region_interface = {"wl_region", 1, 0, NULL, 0, NULL};

static const struct wl_interface *g_xdg_wm_create_positioner_types[] = {
    &vgfx_xdg_positioner_interface};
static const struct wl_interface *g_xdg_wm_get_surface_types[] = {
    &vgfx_xdg_surface_interface, &g_wl_surface_interface};

static const struct wl_message g_xdg_wm_base_requests[] = {
    {"destroy", "", NULL},
    {"create_positioner", "n", g_xdg_wm_create_positioner_types},
    {"get_xdg_surface", "no", g_xdg_wm_get_surface_types},
    {"pong", "u", NULL},
};
static const struct wl_message g_xdg_wm_base_events[] = {{"ping", "u", NULL}};

static const struct wl_message g_xdg_positioner_requests[] = {
    {"destroy", "", NULL},
    {"set_size", "ii", NULL},
    {"set_anchor_rect", "iiii", NULL},
    {"set_anchor", "u", NULL},
    {"set_gravity", "u", NULL},
    {"set_constraint_adjustment", "u", NULL},
    {"set_offset", "ii", NULL},
    {"set_reactive", "3", NULL},
    {"set_parent_size", "3ii", NULL},
    {"set_parent_configure", "3u", NULL},
};

static const struct wl_interface *g_xdg_surface_get_toplevel_types[] = {
    &vgfx_xdg_toplevel_interface};
static const struct wl_interface *g_xdg_surface_get_popup_types[] = {
    &vgfx_xdg_popup_interface, &vgfx_xdg_surface_interface, &vgfx_xdg_positioner_interface};
static const struct wl_message g_xdg_surface_requests[] = {
    {"destroy", "", NULL},
    {"get_toplevel", "n", g_xdg_surface_get_toplevel_types},
    {"get_popup", "n?oo", g_xdg_surface_get_popup_types},
    {"set_window_geometry", "iiii", NULL},
    {"ack_configure", "u", NULL},
};
static const struct wl_message g_xdg_surface_events[] = {{"configure", "u", NULL}};

static const struct wl_interface *g_xdg_toplevel_parent_types[] = {&vgfx_xdg_toplevel_interface};
static const struct wl_interface *g_xdg_toplevel_seat_types[] = {&g_wl_seat_interface, NULL, NULL,
                                                                  NULL};
static const struct wl_interface *g_xdg_toplevel_move_types[] = {&g_wl_seat_interface, NULL};
static const struct wl_interface *g_xdg_toplevel_resize_types[] = {&g_wl_seat_interface, NULL, NULL};
static const struct wl_interface *g_xdg_toplevel_fullscreen_types[] = {&g_wl_output_interface};
static const struct wl_message g_xdg_toplevel_requests[] = {
    {"destroy", "", NULL},
    {"set_parent", "?o", g_xdg_toplevel_parent_types},
    {"set_title", "s", NULL},
    {"set_app_id", "s", NULL},
    {"show_window_menu", "ouii", g_xdg_toplevel_seat_types},
    {"move", "ou", g_xdg_toplevel_move_types},
    {"resize", "ouu", g_xdg_toplevel_resize_types},
    {"set_max_size", "ii", NULL},
    {"set_min_size", "ii", NULL},
    {"set_maximized", "", NULL},
    {"unset_maximized", "", NULL},
    {"set_fullscreen", "?o", g_xdg_toplevel_fullscreen_types},
    {"unset_fullscreen", "", NULL},
    {"set_minimized", "", NULL},
};
static const struct wl_message g_xdg_toplevel_events[] = {
    {"configure", "iia", NULL},
    {"close", "", NULL},
    {"configure_bounds", "4ii", NULL},
    {"wm_capabilities", "5a", NULL},
};

static const struct wl_interface *g_xdg_popup_grab_types[] = {&g_wl_seat_interface, NULL};
static const struct wl_interface *g_xdg_popup_reposition_types[] = {&vgfx_xdg_positioner_interface,
                                                                    NULL};
static const struct wl_message g_xdg_popup_requests[] = {
    {"destroy", "", NULL},
    {"grab", "ou", g_xdg_popup_grab_types},
    {"reposition", "3ou", g_xdg_popup_reposition_types},
};
static const struct wl_message g_xdg_popup_events[] = {
    {"configure", "iiii", NULL},
    {"popup_done", "", NULL},
    {"repositioned", "3u", NULL},
};

static const struct wl_interface *g_text_input_get_types[] = {&vgfx_zwp_text_input_v3_interface,
                                                              &g_wl_seat_interface};
static const struct wl_message g_text_input_manager_v3_requests[] = {
    {"destroy", "", NULL},
    {"get_text_input", "no", g_text_input_get_types},
};

static const struct wl_message g_text_input_v3_requests[] = {
    {"destroy", "", NULL},
    {"enable", "", NULL},
    {"disable", "", NULL},
    {"set_surrounding_text", "sii", NULL},
    {"set_text_change_cause", "u", NULL},
    {"set_content_type", "uu", NULL},
    {"set_cursor_rectangle", "iiii", NULL},
    {"commit", "", NULL},
};

static const struct wl_interface *g_text_input_surface_types[] = {&g_wl_surface_interface};
static const struct wl_message g_text_input_v3_events[] = {
    {"enter", "o", g_text_input_surface_types},
    {"leave", "o", g_text_input_surface_types},
    {"preedit_string", "?sii", NULL},
    {"commit_string", "?s", NULL},
    {"delete_surrounding_text", "uu", NULL},
    {"done", "u", NULL},
};

static const struct wl_interface *g_viewporter_get_types[] = {&vgfx_wp_viewport_interface,
                                                              &g_wl_surface_interface};
static const struct wl_message g_viewporter_requests[] = {
    {"destroy", "", NULL},
    {"get_viewport", "no", g_viewporter_get_types},
};
static const struct wl_message g_viewport_requests[] = {
    {"destroy", "", NULL},
    {"set_source", "ffff", NULL},
    {"set_destination", "ii", NULL},
};
static const struct wl_interface *g_fractional_get_types[] = {
    &vgfx_wp_fractional_scale_v1_interface, &g_wl_surface_interface};
static const struct wl_message g_fractional_scale_manager_requests[] = {
    {"destroy", "", NULL},
    {"get_fractional_scale", "no", g_fractional_get_types},
};
static const struct wl_message g_fractional_scale_requests[] = {
    {"destroy", "", NULL},
};
static const struct wl_message g_fractional_scale_events[] = {
    {"preferred_scale", "u", NULL},
};
static const struct wl_interface *g_decoration_get_types[] = {
    &vgfx_zxdg_toplevel_decoration_v1_interface, &vgfx_xdg_toplevel_interface};
static const struct wl_message g_decoration_manager_requests[] = {
    {"destroy", "", NULL},
    {"get_toplevel_decoration", "no", g_decoration_get_types},
};
static const struct wl_message g_toplevel_decoration_requests[] = {
    {"destroy", "", NULL},
    {"set_mode", "u", NULL},
    {"unset_mode", "", NULL},
};
static const struct wl_message g_toplevel_decoration_events[] = {
    {"configure", "u", NULL},
};
static const struct wl_interface *g_relative_get_types[] = {&vgfx_zwp_relative_pointer_v1_interface,
                                                            &g_wl_pointer_interface};
static const struct wl_message g_relative_pointer_manager_requests[] = {
    {"destroy", "", NULL},
    {"get_relative_pointer", "no", g_relative_get_types},
};
static const struct wl_message g_relative_pointer_requests[] = {{"destroy", "", NULL}};
static const struct wl_message g_relative_pointer_events[] = {
    {"relative_motion", "uuffff", NULL},
};
static const struct wl_interface *g_pointer_constraint_types[] = {
    &vgfx_zwp_locked_pointer_v1_interface, &g_wl_surface_interface, &g_wl_pointer_interface,
    &g_wl_region_interface, NULL};
static const struct wl_message g_pointer_constraints_requests[] = {
    {"destroy", "", NULL},
    {"lock_pointer", "noo?ou", g_pointer_constraint_types},
    {"confine_pointer", "noo?ou", g_pointer_constraint_types},
};
static const struct wl_interface *g_locked_region_types[] = {&g_wl_region_interface};
static const struct wl_message g_locked_pointer_requests[] = {
    {"destroy", "", NULL},
    {"set_cursor_position_hint", "ff", NULL},
    {"set_region", "?o", g_locked_region_types},
};
static const struct wl_message g_locked_pointer_events[] = {
    {"locked", "", NULL},
    {"unlocked", "", NULL},
};
static const struct wl_interface *g_activation_get_types[] = {
    &vgfx_xdg_activation_token_v1_interface};
static const struct wl_interface *g_activation_activate_types[] = {NULL, &g_wl_surface_interface};
static const struct wl_message g_activation_requests[] = {
    {"destroy", "", NULL},
    {"get_activation_token", "n", g_activation_get_types},
    {"activate", "so", g_activation_activate_types},
};
static const struct wl_interface *g_activation_serial_types[] = {NULL, &g_wl_seat_interface};
static const struct wl_interface *g_activation_surface_types[] = {&g_wl_surface_interface};
static const struct wl_message g_activation_token_requests[] = {
    {"set_serial", "uo", g_activation_serial_types},
    {"set_app_id", "s", NULL},
    {"set_surface", "o", g_activation_surface_types},
    {"commit", "", NULL},
    {"destroy", "", NULL},
};
static const struct wl_message g_activation_token_events[] = {{"done", "s", NULL}};

const struct wl_interface vgfx_xdg_wm_base_interface = {
    "xdg_wm_base", 6, 4, g_xdg_wm_base_requests, 1, g_xdg_wm_base_events};
const struct wl_interface vgfx_xdg_positioner_interface = {
    "xdg_positioner", 6, 10, g_xdg_positioner_requests, 0, NULL};
const struct wl_interface vgfx_xdg_surface_interface = {
    "xdg_surface", 6, 5, g_xdg_surface_requests, 1, g_xdg_surface_events};
const struct wl_interface vgfx_xdg_toplevel_interface = {
    "xdg_toplevel", 6, 14, g_xdg_toplevel_requests, 4, g_xdg_toplevel_events};
const struct wl_interface vgfx_xdg_popup_interface = {
    "xdg_popup", 6, 3, g_xdg_popup_requests, 3, g_xdg_popup_events};
const struct wl_interface vgfx_zwp_text_input_manager_v3_interface = {
    "zwp_text_input_manager_v3", 1, 2, g_text_input_manager_v3_requests, 0, NULL};
const struct wl_interface vgfx_zwp_text_input_v3_interface = {
    "zwp_text_input_v3", 1, 8, g_text_input_v3_requests, 6, g_text_input_v3_events};
const struct wl_interface vgfx_wp_viewporter_interface = {
    "wp_viewporter", 1, 2, g_viewporter_requests, 0, NULL};
const struct wl_interface vgfx_wp_viewport_interface = {
    "wp_viewport", 1, 3, g_viewport_requests, 0, NULL};
const struct wl_interface vgfx_wp_fractional_scale_manager_v1_interface = {
    "wp_fractional_scale_manager_v1", 1, 2, g_fractional_scale_manager_requests, 0, NULL};
const struct wl_interface vgfx_wp_fractional_scale_v1_interface = {
    "wp_fractional_scale_v1", 1, 1, g_fractional_scale_requests, 1, g_fractional_scale_events};
const struct wl_interface vgfx_zxdg_decoration_manager_v1_interface = {
    "zxdg_decoration_manager_v1", 1, 2, g_decoration_manager_requests, 0, NULL};
const struct wl_interface vgfx_zxdg_toplevel_decoration_v1_interface = {
    "zxdg_toplevel_decoration_v1", 1, 3, g_toplevel_decoration_requests, 1,
    g_toplevel_decoration_events};
const struct wl_interface vgfx_zwp_relative_pointer_manager_v1_interface = {
    "zwp_relative_pointer_manager_v1", 1, 2, g_relative_pointer_manager_requests, 0, NULL};
const struct wl_interface vgfx_zwp_relative_pointer_v1_interface = {
    "zwp_relative_pointer_v1", 1, 1, g_relative_pointer_requests, 1, g_relative_pointer_events};
const struct wl_interface vgfx_zwp_pointer_constraints_v1_interface = {
    "zwp_pointer_constraints_v1", 1, 3, g_pointer_constraints_requests, 0, NULL};
const struct wl_interface vgfx_zwp_locked_pointer_v1_interface = {
    "zwp_locked_pointer_v1", 1, 3, g_locked_pointer_requests, 2, g_locked_pointer_events};
const struct wl_interface vgfx_xdg_activation_v1_interface = {
    "xdg_activation_v1", 1, 3, g_activation_requests, 0, NULL};
const struct wl_interface vgfx_xdg_activation_token_v1_interface = {
    "xdg_activation_token_v1", 1, 5, g_activation_token_requests, 1,
    g_activation_token_events};

/// @copydoc vgfx_wayland_registry_bind
struct wl_proxy *vgfx_wayland_registry_bind(const vgfx_wayland_client_api_t *api,
                                             struct wl_registry *registry,
                                             uint32_t name,
                                             const struct wl_interface *interface,
                                             uint32_t advertised_version,
                                             uint32_t tested_version) {
    if (!api || !api->proxy_marshal_flags || !registry || !interface || advertised_version == 0 ||
        tested_version == 0)
        return NULL;
    uint32_t version = advertised_version < tested_version ? advertised_version : tested_version;
    return api->proxy_marshal_flags((struct wl_proxy *)registry,
                                    VGFX_WL_REGISTRY_BIND,
                                    interface,
                                    version,
                                    0,
                                    name,
                                    interface->name,
                                    version,
                                    NULL);
}

/// @copydoc vgfx_xdg_wm_base_add_listener
int vgfx_xdg_wm_base_add_listener(const vgfx_wayland_client_api_t *api,
                                  struct xdg_wm_base *wm_base,
                                  const vgfx_xdg_wm_base_listener_t *listener,
                                  void *data) {
    if (!api || !api->proxy_add_listener || !wm_base || !listener)
        return -1;
    return api->proxy_add_listener((struct wl_proxy *)wm_base,
                                   (void (**)(void))(void *)listener,
                                   data);
}

/// @copydoc vgfx_xdg_wm_base_pong
void vgfx_xdg_wm_base_pong(const vgfx_wayland_client_api_t *api,
                           struct xdg_wm_base *wm_base,
                           uint32_t serial) {
    if (!api || !api->proxy_marshal_flags || !api->proxy_get_version || !wm_base)
        return;
    (void)api->proxy_marshal_flags((struct wl_proxy *)wm_base,
                                   VGFX_XDG_WM_BASE_PONG,
                                   NULL,
                                   api->proxy_get_version((struct wl_proxy *)wm_base),
                                   0,
                                   serial);
}
