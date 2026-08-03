//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_sky3d.c
// Purpose: Procedural analytic sky — a CPU-generated low-res cubemap (gradient
//   sky with turbidity-tinted horizon, sun disc + halo, night fade) installed
//   through the existing skybox path so IBL, fog, and all four backends pick
//   it up with zero shader changes.
// Key invariants:
//   - Regeneration is explicit (Update) and cheap (default 64x64 faces);
//     the skybox-change path triggers the existing lazy IBL rebuild.
//   - Deterministic: pure function of sun direction/turbidity/ground albedo.
// Ownership/Lifetime:
//   - GC-managed; the sky retains its generated cubemap between updates.
// Links: ADR 0090, rt_sky3d.h.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sky3d.c
 * @brief Implements deterministic procedural-sky cubemap generation for Canvas3D.
 *
 * Sky radiance is evaluated on the CPU from sun direction, turbidity, and ground albedo. Explicit
 * updates regenerate a six-face cubemap and install it through the normal skybox path so fog and
 * image-based lighting observe the same environment.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_sky3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern int64_t rt_obj_class_id(void *obj);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_pixels_new(int64_t width, int64_t height);
extern void rt_canvas3d_set_skybox(void *canvas, void *cubemap);

typedef struct rt_sky3d {
    void *vptr;
    double sun_dir[3]; /* normalized, toward the sun */
    double turbidity;  /* 1 (clear) .. 10 (hazy) */
    double ground_albedo[3];
    int64_t resolution;
    int8_t dirty;
    void *cubemap; /* retained generated CubeMap3D */
} rt_sky3d;

/// @brief Release a retained cubemap only when its runtime class is still valid.
/// @param[in,out] slot Sky cubemap ownership slot.
static void sky3d_release_cubemap(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release the generated cubemap retained by a Sky3D object.
/// @param obj Sky3D runtime object being finalized; NULL is accepted.
static void sky3d_finalize(void *obj) {
    rt_sky3d *sky = (rt_sky3d *)obj;
    if (sky)
        sky3d_release_cubemap(&sky->cubemap);
}

/// @brief Allocate a procedural sky with clear-day defaults and a dirty 64-pixel cubemap.
/// @return New GC-managed Sky3D handle, or NULL after trapping on allocation failure.
void *rt_sky3d_new(void) {
    rt_sky3d *sky = (rt_sky3d *)rt_obj_new_i64(RT_G3D_SKY3D_CLASS_ID, (int64_t)sizeof(rt_sky3d));
    if (!sky) {
        rt_trap("Sky3D.New: allocation failed");
        return NULL;
    }
    memset(sky, 0, sizeof(*sky));
    rt_obj_set_finalizer(sky, sky3d_finalize);
    sky->sun_dir[0] = 0.3;
    sky->sun_dir[1] = 0.8;
    sky->sun_dir[2] = 0.5;
    double len = sqrt(sky->sun_dir[0] * sky->sun_dir[0] + sky->sun_dir[1] * sky->sun_dir[1] +
                      sky->sun_dir[2] * sky->sun_dir[2]);
    sky->sun_dir[0] /= len;
    sky->sun_dir[1] /= len;
    sky->sun_dir[2] /= len;
    sky->turbidity = 2.5;
    sky->ground_albedo[0] = sky->ground_albedo[1] = sky->ground_albedo[2] = 0.25;
    sky->resolution = 64;
    sky->dirty = 1;
    return sky;
}

/// @brief Validate a runtime handle as Sky3D and trap with caller-supplied context on failure.
/// @param obj Candidate runtime object handle.
/// @param method Trap message identifying the calling API.
/// @return Typed Sky3D pointer, or NULL when validation fails.
static rt_sky3d *sky3d_checked(void *obj, const char *method) {
    rt_sky3d *sky = (rt_sky3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKY3D_CLASS_ID);
    if (!sky)
        rt_trap(method);
    return sky;
}

/// @brief Set and normalize the direction from the scene toward the sun.
/// @param obj Sky3D handle; invalid handles trap and are ignored.
/// @param direction Vec3 handle containing the new direction; invalid or near-zero vectors are
/// ignored.
void rt_sky3d_set_sun_direction(void *obj, void *direction) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.SetSunDirection: invalid sky");
    if (!sky || !direction || rt_obj_class_id(direction) != RT_VEC3_CLASS_ID)
        return;
    double d[3] = {rt_vec3_x(direction), rt_vec3_y(direction), rt_vec3_z(direction)};
    double scale = fmax(fabs(d[0]), fmax(fabs(d[1]), fabs(d[2])));
    if (!isfinite(d[0]) || !isfinite(d[1]) || !isfinite(d[2]) || !isfinite(scale) || scale < 1e-12)
        return;
    d[0] /= scale;
    d[1] /= scale;
    d[2] /= scale;
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!isfinite(len) || len < 1e-12)
        return;
    d[0] /= len;
    d[1] /= len;
    d[2] /= len;
    if (sky->sun_dir[0] != d[0] || sky->sun_dir[1] != d[1] || sky->sun_dir[2] != d[2]) {
        memcpy(sky->sun_dir, d, sizeof(d));
        sky->dirty = 1;
    }
}

/// @brief Set the atmospheric turbidity controlling horizon haze and sun-halo width.
/// @param obj Sky3D handle; invalid handles trap and are ignored.
/// @param turbidity Requested turbidity, clamped to `[1, 10]`; non-finite values are ignored.
void rt_sky3d_set_turbidity(void *obj, double turbidity) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.set_Turbidity: invalid sky");
    if (!sky || !isfinite(turbidity))
        return;
    if (turbidity < 1.0)
        turbidity = 1.0;
    if (turbidity > 10.0)
        turbidity = 10.0;
    if (sky->turbidity != turbidity) {
        sky->turbidity = turbidity;
        sky->dirty = 1;
    }
}

/// @brief Return the current atmospheric turbidity.
/// @param obj Sky3D handle; invalid handles trap.
/// @return Turbidity in `[1, 10]`, or zero when validation fails.
double rt_sky3d_get_turbidity(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_Turbidity: invalid sky");
    return sky ? sky->turbidity : 0.0;
}

/// @brief Set the RGB albedo used to shade the cubemap's ground hemisphere.
/// @param obj Sky3D handle; invalid handles trap and are ignored.
/// @param r Red albedo component, clamped to `[0, 1]`.
/// @param g Green albedo component, clamped to `[0, 1]`.
/// @param b Blue albedo component, clamped to `[0, 1]`.
void rt_sky3d_set_ground_albedo(void *obj, double r, double g, double b) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.SetGroundAlbedo: invalid sky");
    if (!sky)
        return;
    double albedo[3] = {isfinite(r) && r > 0.0 ? (r > 1.0 ? 1.0 : r) : 0.0,
                        isfinite(g) && g > 0.0 ? (g > 1.0 ? 1.0 : g) : 0.0,
                        isfinite(b) && b > 0.0 ? (b > 1.0 ? 1.0 : b) : 0.0};
    if (memcmp(sky->ground_albedo, albedo, sizeof(albedo)) != 0) {
        memcpy(sky->ground_albedo, albedo, sizeof(albedo));
        sky->dirty = 1;
    }
}

/// @brief `Sky3D.SunDirection` — fresh Vec3 of the normalized sun direction (ADR 0233).
/// @param obj Borrowed Sky3D handle.
/// @return Newly allocated direction snapshot; origin for an invalid handle.
void *rt_sky3d_get_sun_direction(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_SunDirection: invalid sky");
    if (!sky)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(sky->sun_dir[0], sky->sun_dir[1], sky->sun_dir[2]);
}

/// @brief `Sky3D.GroundAlbedo` — fresh Vec3 of the retained ground albedo (ADR 0233).
/// @param obj Borrowed Sky3D handle.
/// @return Newly allocated albedo snapshot; origin for an invalid handle.
void *rt_sky3d_get_ground_albedo(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_GroundAlbedo: invalid sky");
    if (!sky)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(sky->ground_albedo[0], sky->ground_albedo[1], sky->ground_albedo[2]);
}

/// @brief Set the square pixel resolution generated for each cubemap face.
/// @param obj Sky3D handle; invalid handles trap and are ignored.
/// @param resolution Requested face dimension, clamped to `[16, 256]`.
void rt_sky3d_set_resolution(void *obj, int64_t resolution) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.set_Resolution: invalid sky");
    if (!sky)
        return;
    if (resolution < 16)
        resolution = 16;
    if (resolution > 256)
        resolution = 256;
    if (sky->resolution != resolution) {
        sky->resolution = resolution;
        sky->dirty = 1;
    }
}

/// @brief Return the configured cubemap-face resolution.
/// @param obj Sky3D handle; invalid handles trap.
/// @return Face dimension in pixels, or zero when validation fails.
int64_t rt_sky3d_get_resolution(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_Resolution: invalid sky");
    return sky ? sky->resolution : 0;
}

/// @brief Report whether authored sky parameters require cubemap regeneration.
/// @param obj Sky3D handle; invalid handles trap.
/// @return 1 when the generated cubemap is stale, otherwise 0.
int8_t rt_sky3d_get_dirty(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_Dirty: invalid sky");
    return sky ? sky->dirty : 0;
}

/// @brief Analytic sky radiance for a view direction (gradient + sun + night fade).
/// @details Simplified two-band model (recorded per plan section 8): a
///   zenith/horizon gradient whose colors follow sun elevation (blue day, warm
///   sunset band near the sun azimuth, near-black night), a Mie-inspired sun
///   halo, a splatted sun disc, and a constant ground-albedo hemisphere.
/// @param sky Sky parameters controlling the analytic model.
/// @param dir Normalized outward sample direction.
/// @param out Three-element output array receiving linear RGB radiance.
static void sky3d_radiance(const rt_sky3d *sky, const double dir[3], double out[3]) {
    double sun_elev = sky->sun_dir[1]; /* sin(elevation) */
    double day = sun_elev <= -0.15 ? 0.0 : (sun_elev >= 0.25 ? 1.0 : (sun_elev + 0.15) / 0.40);
    double dusk = (1.0 - fabs(sun_elev) * 4.0);
    if (dusk < 0.0)
        dusk = 0.0;

    if (dir[1] < 0.0) {
        /* Ground hemisphere: albedo lit by the day factor. */
        double lit = 0.15 + 0.85 * day;
        out[0] = sky->ground_albedo[0] * lit;
        out[1] = sky->ground_albedo[1] * lit;
        out[2] = sky->ground_albedo[2] * lit;
        return;
    }

    /* Base gradient: zenith and horizon colors by day/dusk mix. */
    double haze = (sky->turbidity - 1.0) / 9.0;
    double zenith[3] = {0.10 + 0.12 * haze, 0.28 + 0.10 * haze, 0.62};
    double horizon[3] = {0.55 + 0.25 * haze, 0.68 + 0.15 * haze, 0.85};
    double night[3] = {0.012, 0.014, 0.03};
    double sunset[3] = {0.95, 0.45, 0.18};
    double cos_sun = dir[0] * sky->sun_dir[0] + dir[1] * sky->sun_dir[1] + dir[2] * sky->sun_dir[2];
    double t = pow(1.0 - dir[1], 2.0); /* horizon weight */
    double base[3];
    for (int c = 0; c < 3; ++c) {
        double day_col = zenith[c] + (horizon[c] - zenith[c]) * t;
        /* Sunset tint pools on the horizon toward the sun. */
        double sun_side = cos_sun > 0.0 ? cos_sun : 0.0;
        day_col += (sunset[c] - day_col) * dusk * t * sun_side * 0.8;
        base[c] = night[c] + (day_col - night[c]) * day;
    }

    /* Sun halo (Mie-inspired forward lobe) + disc splat. */
    double halo = cos_sun > 0.0 ? pow(cos_sun, 64.0 + 192.0 * (1.0 - haze)) : 0.0;
    double disc = cos_sun > 0.9995 ? 1.0 : 0.0;
    double sun_strength = day * (0.35 + 0.65 * (1.0 - haze));
    double sun_col[3] = {1.0, 0.92 - 0.35 * dusk, 0.80 - 0.55 * dusk};
    for (int c = 0; c < 3; ++c) {
        out[c] = base[c] + sun_col[c] * (halo * 0.6 + disc * 4.0) * sun_strength;
        if (out[c] > 4.0)
            out[c] = 4.0;
    }
}

/// @brief Map a face texel to its outward direction (CubeMap3D face order).
/// @param face Cubemap face index: positive/negative x, y, then z.
/// @param u Horizontal face coordinate in `[-1, 1]`.
/// @param v Vertical face coordinate in `[-1, 1]`.
/// @param out Three-element output array receiving the normalized direction.
static void sky3d_face_dir(int face, double u, double v, double out[3]) {
    /* u, v in [-1, 1]; faces: +X, -X, +Y, -Y, +Z, -Z. */
    switch (face) {
        case 0:
            out[0] = 1.0;
            out[1] = -v;
            out[2] = -u;
            break;
        case 1:
            out[0] = -1.0;
            out[1] = -v;
            out[2] = u;
            break;
        case 2:
            out[0] = u;
            out[1] = 1.0;
            out[2] = v;
            break;
        case 3:
            out[0] = u;
            out[1] = -1.0;
            out[2] = -v;
            break;
        case 4:
            out[0] = u;
            out[1] = -v;
            out[2] = 1.0;
            break;
        default:
            out[0] = -u;
            out[1] = -v;
            out[2] = -1.0;
            break;
    }
    double len = sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
}

/// @brief Regenerate the sky cubemap if dirty and install it as @p canvas's skybox.
/// @details Installing through the normal skybox path re-triggers the existing lazy
///   IBL rebuild, so environment lighting follows the sky for free. Passing a NULL
///   canvas regenerates without installing (tests/tooling).
/// @param obj Sky3D handle; invalid handles trap.
/// @param canvas Optional Canvas3D handle that receives the generated cubemap as its skybox.
/// @return 1 when a (re)generated cubemap is available.
int8_t rt_sky3d_update(void *obj, void *canvas) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.Update: invalid sky");
    if (!sky)
        return 0;
    if (canvas && !rt_g3d_has_class(canvas, RT_G3D_CANVAS3D_CLASS_ID))
        return 0;
    if (sky->resolution < 16) {
        sky->resolution = 16;
        sky->dirty = 1;
    } else if (sky->resolution > 256) {
        sky->resolution = 256;
        sky->dirty = 1;
    }
    if (!isfinite(sky->turbidity) || sky->turbidity < 1.0 || sky->turbidity > 10.0) {
        sky->turbidity = 2.5;
        sky->dirty = 1;
    }
    for (int lane = 0; lane < 3; ++lane) {
        if (!isfinite(sky->ground_albedo[lane]) || sky->ground_albedo[lane] < 0.0 ||
            sky->ground_albedo[lane] > 1.0) {
            sky->ground_albedo[lane] = 0.25;
            sky->dirty = 1;
        }
    }
    {
        double old_dir[3] = {sky->sun_dir[0], sky->sun_dir[1], sky->sun_dir[2]};
        double dir_scale =
            fmax(fabs(sky->sun_dir[0]), fmax(fabs(sky->sun_dir[1]), fabs(sky->sun_dir[2])));
        if (!isfinite(dir_scale) || dir_scale < 1e-12) {
            sky->sun_dir[0] = 0.0;
            sky->sun_dir[1] = 1.0;
            sky->sun_dir[2] = 0.0;
            sky->dirty = 1;
        } else {
            double sx = sky->sun_dir[0] / dir_scale;
            double sy = sky->sun_dir[1] / dir_scale;
            double sz = sky->sun_dir[2] / dir_scale;
            double len = sqrt(sx * sx + sy * sy + sz * sz);
            if (!isfinite(len) || len < 1e-12) {
                sky->sun_dir[0] = 0.0;
                sky->sun_dir[1] = 1.0;
                sky->sun_dir[2] = 0.0;
            } else {
                sky->sun_dir[0] = sx / len;
                sky->sun_dir[1] = sy / len;
                sky->sun_dir[2] = sz / len;
            }
        }
        if (old_dir[0] != sky->sun_dir[0] || old_dir[1] != sky->sun_dir[1] ||
            old_dir[2] != sky->sun_dir[2])
            sky->dirty = 1;
    }
    if (sky->cubemap && !rt_cubemap3d_is_complete(sky->cubemap)) {
        sky3d_release_cubemap(&sky->cubemap);
        sky->dirty = 1;
    }
    if (sky->dirty || !sky->cubemap) {
        int64_t dim = sky->resolution;
        void *faces[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
        int ok = 1;
        for (int f = 0; f < 6 && ok; ++f) {
            faces[f] = rt_pixels_new(dim, dim);
            if (!faces[f]) {
                ok = 0;
                break;
            }
            rt_pixels_impl *face = rt_pixels_checked_impl_or_null(faces[f]);
            if (!face || !face->data || face->width != dim || face->height != dim) {
                ok = 0;
                break;
            }
            for (int64_t y = 0; y < dim; ++y) {
                for (int64_t x = 0; x < dim; ++x) {
                    double u = ((double)x + 0.5) / (double)dim * 2.0 - 1.0;
                    double v = ((double)y + 0.5) / (double)dim * 2.0 - 1.0;
                    double dir[3];
                    sky3d_face_dir(f, u, v, dir);
                    double rgb[3];
                    sky3d_radiance(sky, dir, rgb);
                    int64_t ir = (int64_t)(fmin(fmax(rgb[0], 0.0), 1.0) * 255.0 + 0.5);
                    int64_t ig = (int64_t)(fmin(fmax(rgb[1], 0.0), 1.0) * 255.0 + 0.5);
                    int64_t ib = (int64_t)(fmin(fmax(rgb[2], 0.0), 1.0) * 255.0 + 0.5);
                    face->data[(size_t)y * (size_t)dim + (size_t)x] =
                        (uint32_t)((ir << 24) | (ig << 16) | (ib << 8) | 0xFF);
                }
            }
            pixels_touch(face);
        }
        if (ok) {
            void *cubemap =
                rt_cubemap3d_new(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
            if (cubemap) {
                sky3d_release_cubemap(&sky->cubemap);
                sky->cubemap = cubemap;
                sky->dirty = 0;
            } else {
                ok = 0;
            }
        }
        for (int f = 0; f < 6; ++f) {
            if (faces[f] && rt_obj_release_check0(faces[f]))
                rt_obj_free(faces[f]);
        }
        if (!ok)
            return 0;
    }
    if (canvas && sky->cubemap)
        rt_canvas3d_set_skybox(canvas, sky->cubemap);
    return sky->cubemap ? 1 : 0;
}

/// @brief Retained generated cubemap (NULL before the first Update).
/// @param obj Sky3D handle; invalid handles trap.
/// @return Retained CubeMap3D handle for the generated sky, or NULL before generation or on invalid
/// input. The caller owns the returned reference.
void *rt_sky3d_get_cubemap(void *obj) {
    rt_sky3d *sky = sky3d_checked(obj, "Sky3D.get_Cubemap: invalid sky");
    if (!sky || !sky->cubemap)
        return NULL;
    if (!rt_cubemap3d_is_complete(sky->cubemap)) {
        sky3d_release_cubemap(&sky->cubemap);
        sky->dirty = 1;
        return NULL;
    }
    rt_obj_retain_maybe(sky->cubemap);
    return sky->cubemap;
}

#else
typedef int rt_sky3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
