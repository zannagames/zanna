//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_minimap.c
// Purpose: Minimap3D (plan 28) — authored-map minimap with entity/point
//   markers and rim clamping, a compass strip, and off-screen objective
//   indicators, all drawn through the existing Canvas3D overlay primitives.
// Key invariants:
//   - The map is a north-up authored image with a world-rect affine; all
//     marker math is pure arithmetic over that affine (deterministic).
//   - Stale marker entities drop their markers fail-closed at draw time.
//   - Image handles are validated as Pixels and every value later converted to
//     an integer canvas coordinate is bounded before storage.
// Ownership/Lifetime:
//   - GC handle; retains the world, map image, tracked entity, and marker
//     entities/icons; finalizer releases everything.
// Links: misc/plans/thirdpersonupgrade/28-minimap-markers.md, ADR 0098
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements authored-map minimaps, compass markers, and objective indicators.
/// @details Minimap3D maps world XZ coordinates into a configurable HUD viewport,
/// tracks retained entity or fixed-point markers, discards stale entity markers,
/// and renders optional compass and screen-edge objective projections through
/// Canvas3D overlay primitives.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINIMAP3D_MAX_MARKERS 64
#define MINIMAP3D_PI 3.14159265358979323846
#define MINIMAP3D_SCREEN_ABS_MAX RT_GAME3D_COORD_ABS_MAX

/// @brief One fixed-capacity minimap marker and its retained rendering references.
typedef struct rt_game3d_minimap_marker {
    int8_t used;
    void *entity;    /* retained Entity3D, or NULL for point markers */
    double point[3]; /* world point when entity == NULL */
    void *icon;      /* retained Pixels icon, or NULL for a color chip */
    int64_t color;   /* packed 0xRRGGBB chip color */
    double scale;    /* icon/chip scale (default 1) */
    int8_t edge_clamp;
    int8_t on_compass;
    int8_t objective;
} rt_game3d_minimap_marker;

/// @brief Minimap3D payload: map source, viewport, markers, compass state.
typedef struct rt_game3d_minimap {
    void *world;     /* retained World3D (the minimap is game-owned; no cycle) */
    void *map_image; /* retained Pixels authored map, or NULL */
    double world_min_x, world_min_z, world_max_x, world_max_z;
    void *tracked; /* retained Entity3D centered/arrowed, or NULL */
    int64_t size_px;
    double view_x, view_y, view_w, view_h;
    int8_t compass_enabled;
    double compass_width;
    rt_game3d_minimap_marker markers[MINIMAP3D_MAX_MARKERS];
    int64_t next_marker_id; /* monotonically increasing handle base */
    int64_t marker_ids[MINIMAP3D_MAX_MARKERS];
} rt_game3d_minimap;

/// @brief Validate an opaque handle as a Minimap3D payload.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message recorded when validation fails.
/// @return Typed minimap payload, or `NULL` after recording a trap.
static rt_game3d_minimap *game3d_minimap_checked(void *obj, const char *method) {
    rt_game3d_minimap *map =
        (rt_game3d_minimap *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_MINIMAP_CLASS_ID);
    if (!map)
        rt_trap(method);
    return map;
}

/// @brief Validate an optional Pixels handle used by a map or marker.
/// @param pixels Candidate handle; NULL is accepted.
/// @param method Trap message recorded on a wrong runtime class.
/// @return Nonzero for NULL or a live Pixels handle.
static int game3d_minimap_pixels_valid(void *pixels, const char *method) {
    if (!pixels)
        return 1;
    if (rt_obj_class_id(pixels) == RT_PIXELS_CLASS_ID)
        return 1;
    rt_trap(method);
    return 0;
}

/// @brief Return the tracked entity only while its retained handle remains live.
/// @details Destroyed tracked entities are released in place before any map,
///          compass, or objective coordinate reads.
/// @param map Minimap whose optional tracked entity is inspected.
/// @return Borrowed live Entity3D, or `NULL` after clearing stale state.
static rt_game3d_entity *game3d_minimap_tracked_ref(rt_game3d_minimap *map) {
    if (!map || !map->tracked)
        return NULL;
    rt_game3d_entity *tracked =
        (rt_game3d_entity *)rt_g3d_checked_or_null(map->tracked, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    if (game3d_entity_alive_or_record(tracked))
        return tracked;
    game3d_release_typed_ref(&map->tracked, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return NULL;
}

/// @brief Convert a world canvas extent to a bounded screen-space double.
/// @param extent Integer canvas width or height.
/// @param fallback Positive fallback used for missing/nonpositive extents.
/// @return Positive value bounded for later integer coordinate conversion.
static double game3d_minimap_screen_extent(int64_t extent, double fallback) {
    if (extent <= 0)
        return fallback;
    if (extent > (int64_t)MINIMAP3D_SCREEN_ABS_MAX)
        return MINIMAP3D_SCREEN_ABS_MAX;
    return (double)extent;
}

/// @brief GC finalizer: release the world, map image, and marker refs.
/// @param obj Finalized Minimap3D payload; `NULL` is ignored.
static void game3d_minimap_finalize(void *obj) {
    rt_game3d_minimap *map = (rt_game3d_minimap *)obj;
    if (!map)
        return;
    game3d_release_typed_ref(&map->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    game3d_release_ref(&map->map_image);
    game3d_release_typed_ref(&map->tracked, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
        game3d_release_typed_ref(&map->markers[i].entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        game3d_release_ref(&map->markers[i].icon);
        map->markers[i].used = 0;
    }
}

/// @brief Create a minimap bound to @p world with a square default viewport.
/// @param world_obj Borrowed live World3D handle retained by the minimap.
/// @param size_px Initial square viewport size in pixels, from 32 through 2048.
/// @return New GC-managed Minimap3D handle, or `NULL` after validation or allocation failure.
void *rt_game3d_minimap_new(void *world_obj, int64_t size_px) {
    rt_game3d_world *world = game3d_world_checked(world_obj, "Game3D.Minimap3D.New: invalid world");
    if (!world)
        return NULL;
    if (size_px < 32 || size_px > 2048) {
        rt_trap("Game3D.Minimap3D.New: sizePx must be 32..2048");
        return NULL;
    }
    rt_game3d_minimap *map =
        (rt_game3d_minimap *)rt_obj_new_i64(RT_G3D_GAME3D_MINIMAP_CLASS_ID, (int64_t)sizeof(*map));
    if (!map) {
        rt_trap("Game3D.Minimap3D.New: allocation failed");
        return NULL;
    }
    memset(map, 0, sizeof(*map));
    rt_obj_set_finalizer(map, game3d_minimap_finalize);
    game3d_assign_typed_ref(&map->world, world_obj, RT_G3D_GAME3D_WORLD_CLASS_ID);
    map->size_px = size_px;
    map->view_x = 20.0;
    map->view_y = 20.0;
    map->view_w = (double)size_px;
    map->view_h = (double)size_px;
    map->world_min_x = -100.0;
    map->world_min_z = -100.0;
    map->world_max_x = 100.0;
    map->world_max_z = 100.0;
    map->compass_width = 360.0;
    map->next_marker_id = 1;
    return map;
}

/// @brief Set the authored north-up map image and its world-rect affine.
/// @param obj Borrowed Minimap3D handle.
/// @param pixels Borrowed Pixels handle retained as the map backdrop, or `NULL`.
/// @param min_x World-space X coordinate mapped to the viewport's left edge.
/// @param min_z World-space Z coordinate mapped to the viewport's top edge.
/// @param max_x World-space X coordinate mapped to the viewport's right edge.
/// @param max_z World-space Z coordinate mapped to the viewport's bottom edge.
void rt_game3d_minimap_set_map_image(
    void *obj, void *pixels, double min_x, double min_z, double max_x, double max_z) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetMapImage: invalid minimap");
    if (!map)
        return;
    if (!game3d_minimap_pixels_valid(pixels,
                                     "Game3D.Minimap3D.SetMapImage: image must be Pixels or null"))
        return;
    if (!isfinite(min_x) || !isfinite(min_z) || !isfinite(max_x) || !isfinite(max_z) ||
        fabs(min_x) > RT_GAME3D_COORD_ABS_MAX || fabs(min_z) > RT_GAME3D_COORD_ABS_MAX ||
        fabs(max_x) > RT_GAME3D_COORD_ABS_MAX || fabs(max_z) > RT_GAME3D_COORD_ABS_MAX ||
        max_x - min_x <= 1e-12 || max_z - min_z <= 1e-12) {
        rt_trap("Game3D.Minimap3D.SetMapImage: world rect must be bounded and non-empty");
        return;
    }
    game3d_assign_ref(&map->map_image, pixels);
    map->world_min_x = min_x;
    map->world_min_z = min_z;
    map->world_max_x = max_x;
    map->world_max_z = max_z;
}

/// @brief Track an entity (player): centered arrow + objective reference point.
/// @param obj Borrowed Minimap3D handle.
/// @param entity Borrowed Entity3D handle to retain, or `NULL` to clear tracking.
void rt_game3d_minimap_set_tracked_entity(void *obj, void *entity) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetTrackedEntity: invalid minimap");
    if (!map)
        return;
    if (!entity) {
        game3d_release_typed_ref(&map->tracked, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        return;
    }
    rt_game3d_entity *tracked = game3d_entity_checked(
        entity, "Game3D.Minimap3D.SetTrackedEntity: expected a live Entity3D");
    if (tracked)
        game3d_assign_typed_ref(&map->tracked, tracked, RT_G3D_GAME3D_ENTITY_CLASS_ID);
}

/// @brief Place the minimap viewport on screen.
/// @param obj Borrowed Minimap3D handle.
/// @param x Finite viewport left coordinate in pixels.
/// @param y Finite viewport top coordinate in pixels.
/// @param w Positive finite viewport width in pixels.
/// @param h Positive finite viewport height in pixels.
void rt_game3d_minimap_set_viewport(void *obj, double x, double y, double w, double h) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetViewport: invalid minimap");
    if (!map)
        return;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w <= 0.0 || h <= 0.0 ||
        fabs(x) > MINIMAP3D_SCREEN_ABS_MAX || fabs(y) > MINIMAP3D_SCREEN_ABS_MAX ||
        w > MINIMAP3D_SCREEN_ABS_MAX || h > MINIMAP3D_SCREEN_ABS_MAX) {
        rt_trap("Game3D.Minimap3D.SetViewport: viewport must be finite, positive, and bounded");
        return;
    }
    map->view_x = x;
    map->view_y = y;
    map->view_w = w;
    map->view_h = h;
}

/// @brief Enable the top-center compass strip.
/// @param obj Borrowed Minimap3D handle.
/// @param enabled Nonzero to render the compass strip.
/// @param width_px Requested compass width; finite values of at least 64 pixels replace the current
/// width.
void rt_game3d_minimap_set_compass(void *obj, int8_t enabled, double width_px) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetCompass: invalid minimap");
    if (!map)
        return;
    map->compass_enabled = enabled ? 1 : 0;
    if (isfinite(width_px) && width_px >= 64.0)
        map->compass_width =
            width_px > MINIMAP3D_SCREEN_ABS_MAX ? MINIMAP3D_SCREEN_ABS_MAX : width_px;
}

/// @brief Resolve an active marker by its monotonically assigned identifier.
/// @param map Borrowed minimap payload to search.
/// @param id Marker identifier returned by an add operation.
/// @return Borrowed marker slot, or `NULL` when @p id is not active.
static rt_game3d_minimap_marker *game3d_minimap_marker_by_id(rt_game3d_minimap *map, int64_t id) {
    for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i)
        if (map->markers[i].used && map->marker_ids[i] == id)
            return &map->markers[i];
    return NULL;
}

/// @brief Allocate a positive marker id without signed overflow or active-id reuse.
/// @param map Minimap whose active id set and next-id cursor are inspected.
/// @return A positive id unique among the fixed active marker slots.
static int64_t game3d_minimap_next_marker_id(rt_game3d_minimap *map) {
    int64_t candidate = map->next_marker_id > 0 ? map->next_marker_id : 1;
    for (int32_t attempts = 0; attempts <= MINIMAP3D_MAX_MARKERS; ++attempts) {
        if (!game3d_minimap_marker_by_id(map, candidate)) {
            map->next_marker_id = candidate == INT64_MAX ? 1 : candidate + 1;
            return candidate;
        }
        candidate = candidate == INT64_MAX ? 1 : candidate + 1;
    }
    return 0;
}

/// @brief Initialize the first free marker slot and retain its optional references.
/// @param map Borrowed minimap payload receiving the marker.
/// @param entity Borrowed Entity3D handle for a following marker, or `NULL`.
/// @param point Three-component fixed world point used when @p entity is `NULL`.
/// @param icon Borrowed Pixels icon to retain, or `NULL` for a color chip.
/// @param color Packed `0xRRGGBB` fallback color.
/// @return Positive marker identifier, or zero after the fixed marker budget is exhausted.
static int64_t game3d_minimap_add_marker_slot(
    rt_game3d_minimap *map, void *entity, const double point[3], void *icon, int64_t color) {
    if (!game3d_minimap_pixels_valid(icon,
                                     "Game3D.Minimap3D.AddMarker: icon must be Pixels or null"))
        return 0;
    for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
        rt_game3d_minimap_marker *marker = &map->markers[i];
        if (marker->used)
            continue;
        game3d_release_typed_ref(&marker->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        game3d_release_ref(&marker->icon);
        memset(marker, 0, sizeof(*marker));
        marker->used = 1;
        marker->edge_clamp = 1;
        marker->scale = 1.0;
        marker->color = color;
        if (entity)
            game3d_assign_typed_ref(&marker->entity, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        else
            memcpy(marker->point, point, 3 * sizeof(double));
        if (icon)
            game3d_assign_ref(&marker->icon, icon);
        map->marker_ids[i] = 0;
        map->marker_ids[i] = game3d_minimap_next_marker_id(map);
        if (map->marker_ids[i] <= 0) {
            game3d_release_typed_ref(&marker->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
            game3d_release_ref(&marker->icon);
            marker->used = 0;
            rt_trap("Game3D.Minimap3D.AddMarker: no marker identifiers available");
            return 0;
        }
        return map->marker_ids[i];
    }
    rt_trap("Game3D.Minimap3D.AddMarker: marker budget (64) exceeded");
    return 0;
}

/// @brief Add a marker following an entity. Returns a marker id.
/// @param obj Borrowed Minimap3D handle.
/// @param entity Borrowed Entity3D handle retained by the marker.
/// @param icon Borrowed Pixels icon retained by the marker, or `NULL` for a color chip.
/// @param color Packed `0xRRGGBB` fallback color.
/// @return Positive marker identifier, or zero after invalid input or budget exhaustion.
int64_t rt_game3d_minimap_add_marker(void *obj, void *entity, void *icon, int64_t color) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.AddMarker: invalid minimap");
    if (!map)
        return 0;
    rt_game3d_entity *marker_entity =
        game3d_entity_checked(entity, "Game3D.Minimap3D.AddMarker: expected a live Entity3D");
    if (!marker_entity)
        return 0;
    return game3d_minimap_add_marker_slot(map, marker_entity, NULL, icon, color);
}

/// @brief Add a marker at a fixed world point. Returns a marker id.
/// @param obj Borrowed Minimap3D handle.
/// @param point Borrowed Vec3 containing the fixed marker position.
/// @param icon Borrowed Pixels icon retained by the marker, or `NULL` for a color chip.
/// @param color Packed `0xRRGGBB` fallback color.
/// @return Positive marker identifier, or zero after invalid input or budget exhaustion.
int64_t rt_game3d_minimap_add_marker_at(void *obj, void *point, void *icon, int64_t color) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.AddMarkerAt: invalid minimap");
    double p[3];
    if (!map)
        return 0;
    if (!game3d_read_vec3(point, p, "Game3D.Minimap3D.AddMarkerAt: point must be Vec3"))
        return 0;
    return game3d_minimap_add_marker_slot(map, NULL, p, icon, color);
}

/// @brief Remove a marker by id (unknown ids are a safe no-op).
/// @param obj Borrowed Minimap3D handle.
/// @param id Marker identifier to remove.
void rt_game3d_minimap_remove_marker(void *obj, int64_t id) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.RemoveMarker: invalid minimap");
    if (!map)
        return;
    for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
        if (!map->markers[i].used || map->marker_ids[i] != id)
            continue;
        game3d_release_typed_ref(&map->markers[i].entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        game3d_release_ref(&map->markers[i].icon);
        map->markers[i].used = 0;
        map->marker_ids[i] = 0;
        return;
    }
}

/// @brief Toggle rim clamping for off-map markers (default on).
/// @param obj Borrowed Minimap3D handle.
/// @param id Marker identifier to update; unknown identifiers are ignored.
/// @param clamp Nonzero to clamp the marker to the minimap border.
void rt_game3d_minimap_set_marker_edge_clamp(void *obj, int64_t id, int8_t clamp) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetMarkerEdgeClamp: invalid minimap");
    rt_game3d_minimap_marker *marker = map ? game3d_minimap_marker_by_id(map, id) : NULL;
    if (marker)
        marker->edge_clamp = clamp ? 1 : 0;
}

/// @brief Scale a marker's icon/chip (default 1).
/// @param obj Borrowed Minimap3D handle.
/// @param id Marker identifier to update; unknown identifiers are ignored.
/// @param scale Positive finite glyph scale, capped at 8.
void rt_game3d_minimap_set_marker_scale(void *obj, int64_t id, double scale) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetMarkerScale: invalid minimap");
    rt_game3d_minimap_marker *marker = map ? game3d_minimap_marker_by_id(map, id) : NULL;
    if (marker && isfinite(scale) && scale > 0.0)
        marker->scale = scale > 8.0 ? 8.0 : scale;
}

/// @brief Project this marker onto the compass strip by bearing.
/// @param obj Borrowed Minimap3D handle.
/// @param id Marker identifier to update; unknown identifiers are ignored.
/// @param enabled Nonzero to show the marker within the forward compass hemisphere.
void rt_game3d_minimap_set_marker_on_compass(void *obj, int64_t id, int8_t enabled) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetMarkerOnCompass: invalid minimap");
    rt_game3d_minimap_marker *marker = map ? game3d_minimap_marker_by_id(map, id) : NULL;
    if (marker)
        marker->on_compass = enabled ? 1 : 0;
}

/// @brief Draw this marker as a screen-space objective indicator.
/// @param obj Borrowed Minimap3D handle.
/// @param id Marker identifier to update; unknown identifiers are ignored.
/// @param enabled Nonzero to render the projected or screen-edge objective glyph.
void rt_game3d_minimap_set_objective_indicator(void *obj, int64_t id, int8_t enabled) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.SetObjectiveIndicator: invalid minimap");
    rt_game3d_minimap_marker *marker = map ? game3d_minimap_marker_by_id(map, id) : NULL;
    if (marker)
        marker->objective = enabled ? 1 : 0;
}

/// @brief Number of live markers (telemetry/tests).
/// @param obj Borrowed Minimap3D handle.
/// @return Number of occupied marker slots, or zero for an invalid handle.
int64_t rt_game3d_minimap_get_marker_count(void *obj) {
    rt_game3d_minimap *map =
        game3d_minimap_checked(obj, "Game3D.Minimap3D.get_MarkerCount: invalid minimap");
    int64_t count = 0;
    if (map)
        for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i)
            if (map->markers[i].used)
                count++;
    return count;
}

/// @brief World X/Z to minimap viewport X (affine; viewport-unclamped but coordinate-bounded).
/// @param obj Borrowed Minimap3D handle.
/// @param world_x World-space X coordinate to project.
/// @param world_z World-space Z coordinate, accepted for API symmetry and otherwise unused.
/// @return Finite bounded viewport X coordinate, or zero for an invalid handle.
double rt_game3d_minimap_map_x(void *obj, double world_x, double world_z) {
    rt_game3d_minimap *map = game3d_minimap_checked(obj, "Game3D.Minimap3D.MapX: invalid minimap");
    (void)world_z;
    if (!map)
        return 0.0;
    world_x = game3d_clamp_coord_or(world_x, map->world_min_x);
    double span = map->world_max_x - map->world_min_x;
    double u = span > 1e-12 ? (world_x - map->world_min_x) / span : 0.5;
    return game3d_clamp_abs_or(
        map->view_x + u * map->view_w, map->view_x, MINIMAP3D_SCREEN_ABS_MAX);
}

/// @brief World X/Z to minimap viewport Y (affine; viewport-unclamped but coordinate-bounded).
/// @param obj Borrowed Minimap3D handle.
/// @param world_x World-space X coordinate, accepted for API symmetry and otherwise unused.
/// @param world_z World-space Z coordinate to project.
/// @return Finite bounded viewport Y coordinate, or zero for an invalid handle.
double rt_game3d_minimap_map_y(void *obj, double world_x, double world_z) {
    rt_game3d_minimap *map = game3d_minimap_checked(obj, "Game3D.Minimap3D.MapY: invalid minimap");
    (void)world_x;
    if (!map)
        return 0.0;
    world_z = game3d_clamp_coord_or(world_z, map->world_min_z);
    double span = map->world_max_z - map->world_min_z;
    double v = span > 1e-12 ? (world_z - map->world_min_z) / span : 0.5;
    return game3d_clamp_abs_or(
        map->view_y + v * map->view_h, map->view_y, MINIMAP3D_SCREEN_ABS_MAX);
}

/// @brief Wrap an angle to [-pi, pi].
/// @param angle Angle in radians.
/// @return Coterminal angle in the inclusive `[-pi, pi]` interval.
static double game3d_minimap_wrap_angle(double angle) {
    if (!isfinite(angle))
        return 0.0;
    while (angle > MINIMAP3D_PI)
        angle -= 2.0 * MINIMAP3D_PI;
    while (angle < -MINIMAP3D_PI)
        angle += 2.0 * MINIMAP3D_PI;
    return angle;
}

/// @brief Clamp a map-space point to the viewport border along the ray from
///   the viewport center (the rim-bearing clamp).
/// @param map Borrowed minimap payload defining the viewport.
/// @param[in,out] px Horizontal point coordinate to clamp.
/// @param[in,out] py Vertical point coordinate to clamp.
static void game3d_minimap_clamp_to_rim(rt_game3d_minimap *map, double *px, double *py) {
    double cx = map->view_x + map->view_w * 0.5;
    double cy = map->view_y + map->view_h * 0.5;
    double dx = *px - cx;
    double dy = *py - cy;
    double half_w = map->view_w * 0.5;
    double half_h = map->view_h * 0.5;
    double sx = dx != 0.0 ? half_w / fabs(dx) : 1e30;
    double sy = dy != 0.0 ? half_h / fabs(dy) : 1e30;
    double s = sx < sy ? sx : sy;
    if (s < 1.0) {
        *px = cx + dx * s;
        *py = cy + dy * s;
    }
}

/// @brief Resolve a marker's current world position; 0 = drop (stale entity).
/// @details Entity-following markers are removed in place when their retained entity is stale.
/// Fixed-point markers copy their stored coordinates without additional validation.
/// @param map Borrowed minimap payload owning the marker.
/// @param[in,out] marker Marker slot to resolve and possibly invalidate.
/// @param slot Marker-array index used to clear the parallel identifier.
/// @param[out] out Required three-component world-position destination.
/// @return Nonzero when @p out receives a position; zero when a stale marker was discarded.
static int game3d_minimap_marker_world(rt_game3d_minimap *map,
                                       rt_game3d_minimap_marker *marker,
                                       int32_t slot,
                                       double out[3]) {
    if (marker->entity) {
        rt_game3d_entity *entity = (rt_game3d_entity *)rt_g3d_checked_or_null(
            marker->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        if (!entity || !entity->alive) {
            game3d_release_typed_ref(&marker->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
            game3d_release_ref(&marker->icon);
            marker->used = 0;
            map->marker_ids[slot] = 0;
            return 0;
        }
        return game3d_entity_world_position_components(entity, out);
    }
    memcpy(out, marker->point, 3 * sizeof(double));
    return 1;
}

/// @brief Draw one marker glyph (icon or color chip) centered at px/py.
/// @param canvas Borrowed Canvas3D handle receiving overlay primitives.
/// @param marker Borrowed marker supplying its icon, color, and scale.
/// @param px Horizontal glyph-center coordinate in pixels.
/// @param py Vertical glyph-center coordinate in pixels.
static void game3d_minimap_draw_glyph(void *canvas,
                                      rt_game3d_minimap_marker *marker,
                                      double px,
                                      double py) {
    marker->scale = game3d_positive_clamped_or(marker->scale, 1.0, 8.0);
    double size = 8.0 * marker->scale;
    if (marker->icon) {
        rt_canvas3d_draw_image2d(canvas,
                                 (int64_t)(px - size),
                                 (int64_t)(py - size),
                                 (int64_t)(size * 2.0),
                                 (int64_t)(size * 2.0),
                                 marker->icon);
    } else {
        rt_canvas3d_draw_rect2d(canvas,
                                (int64_t)(px - size * 0.5),
                                (int64_t)(py - size * 0.5),
                                (int64_t)size,
                                (int64_t)size,
                                marker->color);
    }
}

/// @brief Render the minimap, compass, and objective indicators through the
///   Canvas3D overlay primitives. Games call this from their HUD pass.
/// @param obj Borrowed Minimap3D handle.
void rt_game3d_minimap_draw(void *obj) {
    rt_game3d_minimap *map = game3d_minimap_checked(obj, "Game3D.Minimap3D.Draw: invalid minimap");
    if (!map)
        return;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(map->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    rt_game3d_entity *tracked = game3d_minimap_tracked_ref(map);
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    void *camera = world ? rt_g3d_checked_or_null(world->camera, RT_G3D_CAMERA3D_CLASS_ID) : NULL;
    if (!world || !canvas)
        return;

    /* Map backdrop: authored image, or a dark panel when none is set. */
    if (map->map_image)
        rt_canvas3d_draw_image2d(canvas,
                                 (int64_t)map->view_x,
                                 (int64_t)map->view_y,
                                 (int64_t)map->view_w,
                                 (int64_t)map->view_h,
                                 map->map_image);
    else
        rt_canvas3d_draw_rect2d(canvas,
                                (int64_t)map->view_x,
                                (int64_t)map->view_y,
                                (int64_t)map->view_w,
                                (int64_t)map->view_h,
                                0x10241C);

    /* Camera yaw from the forward vector (bearing convention: -Z = north). */
    double yaw = 0.0;
    if (camera) {
        void *forward = rt_camera3d_get_forward(camera);
        if (forward) {
            yaw = atan2(rt_vec3_x(forward), -rt_vec3_z(forward));
            game3d_release_ref(&forward);
        }
    }
    if (!isfinite(yaw))
        yaw = 0.0;

    /* Markers on the map. */
    for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
        rt_game3d_minimap_marker *marker = &map->markers[i];
        if (!marker->used)
            continue;
        double world_pos[3];
        if (!game3d_minimap_marker_world(map, marker, i, world_pos))
            continue;
        double px = rt_game3d_minimap_map_x(map, world_pos[0], world_pos[2]);
        double py = rt_game3d_minimap_map_y(map, world_pos[0], world_pos[2]);
        int inside = px >= map->view_x && px <= map->view_x + map->view_w && py >= map->view_y &&
                     py <= map->view_y + map->view_h;
        if (!inside) {
            if (!marker->edge_clamp)
                continue;
            game3d_minimap_clamp_to_rim(map, &px, &py);
        }
        game3d_minimap_draw_glyph(canvas, marker, px, py);
    }

    /* Tracked-entity arrow: a chip plus a heading tick along the camera yaw. */
    if (tracked) {
        double world_pos[3];
        if (game3d_entity_world_position_components(tracked, world_pos)) {
            double px = rt_game3d_minimap_map_x(map, world_pos[0], world_pos[2]);
            double py = rt_game3d_minimap_map_y(map, world_pos[0], world_pos[2]);
            game3d_minimap_clamp_to_rim(map, &px, &py);
            rt_canvas3d_draw_rect2d(canvas, (int64_t)(px - 4), (int64_t)(py - 4), 8, 8, 0xFFFFFF);
            double tx = px + sin(yaw) * 8.0;
            double ty = py - cos(yaw) * 8.0;
            rt_canvas3d_draw_rect2d(canvas, (int64_t)(tx - 2), (int64_t)(ty - 2), 4, 4, 0xFFFFFF);
        }
    }

    /* Compass strip: cardinal letters + on-compass markers by bearing. */
    if (map->compass_enabled) {
        double screen_w = game3d_minimap_screen_extent(world->width, 640.0);
        double cx = screen_w * 0.5;
        double top = 12.0;
        double half = map->compass_width * 0.5;
        rt_canvas3d_draw_rect2d(canvas,
                                (int64_t)(cx - half),
                                (int64_t)(top - 2),
                                (int64_t)map->compass_width,
                                18,
                                0x101820);
        static const char *kCardinals[4] = {"N", "E", "S", "W"};
        /* Intern the four compass labels once (held for process life) instead of
         * allocating a runtime string per label every frame; draw_text_3d borrows
         * the string, so a cached reference is safe. Minimap draw is main-thread. */
        static rt_string kCardinalLabels[4] = {NULL, NULL, NULL, NULL};
        for (int c = 0; c < 4; ++c) {
            double bearing = (double)c * MINIMAP3D_PI * 0.5;
            double rel = game3d_minimap_wrap_angle(bearing - yaw);
            if (fabs(rel) > MINIMAP3D_PI * 0.5)
                continue;
            double x = cx + rel / (MINIMAP3D_PI * 0.5) * half;
            if (!kCardinalLabels[c])
                kCardinalLabels[c] = rt_const_cstr(kCardinals[c]);
            if (kCardinalLabels[c])
                rt_canvas3d_draw_text_3d(
                    canvas, (int64_t)(x - 4), (int64_t)top, kCardinalLabels[c], 0xE8E8E8);
        }
        double center[3] = {0.0, 0.0, 0.0};
        if (tracked)
            (void)game3d_entity_world_position_components(tracked, center);
        for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
            rt_game3d_minimap_marker *marker = &map->markers[i];
            if (!marker->used || !marker->on_compass)
                continue;
            double world_pos[3];
            if (!game3d_minimap_marker_world(map, marker, i, world_pos))
                continue;
            double bearing = atan2(world_pos[0] - center[0], -(world_pos[2] - center[2]));
            double rel = game3d_minimap_wrap_angle(bearing - yaw);
            if (fabs(rel) > MINIMAP3D_PI * 0.5)
                continue;
            double x = cx + rel / (MINIMAP3D_PI * 0.5) * half;
            rt_canvas3d_draw_rect2d(
                canvas, (int64_t)(x - 2), (int64_t)(top + 12), 4, 4, marker->color);
        }
    }

    /* Objective indicators: on-screen at the projected point, else edge-clamped. */
    if (camera) {
        double screen_w = game3d_minimap_screen_extent(world->width, 640.0);
        double screen_h = game3d_minimap_screen_extent(world->height, 480.0);
        for (int32_t i = 0; i < MINIMAP3D_MAX_MARKERS; ++i) {
            rt_game3d_minimap_marker *marker = &map->markers[i];
            if (!marker->used || !marker->objective)
                continue;
            double world_pos[3];
            if (!game3d_minimap_marker_world(map, marker, i, world_pos))
                continue;
            double sx = 0.0, sy = 0.0;
            int8_t visible = rt_camera3d_world_to_screen(camera,
                                                         world_pos[0],
                                                         world_pos[1],
                                                         world_pos[2],
                                                         (int64_t)screen_w,
                                                         (int64_t)screen_h,
                                                         &sx,
                                                         &sy);
            if (!isfinite(sx) || !isfinite(sy))
                visible = 0;
            if (!visible) {
                /* Clamp along the horizontal bearing to the screen border. */
                double center[3] = {0.0, 0.0, 0.0};
                if (tracked)
                    (void)game3d_entity_world_position_components(tracked, center);
                double bearing = atan2(world_pos[0] - center[0], -(world_pos[2] - center[2]));
                double rel = game3d_minimap_wrap_angle(bearing - yaw);
                sx = screen_w * 0.5 + sin(rel) * (screen_w * 0.5 - 24.0);
                sy = screen_h * 0.5 - cos(rel) * (screen_h * 0.5 - 24.0);
            }
            if (sx < 16.0)
                sx = 16.0;
            if (sx > screen_w - 16.0)
                sx = screen_w - 16.0;
            if (sy < 16.0)
                sy = 16.0;
            if (sy > screen_h - 16.0)
                sy = screen_h - 16.0;
            game3d_minimap_draw_glyph(canvas, marker, sx, sy);
        }
    }
}

#else
typedef int rt_game3d_minimap_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
