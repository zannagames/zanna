//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_camera3d.c
// Purpose: Zanna.Graphics3D.Camera3D — perspective camera with view/projection.
//   Uses existing Mat4 perspective/lookAt math from rt_mat4.c.
//
// Key invariants:
//   - Matrices stored as double[16] row-major (matching Mat4 convention)
//   - OpenGL NDC convention: Z in [-1, 1]
//   - Right-handed: +X right, +Y up, +Z toward viewer
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h, rt_mat4.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements sanitized perspective/orthographic cameras and screen-space transforms.
///
/// Camera state uses row-major double-precision view and projection matrices, with guarded
/// constructors, FPS/orbit/follow controls, deterministic shake, render-aspect overrides,
/// world-to-screen projection, and cached inverse view-projection picking rays.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_mat4.h"
#include "rt_platform.h"

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include <string.h>

#define CAMERA3D_WORLD_ABS_MAX 1000000000000.0
#define CAMERA3D_CLIP_MAX 1000000000000.0
#define CAMERA3D_ASPECT_MAX 1000000.0
#define CAMERA3D_ORTHO_SIZE_MAX 1000000000.0
#define CAMERA3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define CAMERA3D_DAMPING_EXP_MAX 60.0
#define CAMERA3D_MAX_FAR_NEAR_RATIO 10000000.0

/// @brief Allocates a runtime object payload with a graphics class identifier.
/// @param class_id Runtime class identifier.
/// @param byte_size Positive payload size in bytes.
/// @return Owned object handle, or null on allocation failure.
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
#include "rt_trap.h"

/* Access existing Vec3 and Mat4 public API */
/// @brief Allocates a runtime `Vec3`.
/// @param x Vector x component.
/// @param y Vector y component.
/// @param z Vector z component.
/// @return Owned vector handle, or null on allocation failure.
extern void *rt_vec3_new(double x, double y, double z);

/// @brief Reads a borrowed `Vec3` x component.
/// @param v Borrowed vector handle.
/// @return Stored x component.
extern double rt_vec3_x(void *v);

/// @brief Reads a borrowed `Vec3` y component.
/// @param v Borrowed vector handle.
/// @return Stored y component.
extern double rt_vec3_y(void *v);

/// @brief Reads a borrowed `Vec3` z component.
/// @param v Borrowed vector handle.
/// @return Stored z component.
extern double rt_vec3_z(void *v);

/// @brief Builds an OpenGL-style orthographic projection matrix.
/// @param[out] m Writable 16-element matrix.
/// @param left Left view-volume coordinate.
/// @param right Right view-volume coordinate.
/// @param bottom Bottom view-volume coordinate.
/// @param top Top view-volume coordinate.
/// @param near_val Near-plane distance.
/// @param far_val Far-plane distance.
static void build_ortho(double *m,
                        double left,
                        double right,
                        double bottom,
                        double top,
                        double near_val,
                        double far_val);

/// @brief Generate a non-zero seed for Camera3D shake noise.
/// @details Uses a local monotonic counter rather than a fixed seed or object
///          address. This keeps standalone graphics contract tests independent
///          from the core runtime RNG while avoiding identical default shake
///          sequences across cameras.
/// @return Non-zero xorshift seed value.
static uint32_t camera3d_next_shake_seed(void) {
    static int64_t counter = INT64_C(0x12345678);
    int64_t old = rt_atomic_fetch_add_i64(&counter, INT64_C(0x9E3779B9), __ATOMIC_RELAXED);
    uint32_t seed = (uint32_t)old ^ 0xA341316Cu;
    return seed ? seed : 0x12345678u;
}

/// @brief Return `value` when finite, else `fallback`. Scalar boundary sanitizer.
/// @param value Candidate scalar.
/// @param fallback Replacement for NaN or infinity.
/// @return @p value when finite; otherwise @p fallback.
static double finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp `value` into `[-max_abs, max_abs]`, substituting `fallback` when not finite.
/// @param value Candidate scalar.
/// @param fallback Replacement for a non-finite candidate.
/// @param max_abs Non-negative symmetric magnitude bound.
/// @return Finite fallback or clamped candidate.
static double clamp_abs_or(double value, double fallback, double max_abs) {
    value = finite_or(value, fallback);
    if (value > max_abs)
        return max_abs;
    if (value < -max_abs)
        return -max_abs;
    return value;
}

/// @brief True if `value` is finite and within ±CAMERA3D_FLOAT_ABS_MAX (safe to narrow to float).
/// @param value Candidate double-precision matrix element.
/// @return `1` when narrowing to float stays within finite range; otherwise `0`.
static int camera_value_fits_float(double value) {
    return isfinite(value) && value >= -CAMERA3D_FLOAT_ABS_MAX && value <= CAMERA3D_FLOAT_ABS_MAX;
}

/// @brief Write the 4×4 identity matrix into `m` (16 doubles); no-op on NULL.
/// @param[out] m Writable 16-element matrix; may be null.
static void camera_identity_matrix(double *m) {
    if (!m)
        return;
    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

/// @brief True only if all 16 matrix elements are finite (NULL matrix returns false).
/// @param m Borrowed 16-element matrix.
/// @return `1` when @p m is non-null and every element is finite; otherwise `0`.
static int camera_matrix_is_finite(const double *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    return 1;
}

/// @brief Post-build guard: replace a matrix with identity if any element is non-finite.
/// @param[in,out] m Writable 16-element matrix; a null pointer remains a no-op.
static void camera_finish_matrix(double *m) {
    if (!camera_matrix_is_finite(m))
        camera_identity_matrix(m);
}

/// @brief Clamp `value` to `[0, +∞)`, substituting `fallback` when not finite.
/// @details Used for camera knobs (FOV, clip distances, exposure) where negatives would
///   invert the geometry or exponent and NaN would propagate into the view matrix.
/// @param value Candidate scalar.
/// @param fallback Replacement for a non-finite candidate.
/// @return Finite value clamped to `[0, CAMERA3D_WORLD_ABS_MAX]`.
static double sanitize_nonnegative(double value, double fallback) {
    value = finite_or(value, fallback);
    if (value < 0.0)
        return 0.0;
    return value > CAMERA3D_WORLD_ABS_MAX ? CAMERA3D_WORLD_ABS_MAX : value;
}

/// @brief Component-wise fallback for a 3-vector — each non-finite lane uses `fallback[i]`.
/// @details Used to recover camera position / target / up when the caller hands in a
///   Vec3 containing NaNs (e.g., from an earlier divide-by-zero); substituting known-good
///   fallbacks keeps the view matrix construction from producing a non-invertible matrix.
/// @param[in,out] v Three-element vector sanitized component by component.
/// @param fallback Borrowed three-element finite fallback vector.
static void sanitize_vec3(double v[3], const double fallback[3]) {
    if (!v || !fallback)
        return;
    for (int i = 0; i < 3; i++)
        v[i] = clamp_abs_or(v[i], fallback[i], CAMERA3D_WORLD_ABS_MAX);
}

/// @brief Clamp aspect ratio away from zero. Values ≤ 1e-6 (including negatives and
/// zero) fall back to 1.0 so `build_perspective`'s `f / aspect` divide never produces
/// infinity or a negative scale. Used at every projection-building site.
/// @param aspect Candidate width-to-height ratio.
/// @return Finite positive aspect capped at `CAMERA3D_ASPECT_MAX`, or 1 for invalid input.
static double sanitize_aspect(double aspect) {
    if (!isfinite(aspect) || aspect <= 1e-6)
        return 1.0;
    return aspect > CAMERA3D_ASPECT_MAX ? CAMERA3D_ASPECT_MAX : aspect;
}

/// @brief Guard the near/far clip planes against degenerate and low-precision configurations.
/// @details Forces near >= 0.1, keeps far beyond near, clamps the absolute clip range, and caps
///   the far/near ratio. A huge ratio leaves too few depth-buffer bits for mid-range triangles,
///   which shows up as coplanar flicker and unstable CPU/GPU visibility disagreement.
/// @param[in,out] near_val Near-plane distance to sanitize.
/// @param[in,out] far_val Far-plane distance to sanitize relative to @p near_val.
static void sanitize_clip_planes(double *near_val, double *far_val) {
    if (!near_val || !far_val)
        return;
    if (!isfinite(*near_val) || *near_val < 0.1)
        *near_val = 0.1;
    if (*near_val > CAMERA3D_CLIP_MAX)
        *near_val = 0.1;
    if (!isfinite(*far_val) || *far_val <= *near_val + 1e-4)
        *far_val = *near_val + 1000.0;
    if (*far_val > CAMERA3D_CLIP_MAX)
        *far_val = CAMERA3D_CLIP_MAX;
    if (*far_val <= *near_val + 1e-4) {
        *near_val = 0.1;
        *far_val = 1000.1;
    }
    if (*far_val / *near_val > CAMERA3D_MAX_FAR_NEAR_RATIO) {
        double ratio_near = *far_val / CAMERA3D_MAX_FAR_NEAR_RATIO;
        if (isfinite(ratio_near) && ratio_near > *near_val)
            *near_val = ratio_near;
    }
}

/// @brief Clamp vertical FOV to the usable `[1°, 179°]` range. Below 1° the perspective
/// `tan(fov/2)` divisor approaches zero and the projection blows up; above 179° the
/// forward basis effectively flips. Real-world content lives in ~30–100° so this clamp
/// only kicks in for malformed input.
/// @param fov_deg Candidate field of view in degrees.
/// @return Finite vertical field of view in `[1, 179]`, using 60 for non-finite input.
static double sanitize_fov(double fov_deg) {
    if (!isfinite(fov_deg))
        return 60.0;
    if (fov_deg < 1.0)
        return 1.0;
    if (fov_deg > 179.0)
        return 179.0;
    return fov_deg;
}

/// @brief Convert a horizontal perspective field-of-view to the vertical FOV stored by Camera3D.
/// @details The renderer's projection matrix is parameterized by vertical FOV, but game-facing
///   camera tuning is often authored as horizontal FOV because that stays visually stable across
///   common widescreen window sizes. The conversion preserves the requested horizontal aperture
///   for @p aspect using `2 * atan(tan(hfov/2) / aspect)`, with both the input horizontal FOV and
///   the resulting vertical FOV passed through the same safety clamps as regular Camera3D FOVs.
///   Non-finite or degenerate aspect values fall back to 1.0 through `sanitize_aspect`.
/// @param horizontal_fov_deg Requested horizontal aperture in degrees.
/// @param aspect Width-to-height aspect ratio used for conversion.
/// @return Sanitized vertical field of view in degrees.
static double camera_vertical_fov_from_horizontal(double horizontal_fov_deg, double aspect) {
    double horizontal_rad;
    double vertical_rad;

    horizontal_fov_deg = sanitize_fov(horizontal_fov_deg);
    aspect = sanitize_aspect(aspect);
    horizontal_rad = horizontal_fov_deg * (M_PI / 180.0);
    vertical_rad = 2.0 * atan(tan(horizontal_rad * 0.5) / aspect);
    return sanitize_fov(vertical_rad * (180.0 / M_PI));
}

/// @brief Clamp orthographic view-volume half-size away from zero — same logic as
/// `sanitize_aspect` but for the ortho height parameter. Prevents a zero-size ortho
/// projection from collapsing into a divide-by-zero during matrix construction.
/// @param size Candidate orthographic half-height.
/// @return Finite positive half-height capped at `CAMERA3D_ORTHO_SIZE_MAX`, or 1 when invalid.
static double sanitize_ortho_size(double size) {
    if (!isfinite(size) || size <= 1e-6)
        return 1.0;
    return size > CAMERA3D_ORTHO_SIZE_MAX ? CAMERA3D_ORTHO_SIZE_MAX : size;
}

/// @brief Wrap an angle in degrees into (−180, 180]; non-finite input maps to 0.
/// @param degrees Candidate angle.
/// @return Equivalent finite wrapped angle, or zero for non-finite input.
static double camera_wrap_degrees(double degrees) {
    if (!isfinite(degrees))
        return 0.0;
    degrees = fmod(degrees, 360.0);
    if (!isfinite(degrees))
        return 0.0;
    if (degrees > 180.0)
        degrees -= 360.0;
    if (degrees < -180.0)
        degrees += 360.0;
    return degrees;
}

/// @brief Clamp a pitch angle to [−89°, 89°] to avoid look-direction gimbal flip (non-finite → 0).
/// @param pitch Candidate pitch in degrees.
/// @return Finite pitch clamped to `[-89, 89]`.
static double camera_clamp_pitch(double pitch) {
    pitch = finite_or(pitch, 0.0);
    if (pitch > 89.0)
        return 89.0;
    if (pitch < -89.0)
        return -89.0;
    return pitch;
}

/// @brief Frame-rate-independent smoothing factor `1 − exp(−speed·dt)`, clamped to [0, 1];
///   saturates to 1 for large or non-finite exponents.
/// @param speed Non-negative convergence rate in inverse seconds.
/// @param dt Non-negative elapsed time in seconds.
/// @return Finite interpolation factor in `[0, 1]`.
static double camera_damping_factor(double speed, double dt) {
    double exponent;
    speed = sanitize_nonnegative(speed, 0.0);
    dt = sanitize_nonnegative(dt, 0.0);
    exponent = speed * dt;
    if (!isfinite(exponent))
        return 1.0;
    if (exponent >= CAMERA3D_DAMPING_EXP_MAX)
        return 1.0;
    return 1.0 - exp(-exponent);
}

/// @brief Compute the effective eye position as the camera eye plus its shake offset,
///   each lane finite-guarded and clamped to the world bound; origin when `cam` is NULL.
/// @param cam Optional borrowed camera.
/// @param[out] out_eye Three-element destination; a null destination is a no-op.
static void camera_eye_with_shake(const rt_camera3d *cam, double out_eye[3]) {
    static const double fallback_eye[3] = {0.0, 0.0, 0.0};
    if (!out_eye)
        return;
    if (!cam) {
        out_eye[0] = out_eye[1] = out_eye[2] = 0.0;
        return;
    }
    out_eye[0] =
        clamp_abs_or(finite_or(cam->eye[0], fallback_eye[0]) + finite_or(cam->shake_offset[0], 0.0),
                     fallback_eye[0],
                     CAMERA3D_WORLD_ABS_MAX);
    out_eye[1] =
        clamp_abs_or(finite_or(cam->eye[1], fallback_eye[1]) + finite_or(cam->shake_offset[1], 0.0),
                     fallback_eye[1],
                     CAMERA3D_WORLD_ABS_MAX);
    out_eye[2] =
        clamp_abs_or(finite_or(cam->eye[2], fallback_eye[2]) + finite_or(cam->shake_offset[2], 0.0),
                     fallback_eye[2],
                     CAMERA3D_WORLD_ABS_MAX);
}

/// @brief Replace any non-finite or out-of-range lane of the camera's eye position with a
///   safe fallback (origin), keeping later view-matrix construction well-defined.
/// @param[in,out] cam Borrowed camera whose logical eye is sanitized; may be null.
static void camera_sanitize_eye(rt_camera3d *cam) {
    static const double fallback_eye[3] = {0.0, 0.0, 0.0};
    if (cam)
        sanitize_vec3(cam->eye, fallback_eye);
}

/// @brief Normalize the (`*x`,`*y`,`*z`) triple in place, reverting to the
///        fallback direction when any lane is non-finite or the vector length
///        is ~zero. Keeps camera basis vectors unit-length and well-defined.
/// @param[in,out] x Optional x component storage.
/// @param[in,out] y Optional y component storage.
/// @param[in,out] z Optional z component storage.
/// @param fallback_x Fallback x component.
/// @param fallback_y Fallback y component.
/// @param fallback_z Fallback z component.
static void camera_normalize_vec3_or(
    double *x, double *y, double *z, double fallback_x, double fallback_y, double fallback_z) {
    double vx = clamp_abs_or(x ? *x : fallback_x, fallback_x, CAMERA3D_WORLD_ABS_MAX);
    double vy = clamp_abs_or(y ? *y : fallback_y, fallback_y, CAMERA3D_WORLD_ABS_MAX);
    double vz = clamp_abs_or(z ? *z : fallback_z, fallback_z, CAMERA3D_WORLD_ABS_MAX);
    double max_component = fmax(fabs(vx), fmax(fabs(vy), fabs(vz)));
    if (!isfinite(max_component) || max_component <= 1e-12) {
        vx = fallback_x;
        vy = fallback_y;
        vz = fallback_z;
    } else {
        double sx = vx / max_component;
        double sy = vy / max_component;
        double sz = vz / max_component;
        double len = sqrt(sx * sx + sy * sy + sz * sz);
        if (!isfinite(len) || len <= 1e-12) {
            vx = fallback_x;
            vy = fallback_y;
            vz = fallback_z;
        } else {
            vx = sx / len;
            vy = sy / len;
            vz = sz / len;
        }
    }
    if (x)
        *x = vx;
    if (y)
        *y = vy;
    if (z)
        *z = vz;
}

/// @brief Build an OpenGL-style perspective projection matrix into `m` (row-major).
/// `f = 1 / tan(fov/2)` sets vertical scale; horizontal divides by aspect. Writes depth
/// in the `[-1, 1]` NDC convention. With Zanna's row-major, column-vector transform
/// path, `m[14] = -1` makes clip.w = -view.z for perspective division. Zeroes the matrix first so
/// unused slots stay at 0 — callers can assume a fully initialised 4×4.
/// @param[out] m Writable 16-element projection matrix.
/// @param fov_deg Sanitized vertical field of view in degrees.
/// @param aspect Sanitized positive width-to-height ratio.
/// @param near_val Sanitized near-plane distance.
/// @param far_val Sanitized far-plane distance greater than @p near_val.
static void build_perspective(
    double *m, double fov_deg, double aspect, double near_val, double far_val) {
    memset(m, 0, 16 * sizeof(double));
    double fov_rad = fov_deg * (M_PI / 180.0);
    double f = 1.0 / tan(fov_rad * 0.5);
    double nf = 1.0 / (near_val - far_val);

    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_val + near_val) * nf; /* OpenGL NDC: Z in [-1,1] */
    m[11] = 2.0 * far_val * near_val * nf;
    m[14] = -1.0;
    /* m[15] = 0 */
    camera_finish_matrix(m);
}

/// @brief Build a right-handed look-at view matrix into `m`. Forward = normalize(eye -
/// target); right = normalize(cross(up, forward)); true-up = cross(forward, right).
/// Handles two degenerate cases: (1) eye == target (flen ≈ 0) falls back to +Z forward
/// while preserving camera translation — avoids producing an identity with the
/// translation stripped; (2) up parallel to forward (rlen ≈ 0) picks an orthogonal
/// fallback axis based on which world axis dominates the forward vector. Final
/// fallback is the world +X basis if both attempts collapse. Matches
/// `rt_mat4_look_at` bit-for-bit so the Canvas3D projection and Camera3D forward
/// vectors stay in lockstep.
/// @param[out] m Writable 16-element view matrix.
/// @param eye Optional borrowed three-element camera position.
/// @param target Optional borrowed three-element look target.
/// @param up Optional borrowed three-element up hint.
static void build_look_at(double *m, const double *eye, const double *target, const double *up) {
    static const double fallback_eye[3] = {0.0, 0.0, 0.0};
    static const double fallback_target[3] = {0.0, 0.0, -1.0};
    static const double fallback_up_vec[3] = {0.0, 1.0, 0.0};
    double clean_eye[3] = {eye ? eye[0] : fallback_eye[0],
                           eye ? eye[1] : fallback_eye[1],
                           eye ? eye[2] : fallback_eye[2]};
    double clean_target[3] = {target ? target[0] : fallback_target[0],
                              target ? target[1] : fallback_target[1],
                              target ? target[2] : fallback_target[2]};
    double clean_up[3] = {up ? up[0] : fallback_up_vec[0],
                          up ? up[1] : fallback_up_vec[1],
                          up ? up[2] : fallback_up_vec[2]};

    sanitize_vec3(clean_eye, fallback_eye);
    sanitize_vec3(clean_target, fallback_target);
    sanitize_vec3(clean_up, fallback_up_vec);
    eye = clean_eye;
    target = clean_target;
    up = clean_up;

    /* Forward = normalize(eye - target) — right-handed.
     * If eye == target (flen ≈ 0), fall back to the default forward axis
     * while preserving the camera translation. */
    double fx = eye[0] - target[0];
    double fy = eye[1] - target[1];
    double fz = eye[2] - target[2];
    camera_normalize_vec3_or(&fx, &fy, &fz, 0.0, 0.0, 1.0);
    if (fabs(fx) + fabs(fy) + fabs(fz) <= 1e-12) {
        fx = 0.0;
        fy = 0.0;
        fz = 1.0;
    }

    /* Right = normalize(cross(up, forward)) */
    double rx = up[1] * fz - up[2] * fy;
    double ry = up[2] * fx - up[0] * fz;
    double rz = up[0] * fy - up[1] * fx;
    double rlen_probe = fmax(fabs(rx), fmax(fabs(ry), fabs(rz)));
    if (!isfinite(rlen_probe) || rlen_probe <= 1e-12) {
        /* When forward is near-vertical (|fy| > 0.99) the world-Y up hint is
         * degenerate, so fall back to world +Z; otherwise use world +Y. */
        const double fallback_up[3] = {
            0.0, fabs(fy) > 0.99 ? 0.0 : 1.0, fabs(fy) > 0.99 ? 1.0 : 0.0};
        rx = fallback_up[1] * fz - fallback_up[2] * fy;
        ry = fallback_up[2] * fx - fallback_up[0] * fz;
        rz = fallback_up[0] * fy - fallback_up[1] * fx;
    }
    camera_normalize_vec3_or(&rx, &ry, &rz, 1.0, 0.0, 0.0);

    /* True up = cross(forward, right) */
    double ux = fy * rz - fz * ry;
    double uy = fz * rx - fx * rz;
    double uz = fx * ry - fy * rx;
    camera_normalize_vec3_or(&ux, &uy, &uz, 0.0, 1.0, 0.0);

    memset(m, 0, 16 * sizeof(double));
    m[0] = rx;
    m[1] = ry;
    m[2] = rz;
    m[3] = -(rx * eye[0] + ry * eye[1] + rz * eye[2]);
    m[4] = ux;
    m[5] = uy;
    m[6] = uz;
    m[7] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    m[8] = fx;
    m[9] = fy;
    m[10] = fz;
    m[11] = -(fx * eye[0] + fy * eye[1] + fz * eye[2]);
    m[15] = 1.0;
    camera_finish_matrix(m);
}

/// @brief Recompute the camera's projection matrix from current FOV / aspect / clip / ortho size.
/// @details Called whenever any projection input changes. Routes to
///          `build_ortho` or `build_perspective` based on `cam->is_ortho`,
///          re-sanitising the inputs first so callers can pass any value
///          and still produce a valid projection (degenerate aspects or
///          clip planes are clamped).
/// @param[in,out] cam Borrowed camera whose retained projection inputs and matrix are updated.
static void rebuild_projection(rt_camera3d *cam) {
    if (!cam)
        return;
    cam->aspect = sanitize_aspect(cam->aspect);
    sanitize_clip_planes(&cam->near_plane, &cam->far_plane);
    if (cam->is_ortho) {
        double half_h = sanitize_ortho_size(cam->ortho_size);
        double half_w = half_h * cam->aspect;
        cam->ortho_size = half_h;
        build_ortho(
            cam->projection, -half_w, half_w, -half_h, half_h, cam->near_plane, cam->far_plane);
    } else {
        cam->fov = sanitize_fov(cam->fov);
        build_perspective(cam->projection, cam->fov, cam->aspect, cam->near_plane, cam->far_plane);
    }
}

/// @brief Update the camera's aspect ratio to match the active output and rebuild projection.
/// @details Called by Canvas3D / RenderTarget3D `Begin` when the render
///          surface's dimensions change (window resize or render-target
///          rebind). No-op when the new aspect matches within 1e-9, so
///          repeated calls with the same dimensions don't churn the
///          projection matrix.
/// @param obj Borrowed heap or stack camera handle.
/// @param aspect Output width-to-height ratio, sanitized before comparison.
void rt_camera3d_sync_render_aspect(void *obj, double aspect) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    double sanitized_aspect;

    if (!cam)
        return;
    sanitized_aspect = sanitize_aspect(aspect);
    if (fabs(cam->aspect - sanitized_aspect) <= 1e-9)
        return;
    cam->aspect = sanitized_aspect;
    rebuild_projection(cam);
}

/// @brief Build the projection matrix a render pass should use for the given aspect.
/// @details Unlike `rt_camera3d_sync_render_aspect`, this does not mutate the
///          camera object. Canvas3D uses it so the same camera can be reused
///          across outputs with different aspect ratios without permanently
///          rewriting `cam->aspect` / `cam->projection`.
/// @param obj Borrowed heap or stack camera handle.
/// @param aspect_override Positive render-surface aspect, or a non-positive value to use the
///        camera's retained aspect.
/// @param[out] out_projection Writable 16-float row-major matrix. Invalid double-to-float values
///        trap and produce identity.
void rt_camera3d_get_render_projection(void *obj, double aspect_override, float *out_projection) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    double projection[16];
    double aspect;
    double near_plane;
    double far_plane;

    if (!cam || !out_projection)
        return;

    aspect = sanitize_aspect(aspect_override > 1e-6 ? aspect_override : cam->aspect);
    near_plane = cam->near_plane;
    far_plane = cam->far_plane;
    sanitize_clip_planes(&near_plane, &far_plane);

    if (cam->is_ortho) {
        double half_h = sanitize_ortho_size(cam->ortho_size);
        double half_w = half_h * aspect;
        build_ortho(projection, -half_w, half_w, -half_h, half_h, near_plane, far_plane);
    } else {
        build_perspective(projection, sanitize_fov(cam->fov), aspect, near_plane, far_plane);
    }

    for (int i = 0; i < 16; i++) {
        if (!camera_value_fits_float(projection[i])) {
            rt_trap("Camera3D: projection matrix contains values outside float range");
            memset(out_projection, 0, sizeof(float) * 16);
            out_projection[0] = out_projection[5] = out_projection[10] = out_projection[15] = 1.0f;
            return;
        }
        out_projection[i] = (float)projection[i];
    }
}

/// @brief Recover the FPS yaw/pitch state from the current view matrix.
/// @details Called after `LookAt` or other view-replacing ops so the next
///          FPS-style mouse delta starts from the correct heading. Forward
///          is row 2 of the view matrix (negated because view stores the
///          inverse camera basis), so yaw = atan2(fx, -fz) and pitch =
///          asin(fy). Pitch is clamped to ±89° so the next rebuild can't
///          produce a degenerate look-at where forward becomes parallel
///          to the world up vector.
/// @param[in,out] cam Borrowed camera whose cached FPS angles are updated.
static void camera_sync_fps_angles_from_view(rt_camera3d *cam) {
    double fx;
    double fy;
    double fz;

    if (!cam)
        return;
    fx = finite_or(-cam->view[8], 0.0);
    fy = finite_or(-cam->view[9], 0.0);
    fz = finite_or(-cam->view[10], -1.0);
    camera_normalize_vec3_or(&fx, &fy, &fz, 0.0, 0.0, -1.0);
    cam->fps_yaw = camera_wrap_degrees(atan2(fx, -fz) * (180.0 / M_PI));
    cam->fps_pitch = camera_clamp_pitch(asin(fmax(-1.0, fmin(1.0, fy))) * (180.0 / M_PI));
}

/// @brief Rebuild the view matrix from the cached FPS yaw/pitch and eye position.
/// @details Constructs a target one unit ahead of the eye along the
///          spherical-coordinate forward (yaw rotates in the XZ plane,
///          pitch tilts up/down) and feeds it to `build_look_at`. World
///          up is fixed +Y. Called every frame an FPS controller mutates
///          yaw/pitch via `RotateYaw` / `RotatePitch` / `LookDelta`.
/// @param[in,out] cam Borrowed camera whose eye, angles, and view are sanitized or rebuilt.
static void camera_rebuild_fps_view(rt_camera3d *cam) {
    double yaw_rad;
    double pitch_rad;
    double cp;
    double target[3];
    double up[3] = {0.0, 1.0, 0.0};

    if (!cam)
        return;
    camera_sanitize_eye(cam);
    cam->fps_yaw = camera_wrap_degrees(cam->fps_yaw);
    cam->fps_pitch = camera_clamp_pitch(cam->fps_pitch);
    yaw_rad = cam->fps_yaw * (M_PI / 180.0);
    pitch_rad = cam->fps_pitch * (M_PI / 180.0);
    cp = cos(pitch_rad);
    target[0] = cam->eye[0] + sin(yaw_rad) * cp;
    target[1] = cam->eye[1] + sin(pitch_rad);
    target[2] = cam->eye[2] - cos(yaw_rad) * cp;
    build_look_at(cam->view, cam->eye, target, up);
}

/// @brief Re-derive the view matrix using the eye plus the current shake offset.
/// @details Reads forward from the existing view (row 2 negated), shifts
///          eye by the current shake delta, then builds a fresh look-at
///          targeting eye+forward so heading stays identical — only
///          translation jitters. The shake offset itself is updated
///          per-frame by `apply_shake`.
/// @param[in,out] cam Borrowed camera whose view matrix is rebuilt from current shake state.
static void camera_apply_shake_to_view(rt_camera3d *cam) {
    double forward[3];
    double eye[3];
    double target[3];
    double up[3];

    if (!cam)
        return;

    forward[0] = finite_or(-cam->view[8], 0.0);
    forward[1] = finite_or(-cam->view[9], 0.0);
    forward[2] = finite_or(-cam->view[10], -1.0);
    camera_normalize_vec3_or(&forward[0], &forward[1], &forward[2], 0.0, 0.0, -1.0);
    up[0] = finite_or(cam->view[4], 0.0);
    up[1] = finite_or(cam->view[5], 1.0);
    up[2] = finite_or(cam->view[6], 0.0);
    camera_normalize_vec3_or(&up[0], &up[1], &up[2], 0.0, 1.0, 0.0);
    camera_eye_with_shake(cam, eye);
    target[0] = eye[0] + forward[0];
    target[1] = eye[1] + forward[1];
    target[2] = eye[2] + forward[2];
    build_look_at(cam->view, eye, target, up);
}

/// @brief Create a perspective camera with the given field of view and clipping planes.
/// @details Uses a right-handed coordinate system (+X right, +Y up, +Z toward viewer)
///          with OpenGL NDC convention (Z in [-1, 1]). The camera starts at the
///          origin looking along -Z. View and projection matrices are stored as
///          double[16] row-major, matching Mat4 conventions.
/// @param fov      Vertical field of view in degrees.
/// @param aspect   Width/height aspect ratio.
/// @param near_val Near clipping plane distance.
/// @param far_val  Far clipping plane distance.
/// @return Opaque camera handle, or NULL on failure.
void *rt_camera3d_new(double fov, double aspect, double near_val, double far_val) {
    rt_camera3d *cam =
        (rt_camera3d *)rt_obj_new_i64(RT_G3D_CAMERA3D_CLASS_ID, (int64_t)sizeof(rt_camera3d));
    if (!cam) {
        rt_trap("Camera3D.New: memory allocation failed");
        return NULL;
    }
    {
        rt_camera3d zero = {0};
        *cam = zero;
    }
    cam->vptr = NULL;
    cam->identity_serial = rt_g3d_next_identity_serial();
    cam->fov = sanitize_fov(fov);
    cam->aspect = sanitize_aspect(aspect);
    cam->near_plane = near_val;
    cam->far_plane = far_val;

    /* Default identity view (camera at origin looking along -Z) */
    memset(cam->view, 0, 16 * sizeof(double));
    cam->view[0] = cam->view[5] = cam->view[10] = cam->view[15] = 1.0;
    cam->eye[0] = cam->eye[1] = cam->eye[2] = 0.0;
    cam->fps_yaw = 0.0;
    cam->fps_pitch = 0.0;
    cam->shake_intensity = 0.0;
    cam->shake_duration = 0.0;
    cam->shake_decay = 5.0;
    cam->shake_offset[0] = cam->shake_offset[1] = cam->shake_offset[2] = 0.0;
    cam->shake_seed = camera3d_next_shake_seed();
    cam->last_shake_update_token = 0;
    cam->is_ortho = 0;
    cam->ortho_size = 10.0;
    cam->pick_cache_valid = 0;
    rebuild_projection(cam);

    return cam;
}

/// @brief Create a perspective camera from a horizontal field of view.
/// @details This is a convenience constructor for first-person and vehicle cameras where an
///   author-provided vertical FOV can look too wide on 16:9 and ultrawide windows. The camera still
///   stores and reports vertical FOV internally; @p horizontal_fov is converted using @p aspect
///   before delegating to `rt_camera3d_new`, so all clip-plane sanitization and default camera
///   orientation remain identical to the standard constructor.
/// @param horizontal_fov Horizontal field of view in degrees.
/// @param aspect         Width/height aspect ratio used for the conversion.
/// @param near_val       Near clipping plane distance.
/// @param far_val        Far clipping plane distance.
/// @return Opaque camera handle, or NULL on failure.
void *rt_camera3d_new_horizontal_fov(double horizontal_fov,
                                     double aspect,
                                     double near_val,
                                     double far_val) {
    return rt_camera3d_new(
        camera_vertical_fov_from_horizontal(horizontal_fov, aspect), aspect, near_val, far_val);
}

/// @brief Construct a row-major orthographic projection matrix into `m`.
/// @details Maps the axis-aligned view volume `[left,right] × [bottom,top] ×
///   [near,far]` to the three-dimensional OpenGL-style clip cube `[-1, 1]`. Z is flipped
///   because view space uses -Z forward while NDC uses +Z forward, giving
///   `-2 / (far - near)` on the diagonal. The translation column centers the
///   volume on the origin: `(r+l)/(r-l)` is negated to move "left" to -1.
///   Degenerate inputs (any axis spanning less than 1e-12) fall back to
///   identity rather than generating NaN/Inf, so downstream matrix multiplies
///   stay finite.
/// @param[out] m Writable 16-element matrix.
/// @param left Left view-volume coordinate.
/// @param right Right view-volume coordinate.
/// @param bottom Bottom view-volume coordinate.
/// @param top Top view-volume coordinate.
/// @param near_val Near-plane distance.
/// @param far_val Far-plane distance.
static void build_ortho(double *m,
                        double left,
                        double right,
                        double bottom,
                        double top,
                        double near_val,
                        double far_val) {
    memset(m, 0, 16 * sizeof(double));
    double rl = right - left;
    double tb = top - bottom;
    double fn = far_val - near_val;
    if (fabs(rl) < 1e-12 || fabs(tb) < 1e-12 || fabs(fn) < 1e-12)
        goto finish;
    m[0] = 2.0 / rl;
    m[5] = 2.0 / tb;
    m[10] = -2.0 / fn;
    m[3] = -(right + left) / rl;
    m[7] = -(top + bottom) / tb;
    m[11] = -(far_val + near_val) / fn;
    m[15] = 1.0;
finish:
    camera_finish_matrix(m);
}

/// @brief Create an orthographic camera (parallel projection, no perspective foreshortening).
/// @details Useful for 2D games rendered in a 3D pipeline, UI overlays, shadow maps,
///          and minimap views. The size parameter controls the vertical extent.
/// @param size     Half-height of the orthographic view volume.
/// @param aspect   Width/height aspect ratio.
/// @param near_val Near clipping plane distance.
/// @param far_val  Far clipping plane distance.
/// @return Opaque camera handle, or NULL on failure.
void *rt_camera3d_new_ortho(double size, double aspect, double near_val, double far_val) {
    rt_camera3d *cam =
        (rt_camera3d *)rt_obj_new_i64(RT_G3D_CAMERA3D_CLASS_ID, (int64_t)sizeof(rt_camera3d));
    if (!cam) {
        rt_trap("Camera3D.NewOrtho: memory allocation failed");
        return NULL;
    }
    {
        rt_camera3d zero = {0};
        *cam = zero;
    }
    cam->vptr = NULL;
    cam->fov = 0;
    cam->aspect = sanitize_aspect(aspect);
    cam->near_plane = near_val;
    cam->far_plane = far_val;
    cam->is_ortho = 1;
    cam->ortho_size = sanitize_ortho_size(size);

    /* Default identity view */
    memset(cam->view, 0, 16 * sizeof(double));
    cam->view[0] = cam->view[5] = cam->view[10] = cam->view[15] = 1.0;
    cam->eye[0] = cam->eye[1] = cam->eye[2] = 0.0;
    cam->fps_yaw = 0.0;
    cam->fps_pitch = 0.0;
    cam->shake_intensity = 0.0;
    cam->shake_duration = 0.0;
    cam->shake_decay = 5.0;
    cam->shake_offset[0] = cam->shake_offset[1] = cam->shake_offset[2] = 0.0;
    cam->shake_seed = camera3d_next_shake_seed();
    cam->last_shake_update_token = 0;
    rebuild_projection(cam);

    return cam;
}

/// @brief Return non-zero when the camera uses an orthographic projection.
/// @details Lets renderers branch on perspective vs ortho without poking
///          into private struct fields. Returns 0 for null obj or for
///          perspective cameras built via `rt_camera3d_new`.
/// @param obj Borrowed heap or stack camera handle.
/// @return `1` for orthographic projection; otherwise `0`.
int8_t rt_camera3d_is_ortho(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    return cam && cam->is_ortho ? 1 : 0;
}

/// @brief Switch a camera between perspective and orthographic projection.
/// @details Both projection parameter sets remain retained, so a perspective camera can be
///          toggled to orthographic and back without losing its authored FOV. Cameras originally
///          constructed as orthographic receive a safe 60-degree perspective FOV on their first
///          switch because that constructor historically stored zero in the unused FOV field.
/// @param obj Borrowed heap or stack camera handle.
/// @param is_ortho Nonzero to activate orthographic projection; zero for perspective.
void rt_camera3d_set_is_ortho(void *obj, int8_t is_ortho) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    int8_t next;
    if (!cam)
        return;
    next = is_ortho ? 1 : 0;
    if (cam->is_ortho == next)
        return;
    cam->is_ortho = next;
    cam->ortho_size = sanitize_ortho_size(cam->ortho_size);
    if (!next && (!isfinite(cam->fov) || cam->fov < 1.0 || cam->fov > 179.0))
        cam->fov = 60.0;
    rebuild_projection(cam);
}

/// @brief Read the camera's retained orthographic half-height through the native sanitizer.
/// @param obj Borrowed heap or stack camera handle.
/// @return Sanitized positive retained half-height, or zero for an invalid camera.
double rt_camera3d_get_ortho_size(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    return cam ? sanitize_ortho_size(cam->ortho_size) : 0.0;
}

/// @brief Change the retained orthographic half-height and rebuild an active ortho projection.
/// @param obj Borrowed heap or stack camera handle.
/// @param size Requested half-height, sanitized to a finite positive range.
void rt_camera3d_set_ortho_size(void *obj, double size) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->ortho_size = sanitize_ortho_size(size);
    if (cam->is_ortho)
        rebuild_projection(cam);
}

/// @brief Deep-copy a Camera3D for an independently mutable scene instance.
/// @details Camera3D owns no nested heap allocations, so copying its scalar and fixed-array state
///          after allocating a correctly registered destination is a complete clone. The
///          destination's runtime header lives outside this payload and is therefore unaffected.
/// @param obj Borrowed source heap or stack camera.
/// @return Owned independent camera with identical state, or null for invalid input/allocation
///         failure.
void *rt_camera3d_clone(void *obj) {
    rt_camera3d *source = rt_camera3d_checked_or_stack(obj);
    rt_camera3d *copy;
    void *copy_vptr;
    if (!source)
        return NULL;
    copy = (rt_camera3d *)(source->is_ortho ? rt_camera3d_new_ortho(source->ortho_size,
                                                                    source->aspect,
                                                                    source->near_plane,
                                                                    source->far_plane)
                                            : rt_camera3d_new(source->fov,
                                                              source->aspect,
                                                              source->near_plane,
                                                              source->far_plane));
    if (!copy)
        return NULL;
    copy_vptr = copy->vptr;
    memcpy(copy, source, sizeof(*copy));
    copy->vptr = copy_vptr;
    return copy;
}

/// @brief Core look-at: position the camera at @p eye_in aiming at @p target_in with up @p up_in.
/// @details Sanitizes inputs and recomputes the camera basis/view matrix; shared by the Vec3 and
///          scalar-component entry points.
/// @param[in,out] cam Borrowed camera whose eye, view, FPS angles, and shaken view are updated.
/// @param eye_in Optional borrowed three-element logical eye.
/// @param target_in Optional borrowed three-element target.
/// @param up_in Optional borrowed three-element up hint.
static void camera3d_look_at_values(rt_camera3d *cam,
                                    const double eye_in[3],
                                    const double target_in[3],
                                    const double up_in[3]) {
    static const double fallback_eye[3] = {0.0, 0.0, 0.0};
    static const double fallback_target[3] = {0.0, 0.0, -1.0};
    static const double fallback_up[3] = {0.0, 1.0, 0.0};
    double eye[3] = {eye_in ? eye_in[0] : 0.0, eye_in ? eye_in[1] : 0.0, eye_in ? eye_in[2] : 0.0};
    double target[3] = {target_in ? target_in[0] : 0.0,
                        target_in ? target_in[1] : 0.0,
                        target_in ? target_in[2] : -1.0};
    double up[3] = {up_in ? up_in[0] : 0.0, up_in ? up_in[1] : 1.0, up_in ? up_in[2] : 0.0};

    if (!cam)
        return;
    sanitize_vec3(eye, fallback_eye);
    sanitize_vec3(target, fallback_target);
    sanitize_vec3(up, fallback_up);

    cam->eye[0] = eye[0];
    cam->eye[1] = eye[1];
    cam->eye[2] = eye[2];

    build_look_at(cam->view, eye, target, up);
    camera_sync_fps_angles_from_view(cam);
    camera_apply_shake_to_view(cam);
}

/// @brief Position the camera and orient it to look at a target point.
/// @details Builds the view matrix using the standard look-at construction:
///          forward = normalize(eye - target), right = cross(up, forward),
///          true_up = cross(forward, right). Uses right-handed coordinates.
/// @param obj Borrowed heap or stack camera handle.
/// @param eye_v Borrowed `Vec3` eye position.
/// @param target_v Borrowed `Vec3` target position.
/// @param up_v Borrowed `Vec3` up hint.
void rt_camera3d_look_at(void *obj, void *eye_v, void *target_v, void *up_v) {
    if (!obj)
        return;
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    if (!rt_g3d_is_vec3(eye_v) || !rt_g3d_is_vec3(target_v) || !rt_g3d_is_vec3(up_v)) {
        rt_trap("Camera3D.LookAt: eye, target, and up must be Vec3");
        return;
    }

    double eye[3] = {rt_vec3_x(eye_v), rt_vec3_y(eye_v), rt_vec3_z(eye_v)};
    double target[3] = {rt_vec3_x(target_v), rt_vec3_y(target_v), rt_vec3_z(target_v)};
    double up[3] = {rt_vec3_x(up_v), rt_vec3_y(up_v), rt_vec3_z(up_v)};
    camera3d_look_at_values(cam, eye, target, up);
}

/// @brief Aim the camera using scalar eye/target/up components (no Vec3 boxing required).
/// @param obj Borrowed heap or stack camera handle.
/// @param eye_x Eye x coordinate.
/// @param eye_y Eye y coordinate.
/// @param eye_z Eye z coordinate.
/// @param target_x Target x coordinate.
/// @param target_y Target y coordinate.
/// @param target_z Target z coordinate.
/// @param up_x Up-hint x component.
/// @param up_y Up-hint y component.
/// @param up_z Up-hint z component.
void rt_camera3d_look_at_components(void *obj,
                                    double eye_x,
                                    double eye_y,
                                    double eye_z,
                                    double target_x,
                                    double target_y,
                                    double target_z,
                                    double up_x,
                                    double up_y,
                                    double up_z) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    double eye[3] = {eye_x, eye_y, eye_z};
    double target[3] = {target_x, target_y, target_z};
    double up[3] = {up_x, up_y, up_z};
    camera3d_look_at_values(cam, eye, target, up);
}

/// @brief Core orbit: place the camera at (yaw, pitch, distance) around a target, then look at it.
/// @details Shared implementation behind the Vec3 and scalar-component orbit entry points; clamps
///          pitch to avoid gimbal flip at the poles.
/// @param[in,out] cam Borrowed camera to reposition and orient.
/// @param tx Target x coordinate.
/// @param ty Target y coordinate.
/// @param tz Target z coordinate.
/// @param distance Non-negative orbit radius.
/// @param yaw Horizontal angle in degrees, wrapped to the canonical range.
/// @param pitch Vertical angle in degrees, clamped to `[-89, 89]`.
static void camera3d_orbit_values(
    rt_camera3d *cam, double tx, double ty, double tz, double distance, double yaw, double pitch) {
    if (!cam)
        return;

    tx = clamp_abs_or(tx, 0.0, CAMERA3D_WORLD_ABS_MAX);
    ty = clamp_abs_or(ty, 0.0, CAMERA3D_WORLD_ABS_MAX);
    tz = clamp_abs_or(tz, 0.0, CAMERA3D_WORLD_ABS_MAX);
    distance = sanitize_nonnegative(distance, 0.0);
    yaw = camera_wrap_degrees(yaw);
    pitch = camera_clamp_pitch(pitch);

    double yaw_rad = yaw * (M_PI / 180.0);
    double pitch_rad = pitch * (M_PI / 180.0);
    double cp = cos(pitch_rad), sp = sin(pitch_rad);
    double cy = cos(yaw_rad), sy = sin(yaw_rad);

    double eye[3];
    eye[0] = tx + distance * cp * sy;
    eye[1] = ty + distance * sp;
    eye[2] = tz + distance * cp * cy;

    double up[3] = {0.0, 1.0, 0.0};
    double target[3] = {tx, ty, tz};

    cam->eye[0] = clamp_abs_or(eye[0], 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[1] = clamp_abs_or(eye[1], 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[2] = clamp_abs_or(eye[2], 0.0, CAMERA3D_WORLD_ABS_MAX);
    eye[0] = cam->eye[0];
    eye[1] = cam->eye[1];
    eye[2] = cam->eye[2];

    build_look_at(cam->view, eye, target, up);
    camera_sync_fps_angles_from_view(cam);
    camera_apply_shake_to_view(cam);
}

/// @brief Position the camera on a spherical orbit around a target point.
/// @details Computes eye position from spherical coordinates (yaw, pitch, distance)
///          relative to the target, then builds a look-at view matrix. Useful for
///          third-person cameras and object inspection views.
/// @param obj Borrowed heap or stack camera handle.
/// @param target_v Borrowed `Vec3` orbit center.
/// @param distance Non-negative orbit radius.
/// @param yaw Horizontal angle in degrees.
/// @param pitch Vertical angle in degrees.
void rt_camera3d_orbit(void *obj, void *target_v, double distance, double yaw, double pitch) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    if (!rt_g3d_is_vec3(target_v)) {
        rt_trap("Camera3D.Orbit: target must be Vec3");
        return;
    }

    camera3d_orbit_values(
        cam, rt_vec3_x(target_v), rt_vec3_y(target_v), rt_vec3_z(target_v), distance, yaw, pitch);
}

/// @brief Orbit the camera around a target using scalar components (no Vec3 boxing required).
/// @param obj Borrowed heap or stack camera handle.
/// @param target_x Orbit-center x coordinate.
/// @param target_y Orbit-center y coordinate.
/// @param target_z Orbit-center z coordinate.
/// @param distance Non-negative orbit radius.
/// @param yaw Horizontal angle in degrees.
/// @param pitch Vertical angle in degrees.
void rt_camera3d_orbit_components(void *obj,
                                  double target_x,
                                  double target_y,
                                  double target_z,
                                  double distance,
                                  double yaw,
                                  double pitch) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    camera3d_orbit_values(cam, target_x, target_y, target_z, distance, yaw, pitch);
}

/// @brief Get the vertical field of view in degrees.
/// @param obj Borrowed heap or stack camera handle.
/// @return Sanitized vertical FOV for a perspective camera; zero for orthographic/invalid cameras.
double rt_camera3d_get_fov(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return 0.0;
    return cam->is_ortho ? 0.0 : sanitize_fov(cam->fov);
}

/// @brief Change the field of view and rebuild the projection matrix.
/// @param obj Borrowed heap or stack perspective camera.
/// @param fov Requested vertical field of view in degrees, clamped to `[1, 179]`.
void rt_camera3d_set_fov(void *obj, double fov) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    if (cam->is_ortho)
        return;
    cam->fov = sanitize_fov(fov);
    rebuild_projection(cam);
}

/// @brief Set the retained perspective FOV without discarding it on an orthographic camera.
/// @details NodeAnimation3D uses this private path so an FBX camera can animate dormant
///          perspective parameters before a later step channel switches projection mode.
/// @param obj Borrowed heap or stack camera.
/// @param fov Requested retained vertical FOV in degrees.
void rt_camera3d_set_retained_fov(void *obj, double fov) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->fov = sanitize_fov(fov);
    if (!cam->is_ortho)
        rebuild_projection(cam);
}

/// @brief Set a perspective camera's FOV from a horizontal aperture in degrees.
/// @details Converts @p horizontal_fov using the camera's current aspect ratio and stores the
///   resulting vertical FOV, then rebuilds the projection immediately. Orthographic cameras ignore
///   the call, matching `rt_camera3d_set_fov`. Use this when user-facing tuning should describe
///   the horizontal view width rather than the vertical aperture used by the projection matrix.
/// @param obj Borrowed heap or stack perspective camera.
/// @param horizontal_fov Requested horizontal aperture in degrees.
void rt_camera3d_set_horizontal_fov(void *obj, double horizontal_fov) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    if (cam->is_ortho)
        return;
    cam->fov = camera_vertical_fov_from_horizontal(horizontal_fov, cam->aspect);
    rebuild_projection(cam);
}

/// @brief Read the near clip-plane distance.
/// @details Returns the sanitized effective plane used by Camera3D's projection matrix rather
/// than the raw caller-supplied field, so diagnostics see the same depth range as rendering.
/// @param obj Borrowed heap or stack camera.
/// @return Sanitized effective near distance, or zero for an invalid camera.
double rt_camera3d_get_near_plane(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return 0.0;
    double near_plane = cam->near_plane;
    double far_plane = cam->far_plane;
    sanitize_clip_planes(&near_plane, &far_plane);
    return near_plane;
}

/// @brief Read the effective near clip-plane distance used for projection and shadow splits.
/// @details Alias for `NearPlane`'s sanitized getter. Kept as a separate runtime entry point so
/// callers can explicitly request the render-effective value when debugging depth precision.
/// @param obj Borrowed heap or stack camera.
/// @return Sanitized effective near distance, or zero for an invalid camera.
double rt_camera3d_get_effective_near_plane(void *obj) {
    return rt_camera3d_get_near_plane(obj);
}

/// @brief Set the near clip-plane distance; planes are re-sanitized on rebuild.
/// @param obj Borrowed heap or stack camera.
/// @param near_plane Requested near distance; projection rebuilding enforces depth constraints.
void rt_camera3d_set_near_plane(void *obj, double near_plane) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->near_plane = near_plane;
    rebuild_projection(cam);
}

/// @brief Read the far clip-plane distance.
/// @details Returns the sanitized effective plane used by Camera3D's projection matrix rather
/// than the raw caller-supplied field, so diagnostics see the same depth range as rendering.
/// @param obj Borrowed heap or stack camera.
/// @return Sanitized effective far distance, or zero for an invalid camera.
double rt_camera3d_get_far_plane(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return 0.0;
    double near_plane = cam->near_plane;
    double far_plane = cam->far_plane;
    sanitize_clip_planes(&near_plane, &far_plane);
    return far_plane;
}

/// @brief Read the effective far clip-plane distance used for projection and shadow splits.
/// @details Alias for `FarPlane`'s sanitized getter. Exposed separately for code that wants to
/// display the actual render depth range after Camera3D's precision guardrails.
/// @param obj Borrowed heap or stack camera.
/// @return Sanitized effective far distance, or zero for an invalid camera.
double rt_camera3d_get_effective_far_plane(void *obj) {
    return rt_camera3d_get_far_plane(obj);
}

/// @brief Set the far clip-plane distance (e.g. to extend draw distance for a
///   large scene); planes are re-sanitized on rebuild.
/// @param obj Borrowed heap or stack camera.
/// @param far_plane Requested far distance; projection rebuilding enforces depth constraints.
void rt_camera3d_set_far_plane(void *obj, double far_plane) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->far_plane = far_plane;
    rebuild_projection(cam);
}

/// @brief Return the camera's eye position as a freshly allocated Vec3.
/// @details The eye is the *unshaken* position — shake offset is applied
///          per-frame to the view matrix but never mutates `cam->eye`,
///          so callers see the logical camera location (good for HUDs,
///          attaching audio listeners, raycasts from the player).
/// @param obj Borrowed heap or stack camera.
/// @return Owned `Vec3` containing the sanitized logical eye, or null for an invalid camera or
///         allocation failure.
void *rt_camera3d_get_position(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    return rt_vec3_new(clamp_abs_or(cam->eye[0], 0.0, CAMERA3D_WORLD_ABS_MAX),
                       clamp_abs_or(cam->eye[1], 0.0, CAMERA3D_WORLD_ABS_MAX),
                       clamp_abs_or(cam->eye[2], 0.0, CAMERA3D_WORLD_ABS_MAX));
}

/// @brief Read the camera's world position into @p x / @p y / @p z (returns 0 with no writes if
/// invalid).
/// @param obj Borrowed heap or stack camera.
/// @param[out] x Required x-coordinate destination, initialized to zero.
/// @param[out] y Required y-coordinate destination, initialized to zero.
/// @param[out] z Required z-coordinate destination, initialized to zero.
/// @return `1` when all destinations receive a valid logical eye; otherwise `0`.
int8_t rt_camera3d_get_position_components(void *obj, double *x, double *y, double *z) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (x)
        *x = 0.0;
    if (y)
        *y = 0.0;
    if (z)
        *z = 0.0;
    if (!cam || !x || !y || !z)
        return 0;
    *x = clamp_abs_or(cam->eye[0], 0.0, CAMERA3D_WORLD_ABS_MAX);
    *y = clamp_abs_or(cam->eye[1], 0.0, CAMERA3D_WORLD_ABS_MAX);
    *z = clamp_abs_or(cam->eye[2], 0.0, CAMERA3D_WORLD_ABS_MAX);
    return 1;
}

/// @brief Set the camera's eye position from raw components and rebuild the view matrix.
/// @param obj Borrowed heap or stack camera.
/// @param x Logical eye X coordinate.
/// @param y Logical eye Y coordinate.
/// @param z Logical eye Z coordinate.
void rt_camera3d_set_position_components(void *obj, double x, double y, double z) {
    double forward[3];
    double up[3];
    double target[3];

    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    forward[0] = finite_or(-cam->view[8], 0.0);
    forward[1] = finite_or(-cam->view[9], 0.0);
    forward[2] = finite_or(-cam->view[10], -1.0);
    camera_normalize_vec3_or(&forward[0], &forward[1], &forward[2], 0.0, 0.0, -1.0);
    up[0] = finite_or(cam->view[4], 0.0);
    up[1] = finite_or(cam->view[5], 1.0);
    up[2] = finite_or(cam->view[6], 0.0);
    camera_normalize_vec3_or(&up[0], &up[1], &up[2], 0.0, 1.0, 0.0);
    cam->eye[0] = clamp_abs_or(x, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[1] = clamp_abs_or(y, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[2] = clamp_abs_or(z, 0.0, CAMERA3D_WORLD_ABS_MAX);
    target[0] = cam->eye[0] + forward[0];
    target[1] = cam->eye[1] + forward[1];
    target[2] = cam->eye[2] + forward[2];
    build_look_at(cam->view, cam->eye, target, up);
    camera_apply_shake_to_view(cam);
}

/// @brief Set the camera's eye position and rebuild the view matrix.
/// @param obj Borrowed heap or stack camera.
/// @param pos Borrowed `Vec3` logical eye; invalid objects are ignored.
void rt_camera3d_set_position(void *obj, void *pos) {
    if (!rt_camera3d_checked_or_stack(obj) || !rt_g3d_is_vec3(pos))
        return;
    rt_camera3d_set_position_components(obj, rt_vec3_x(pos), rt_vec3_y(pos), rt_vec3_z(pos));
}

/// @brief Extract the world-space forward unit vector the camera is currently pointing
/// at. Read directly from the view matrix's third row (negated because the view looks
/// along -Z by convention). Caller owns the returned Vec3. Useful for "fire a ray from
/// the camera" behaviors and for gameplay that needs the camera facing independent of
/// its `look_at` target.
/// @param obj Borrowed heap or stack camera.
/// @return Owned normalized forward `Vec3`, or null for an invalid camera/allocation failure.
void *rt_camera3d_get_forward(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    double x = finite_or(-cam->view[8], 0.0);
    double y = finite_or(-cam->view[9], 0.0);
    double z = finite_or(-cam->view[10], -1.0);
    camera_normalize_vec3_or(&x, &y, &z, 0.0, 0.0, -1.0);
    return rt_vec3_new(x, y, z);
}

/// @brief Extract the world-space right unit vector (the camera's screen-right axis).
/// Read directly from the view matrix's first row. Used together with forward and up
/// to build camera-relative movement (strafing, mouse-look yaw, etc.). Caller owns the
/// returned Vec3.
/// @param obj Borrowed heap or stack camera.
/// @return Owned normalized right `Vec3`, or null for an invalid camera/allocation failure.
void *rt_camera3d_get_right(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    double x = finite_or(cam->view[0], 1.0);
    double y = finite_or(cam->view[1], 0.0);
    double z = finite_or(cam->view[2], 0.0);
    camera_normalize_vec3_or(&x, &y, &z, 1.0, 0.0, 0.0);
    return rt_vec3_new(x, y, z);
}

/// @brief Extract the world-space up unit vector (the camera's screen-up axis, ADR 0227).
/// Read directly from the view matrix's second row, completing the Forward/Right basis.
/// Caller owns the returned Vec3.
/// @param obj Borrowed heap or stack camera.
/// @return Owned normalized up `Vec3`, or null for an invalid camera/allocation failure.
void *rt_camera3d_get_up(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    double x = finite_or(cam->view[4], 0.0);
    double y = finite_or(cam->view[5], 1.0);
    double z = finite_or(cam->view[6], 0.0);
    camera_normalize_vec3_or(&x, &y, &z, 0.0, 1.0, 0.0);
    return rt_vec3_new(x, y, z);
}

/// @brief Copy the retained row-major view matrix into a caller-owned Mat4 (ADR 0227).
/// @param obj Borrowed heap or stack camera.
/// @return Owned Mat4 copy of the view matrix, or null for an invalid camera.
void *rt_camera3d_get_view_matrix(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    const double *v = cam->view;
    return rt_mat4_new(v[0],
                       v[1],
                       v[2],
                       v[3],
                       v[4],
                       v[5],
                       v[6],
                       v[7],
                       v[8],
                       v[9],
                       v[10],
                       v[11],
                       v[12],
                       v[13],
                       v[14],
                       v[15]);
}

/// @brief Copy the retained row-major projection matrix into a caller-owned Mat4 (ADR 0227).
/// @details This is exactly the projection the renderer last synced for this
///          camera — including reversed-Z where the backend uses it — so
///          Zia-side projection math matches the rendered result.
/// @param obj Borrowed heap or stack camera.
/// @return Owned Mat4 copy of the projection matrix, or null for an invalid camera.
void *rt_camera3d_get_projection_matrix(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return NULL;
    const double *p = cam->projection;
    return rt_mat4_new(p[0],
                       p[1],
                       p[2],
                       p[3],
                       p[4],
                       p[5],
                       p[6],
                       p[7],
                       p[8],
                       p[9],
                       p[10],
                       p[11],
                       p[12],
                       p[13],
                       p[14],
                       p[15]);
}

/// @brief Read the aspect ratio the retained projection was built with (ADR 0227).
/// @param obj Borrowed heap or stack camera.
/// @return Retained positive width-to-height ratio, or 0.0 for an invalid camera.
double rt_camera3d_get_aspect(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return 0.0;
    return finite_or(cam->aspect, 0.0);
}

/* Invert a 4x4 row-major matrix. Returns 0 on success, -1 if singular. */
/// @brief Invert a 4x4 row-major matrix using cofactor expansion.
/// Each inv[i] is the cofactor (signed minor) of the transpose, so
/// the adjugate matrix is built column-by-column. The determinant is
/// computed from the first row and its cofactors. Returns -1 if singular.
/// @param m Borrowed finite 16-element input matrix.
/// @param[out] out Writable 16-element inverse matrix.
/// @return `0` on success, or `-1` for null, non-finite, singular, or overflowing input.
static int mat4d_invert(const double *m, double *out) {
    double inv[16];
    if (!m || !out)
        return -1;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return -1;
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    for (int i = 0; i < 16; i++) {
        if (!isfinite(inv[i]))
            return -1;
    }

    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!isfinite(det) || fabs(det) < 1e-12)
        return -1;

    double inv_det = 1.0 / det;
    for (int i = 0; i < 16; i++) {
        double value = inv[i] * inv_det;
        if (!isfinite(value))
            return -1;
        out[i] = value;
    }
    return 0;
}

/// @brief Relative-epsilon equality for cached pick parameters (NaN/inf compare unequal).
/// @details Tolerance scales with magnitude (max(|a|,|b|,1) * 1e-12), so the inverse-VP cache stays
///          valid across both tiny and large coordinate scales.
/// @param a First scalar.
/// @param b Second scalar.
/// @return `1` when both are finite and relatively equal; otherwise `0`.
static int camera_pick_cache_scalar_equal(double a, double b) {
    double scale;
    if (!isfinite(a) || !isfinite(b))
        return 0;
    scale = fmax(fmax(fabs(a), fabs(b)), 1.0);
    return fabs(a - b) <= scale * 1e-12;
}

/// @brief Compute the inverse view-projection matrix used to unproject screen rays for picking.
/// @details Builds the projection (perspective or ortho) for the given @p aspect, multiplies by the
///          view matrix, and inverts the result. @p out_inv_vp receives the 4x4 inverse.
/// @param[in,out] cam Borrowed camera whose view may be repaired and whose inverse cache is
/// updated.
/// @param aspect Render-surface width-to-height ratio.
/// @param[out] out_inv_vp Writable 16-element inverse view-projection matrix.
/// @return 1 on success, 0 if inputs are invalid or the matrix is singular.
static int camera_get_pick_inv_vp(rt_camera3d *cam, double aspect, double *out_inv_vp) {
    double near_plane;
    double far_plane;
    double fov;
    double ortho_size;
    double projection[16];
    double vp[16];
    if (!cam || !out_inv_vp)
        return 0;
    aspect = sanitize_aspect(aspect);
    near_plane = cam->near_plane;
    far_plane = cam->far_plane;
    sanitize_clip_planes(&near_plane, &far_plane);
    fov = sanitize_fov(cam->fov);
    ortho_size = sanitize_ortho_size(cam->ortho_size);
    if (!camera_matrix_is_finite(cam->view)) {
        cam->pick_cache_valid = 0;
        camera_rebuild_fps_view(cam);
        camera_apply_shake_to_view(cam);
    }
    if (!camera_matrix_is_finite(cam->view))
        return 0;
    if (cam->pick_cache_valid && cam->pick_cache_is_ortho == cam->is_ortho &&
        camera_pick_cache_scalar_equal(cam->pick_cache_aspect, aspect) &&
        camera_pick_cache_scalar_equal(cam->pick_cache_fov, fov) &&
        camera_pick_cache_scalar_equal(cam->pick_cache_near, near_plane) &&
        camera_pick_cache_scalar_equal(cam->pick_cache_far, far_plane) &&
        camera_pick_cache_scalar_equal(cam->pick_cache_ortho_size, ortho_size) &&
        memcmp(cam->pick_cache_view, cam->view, sizeof(cam->view)) == 0) {
        memcpy(out_inv_vp, cam->pick_cache_inv_vp, sizeof(cam->pick_cache_inv_vp));
        return 1;
    }

    if (cam->is_ortho) {
        double half_w = ortho_size * aspect;
        build_ortho(projection, -half_w, half_w, -ortho_size, ortho_size, near_plane, far_plane);
    } else {
        build_perspective(projection, fov, aspect, near_plane, far_plane);
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            vp[r * 4 + c] = projection[r * 4 + 0] * cam->view[0 * 4 + c] +
                            projection[r * 4 + 1] * cam->view[1 * 4 + c] +
                            projection[r * 4 + 2] * cam->view[2 * 4 + c] +
                            projection[r * 4 + 3] * cam->view[3 * 4 + c];
            if (!isfinite(vp[r * 4 + c])) {
                cam->pick_cache_valid = 0;
                return 0;
            }
        }
    }
    if (mat4d_invert(vp, out_inv_vp) != 0) {
        cam->pick_cache_valid = 0;
        return 0;
    }
    memcpy(cam->pick_cache_view, cam->view, sizeof(cam->view));
    memcpy(cam->pick_cache_inv_vp, out_inv_vp, sizeof(cam->pick_cache_inv_vp));
    cam->pick_cache_aspect = aspect;
    cam->pick_cache_fov = fov;
    cam->pick_cache_near = near_plane;
    cam->pick_cache_far = far_plane;
    cam->pick_cache_ortho_size = ortho_size;
    cam->pick_cache_is_ortho = cam->is_ortho;
    cam->pick_cache_valid = 1;
    return 1;
}

/// @brief Project a world-space point to pixel coordinates.
/// @details Builds the same projection×view the picking path uses (perspective
///          or ortho, sanitized clip planes) and projects @p x/y/z. Writes the
///          pixel position and returns 1 when the point is in front of the
///          camera (clip w > 0); 0 means behind (outputs still written from
///          the mirrored projection — callers should hide anchored UI).
/// @param obj Borrowed heap or stack camera.
/// @param x World-space point x coordinate.
/// @param y World-space point y coordinate.
/// @param z World-space point z coordinate.
/// @param sw Positive screen width in pixels.
/// @param sh Positive screen height in pixels.
/// @param[out] out_sx Optional destination initialized to zero and then set to pixel x.
/// @param[out] out_sy Optional destination initialized to zero and then set to pixel y.
/// @return `1` when projection is valid and the point lies in front of the camera; otherwise `0`.
int8_t rt_camera3d_world_to_screen(void *obj,
                                   double x,
                                   double y,
                                   double z,
                                   int64_t sw,
                                   int64_t sh,
                                   double *out_sx,
                                   double *out_sy) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (out_sx)
        *out_sx = 0.0;
    if (out_sy)
        *out_sy = 0.0;
    if (!cam || sw <= 0 || sh <= 0)
        return 0;
    double near_plane = cam->near_plane;
    double far_plane = cam->far_plane;
    sanitize_clip_planes(&near_plane, &far_plane);
    double aspect = sanitize_aspect((double)sw / (double)sh);
    double projection[16];
    if (cam->is_ortho) {
        double ortho_size = sanitize_ortho_size(cam->ortho_size);
        double half_w = ortho_size * aspect;
        build_ortho(projection, -half_w, half_w, -ortho_size, ortho_size, near_plane, far_plane);
    } else {
        build_perspective(projection, sanitize_fov(cam->fov), aspect, near_plane, far_plane);
    }
    if (!camera_matrix_is_finite(cam->view)) {
        camera_rebuild_fps_view(cam);
        camera_apply_shake_to_view(cam);
    }
    if (!camera_matrix_is_finite(cam->view))
        return 0;
    /* view-space then clip-space (column-vector convention, matches picking). */
    double p[4] = {x, y, z, 1.0};
    double view_pt[4];
    for (int r = 0; r < 4; ++r)
        view_pt[r] = cam->view[r * 4 + 0] * p[0] + cam->view[r * 4 + 1] * p[1] +
                     cam->view[r * 4 + 2] * p[2] + cam->view[r * 4 + 3] * p[3];
    double clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = projection[r * 4 + 0] * view_pt[0] + projection[r * 4 + 1] * view_pt[1] +
                  projection[r * 4 + 2] * view_pt[2] + projection[r * 4 + 3] * view_pt[3];
    if (!isfinite(clip[3]) || fabs(clip[3]) <= 1e-12)
        return 0;
    double inv_w = 1.0 / clip[3];
    double ndc_x = clip[0] * inv_w;
    double ndc_y = clip[1] * inv_w;
    if (out_sx)
        *out_sx = (ndc_x + 1.0) * 0.5 * (double)sw;
    if (out_sy)
        *out_sy = (1.0 - ndc_y) * 0.5 * (double)sh;
    return clip[3] > 0.0 ? 1 : 0;
}

/// @brief VM-facing WorldToScreen: returns Vec3(pixelX, pixelY, visible ? 1 : 0).
/// @param obj Borrowed heap or stack camera.
/// @param point Borrowed world-space `Vec3`; invalid types trap and return a zero vector.
/// @param sw Screen width in pixels.
/// @param sh Screen height in pixels.
/// @return Owned `Vec3(pixel_x, pixel_y, visibility_flag)`.
void *rt_camera3d_world_to_screen_vec(void *obj, void *point, int64_t sw, int64_t sh) {
    if (!rt_g3d_is_vec3(point)) {
        rt_trap("Camera3D.WorldToScreen: point must be Vec3");
        return rt_vec3_new(0.0, 0.0, 0.0);
    }
    double sx = 0.0;
    double sy = 0.0;
    int8_t visible = rt_camera3d_world_to_screen(
        obj, rt_vec3_x(point), rt_vec3_y(point), rt_vec3_z(point), sw, sh, &sx, &sy);
    return rt_vec3_new(sx, sy, visible ? 1.0 : 0.0);
}

/// @brief Unprojects a screen-space pixel into a world-space ray direction.
/// @details Perspective rays connect the shaken rendered eye to the unprojected near-plane point.
///          Orthographic rays are the shared normalized view-forward direction. Degenerate screens,
///          singular matrices, or invalid cameras return `(0, 0, -1)`.
/// @param obj Borrowed heap or stack camera.
/// @param sx Pixel x coordinate.
/// @param sy Pixel y coordinate.
/// @param sw Positive screen width in pixels.
/// @param sh Positive screen height in pixels.
/// @return Owned normalized ray-direction `Vec3`.
void *rt_camera3d_screen_to_ray(void *obj, int64_t sx, int64_t sy, int64_t sw, int64_t sh) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam || sw <= 0 || sh <= 0)
        return rt_vec3_new(0.0, 0.0, -1.0);

    if (cam->is_ortho) {
        double fx = -cam->view[8];
        double fy = -cam->view[9];
        double fz = -cam->view[10];
        camera_normalize_vec3_or(&fx, &fy, &fz, 0.0, 0.0, -1.0);
        return rt_vec3_new(fx, fy, fz);
    }

    /* Screen → NDC */
    double ndc_x = clamp_abs_or((2.0 * (double)sx / (double)sw) - 1.0, 0.0, CAMERA3D_ASPECT_MAX);
    double ndc_y = clamp_abs_or(1.0 - (2.0 * (double)sy / (double)sh), 0.0, CAMERA3D_ASPECT_MAX);

    double aspect = sanitize_aspect((double)sw / (double)sh);
    double inv_vp[16];
    if (!camera_get_pick_inv_vp(cam, aspect, inv_vp))
        return rt_vec3_new(0.0, 0.0, -1.0);

    /* Unproject NDC point at near plane (z=-1) to world space */
    double p[4] = {ndc_x, ndc_y, -1.0, 1.0};
    double world[4];
    world[0] = inv_vp[0] * p[0] + inv_vp[1] * p[1] + inv_vp[2] * p[2] + inv_vp[3] * p[3];
    world[1] = inv_vp[4] * p[0] + inv_vp[5] * p[1] + inv_vp[6] * p[2] + inv_vp[7] * p[3];
    world[2] = inv_vp[8] * p[0] + inv_vp[9] * p[1] + inv_vp[10] * p[2] + inv_vp[11] * p[3];
    world[3] = inv_vp[12] * p[0] + inv_vp[13] * p[1] + inv_vp[14] * p[2] + inv_vp[15] * p[3];

    if (!isfinite(world[3]) || fabs(world[3]) <= 1e-12)
        return rt_vec3_new(0.0, 0.0, -1.0);
    world[0] = clamp_abs_or(world[0] / world[3], 0.0, CAMERA3D_WORLD_ABS_MAX);
    world[1] = clamp_abs_or(world[1] / world[3], 0.0, CAMERA3D_WORLD_ABS_MAX);
    world[2] = clamp_abs_or(world[2] / world[3], -1.0, CAMERA3D_WORLD_ABS_MAX);

    /* Ray direction = normalize(worldPoint - rendered eye) */
    double origin[3];
    camera_eye_with_shake(cam, origin);
    double origin_x = origin[0];
    double origin_y = origin[1];
    double origin_z = origin[2];
    double dx = world[0] - origin_x;
    double dy = world[1] - origin_y;
    double dz = world[2] - origin_z;
    camera_normalize_vec3_or(&dx, &dy, &dz, 0.0, 0.0, -1.0);

    return rt_vec3_new(dx, dy, dz);
}

/// @brief Return the world-space origin for a screen-space picking ray.
/// @details Perspective cameras originate at the rendered eye position. Orthographic
///          cameras originate at the unprojected near-plane point for the requested
///          pixel, because each screen pixel has a distinct parallel ray.
/// @param obj Borrowed heap or stack camera.
/// @param sx Pixel x coordinate.
/// @param sy Pixel y coordinate.
/// @param sw Positive screen width in pixels.
/// @param sh Positive screen height in pixels.
/// @return Owned ray-origin `Vec3`; invalid inputs fall back to origin or the rendered eye.
void *rt_camera3d_screen_to_ray_origin(void *obj, int64_t sx, int64_t sy, int64_t sw, int64_t sh) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam || sw <= 0 || sh <= 0)
        return rt_vec3_new(0.0, 0.0, 0.0);

    double eye[3];
    camera_eye_with_shake(cam, eye);
    double eye_x = eye[0];
    double eye_y = eye[1];
    double eye_z = eye[2];
    if (!cam->is_ortho)
        return rt_vec3_new(eye_x, eye_y, eye_z);

    double ndc_x = clamp_abs_or((2.0 * (double)sx / (double)sw) - 1.0, 0.0, CAMERA3D_ASPECT_MAX);
    double ndc_y = clamp_abs_or(1.0 - (2.0 * (double)sy / (double)sh), 0.0, CAMERA3D_ASPECT_MAX);
    double aspect = sanitize_aspect((double)sw / (double)sh);
    double inv_vp[16];
    if (!camera_get_pick_inv_vp(cam, aspect, inv_vp))
        return rt_vec3_new(eye_x, eye_y, eye_z);

    double p[4] = {ndc_x, ndc_y, -1.0, 1.0};
    double world[4];
    world[0] = inv_vp[0] * p[0] + inv_vp[1] * p[1] + inv_vp[2] * p[2] + inv_vp[3] * p[3];
    world[1] = inv_vp[4] * p[0] + inv_vp[5] * p[1] + inv_vp[6] * p[2] + inv_vp[7] * p[3];
    world[2] = inv_vp[8] * p[0] + inv_vp[9] * p[1] + inv_vp[10] * p[2] + inv_vp[11] * p[3];
    world[3] = inv_vp[12] * p[0] + inv_vp[13] * p[1] + inv_vp[14] * p[2] + inv_vp[15] * p[3];

    if (!isfinite(world[3]) || fabs(world[3]) <= 1e-12)
        return rt_vec3_new(eye_x, eye_y, eye_z);
    world[0] = clamp_abs_or(world[0] / world[3], eye_x, CAMERA3D_WORLD_ABS_MAX);
    world[1] = clamp_abs_or(world[1] / world[3], eye_y, CAMERA3D_WORLD_ABS_MAX);
    world[2] = clamp_abs_or(world[2] / world[3], eye_z, CAMERA3D_WORLD_ABS_MAX);
    return rt_vec3_new(world[0], world[1], world[2]);
}

/*==========================================================================
 * FPS camera controller
 *=========================================================================*/

/// @brief Initialize FPS-style camera state (yaw/pitch from current orientation).
/// @param obj Borrowed heap or stack camera.
void rt_camera3d_fps_init(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    camera_sync_fps_angles_from_view(cam);
}

/// @brief Update FPS camera from mouse look (dx/dy) and WASD movement (fwd/right/up).
/// @details Integrates mouse deltas into yaw/pitch (with pitch clamped to ±89°),
///          then computes the forward/right/up vectors and applies WASD movement.
///          The camera shake offset is added last if active.
/// @param obj Borrowed heap or stack camera.
/// @param yaw_delta Horizontal look delta in degrees.
/// @param pitch_delta Vertical look delta in degrees, bounded before accumulation.
/// @param move_fwd Signed forward-axis movement input.
/// @param move_right Signed right-axis movement input.
/// @param move_up Signed world-up movement input.
/// @param speed Non-negative movement speed in world units per second.
/// @param dt Non-negative elapsed time in seconds.
void rt_camera3d_fps_update(void *obj,
                            double yaw_delta,
                            double pitch_delta,
                            double move_fwd,
                            double move_right,
                            double move_up,
                            double speed,
                            double dt) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;

    yaw_delta = camera_wrap_degrees(yaw_delta);
    pitch_delta = clamp_abs_or(pitch_delta, 0.0, 180.0);
    move_fwd = clamp_abs_or(move_fwd, 0.0, CAMERA3D_WORLD_ABS_MAX);
    move_right = clamp_abs_or(move_right, 0.0, CAMERA3D_WORLD_ABS_MAX);
    move_up = clamp_abs_or(move_up, 0.0, CAMERA3D_WORLD_ABS_MAX);
    speed = sanitize_nonnegative(speed, 0.0);
    dt = sanitize_nonnegative(dt, 0.0);
    camera_sanitize_eye(cam);
    cam->fps_yaw = camera_wrap_degrees(cam->fps_yaw);
    cam->fps_pitch = camera_clamp_pitch(cam->fps_pitch);

    /* Accumulate yaw/pitch from mouse deltas */
    cam->fps_yaw = camera_wrap_degrees(cam->fps_yaw + yaw_delta);
    cam->fps_pitch = camera_clamp_pitch(cam->fps_pitch + pitch_delta);

    /* Compute forward and right vectors from yaw/pitch */
    double yaw_rad = cam->fps_yaw * (M_PI / 180.0);
    double pitch_rad = cam->fps_pitch * (M_PI / 180.0);
    double cp = cos(pitch_rad);

    double fwd_x = sin(yaw_rad) * cp;
    double fwd_y = sin(pitch_rad);
    double fwd_z = -cos(yaw_rad) * cp;

    /* Right = cross(forward, world_up), normalized on XZ plane */
    double right_x = cos(yaw_rad);
    double right_z = sin(yaw_rad);

    /* Apply movement */
    double move_scale = speed * dt;
    if (!isfinite(move_scale) || move_scale > CAMERA3D_WORLD_ABS_MAX)
        move_scale = CAMERA3D_WORLD_ABS_MAX;
    cam->eye[0] = clamp_abs_or(cam->eye[0] + fwd_x * move_fwd * move_scale +
                                   right_x * move_right * move_scale,
                               0.0,
                               CAMERA3D_WORLD_ABS_MAX);
    cam->eye[1] = clamp_abs_or(cam->eye[1] + fwd_y * move_fwd * move_scale + move_up * move_scale,
                               0.0,
                               CAMERA3D_WORLD_ABS_MAX);
    cam->eye[2] = clamp_abs_or(cam->eye[2] + fwd_z * move_fwd * move_scale +
                                   right_z * move_right * move_scale,
                               0.0,
                               CAMERA3D_WORLD_ABS_MAX);

    /* Rebuild view matrix via LookAt */
    double target[3] = {cam->eye[0] + fwd_x, cam->eye[1] + fwd_y, cam->eye[2] + fwd_z};
    double up[3] = {0.0, 1.0, 0.0};
    build_look_at(cam->view, cam->eye, target, up);
    camera_apply_shake_to_view(cam);
}

/// @brief Return the FPS-controller yaw in degrees (heading around world Y).
/// @param obj Borrowed heap or stack camera.
/// @return Wrapped yaw in degrees, or zero for an invalid camera.
double rt_camera3d_get_yaw(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    return cam ? camera_wrap_degrees(cam->fps_yaw) : 0.0;
}

/// @brief Return the FPS-controller pitch in degrees (look up/down).
/// @param obj Borrowed heap or stack camera.
/// @return Pitch clamped to `[-89, 89]` degrees, or zero for an invalid camera.
double rt_camera3d_get_pitch(void *obj) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    return cam ? camera_clamp_pitch(cam->fps_pitch) : 0.0;
}

/// @brief Set yaw absolutely and rebuild the view (and shake) to match.
/// @details Unlike `fps_update`'s yaw delta, this lets gameplay code
///          snap heading directly (e.g. cutscene camera, teleport,
///          respawn). Caller is responsible for normalizing the angle
///          if they care about the stored value range.
/// @param obj Borrowed heap or stack camera.
/// @param yaw Absolute heading in degrees; wrapped internally.
void rt_camera3d_set_yaw(void *obj, double yaw) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->fps_yaw = camera_wrap_degrees(yaw);
    camera_rebuild_fps_view(cam);
    camera_apply_shake_to_view(cam);
}

/// @brief Set pitch absolutely (clamped to ±89°) and rebuild the view.
/// @details The clamp matches `fps_update` so manual pitch overrides
///          can never produce a degenerate look-at direction.
/// @param obj Borrowed heap or stack camera.
/// @param pitch Absolute pitch in degrees, clamped to `[-89, 89]`.
void rt_camera3d_set_pitch(void *obj, double pitch) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->fps_pitch = camera_clamp_pitch(pitch);
    camera_rebuild_fps_view(cam);
    camera_apply_shake_to_view(cam);
}

/*==========================================================================
 * Camera shake
 *=========================================================================*/

/// @brief Advance the shake state one frame and recompute the eye offset.
/// @details Bails out early once `shake_duration` reaches zero, also
///          clearing the offset so the camera snaps back to its true
///          eye position. While shake is active:
///          - Duration counts down by dt (linear time decay).
///          - Intensity follows an exponential decay
///            (intensity *= exp(-decay * dt)) so impulses ramp out
///            smoothly — `decay = 5` halves intensity in ~138 ms.
///          - Two consecutive xorshift32 draws produce a deterministic
///            uniform [-1, 1] pair (r1, r2). The Z component uses
///            r1*r2*0.3 so it's correlated with X/Y but smaller, biasing
///            shake toward the screen plane (preferred for first-person).
///          The seed lives on the camera, so two cameras shake
///          independently and a single camera replays identically across
///          runs given the same dt sequence.
/// @param[in,out] cam Borrowed camera whose shake timer, intensity, PRNG seed, and offset advance.
/// @param dt Non-negative elapsed time in seconds.
static void apply_shake(rt_camera3d *cam, double dt) {
    if (!cam)
        return;
    dt = sanitize_nonnegative(dt, 0.0);
    cam->shake_duration = sanitize_nonnegative(cam->shake_duration, 0.0);
    cam->shake_intensity = sanitize_nonnegative(cam->shake_intensity, 0.0);
    cam->shake_decay = sanitize_nonnegative(cam->shake_decay, 5.0);
    if (cam->shake_decay <= 0.0)
        cam->shake_decay = 5.0;
    if (cam->shake_duration <= 0.0) {
        cam->shake_offset[0] = cam->shake_offset[1] = cam->shake_offset[2] = 0.0;
        return;
    }
    cam->shake_duration -= dt;
    if (cam->shake_duration <= 0.0) {
        cam->shake_duration = 0.0;
        cam->shake_intensity = 0.0;
        cam->shake_offset[0] = cam->shake_offset[1] = cam->shake_offset[2] = 0.0;
        return;
    }
    if (cam->shake_decay * dt >= CAMERA3D_DAMPING_EXP_MAX)
        cam->shake_intensity = 0.0;
    else
        cam->shake_intensity *= exp(-cam->shake_decay * dt);

    /* Xorshift PRNG for deterministic random offsets */
    cam->shake_seed ^= cam->shake_seed << 13;
    cam->shake_seed ^= cam->shake_seed >> 17;
    cam->shake_seed ^= cam->shake_seed << 5;
    double r1 = ((double)(cam->shake_seed & 0xFFFF) / 65535.0) * 2.0 - 1.0;
    cam->shake_seed ^= cam->shake_seed << 13;
    cam->shake_seed ^= cam->shake_seed >> 17;
    cam->shake_seed ^= cam->shake_seed << 5;
    double r2 = ((double)(cam->shake_seed & 0xFFFF) / 65535.0) * 2.0 - 1.0;

    cam->shake_offset[0] = clamp_abs_or(r1 * cam->shake_intensity, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->shake_offset[1] = clamp_abs_or(r2 * cam->shake_intensity, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->shake_offset[2] =
        clamp_abs_or((r1 * r2) * cam->shake_intensity * 0.3, 0.0, CAMERA3D_WORLD_ABS_MAX);
}

/// @brief Per-frame entry point that advances shake decay and rebuilds the view.
/// @details Should be called once per frame from the renderer (or game
///          loop) with the frame delta. Splits the work between
///          `apply_shake` (advance state, compute offset) and
///          `camera_apply_shake_to_view` (rebuild view matrix using the
///          new offset) so other call sites that mutate the view can
///          re-apply the shake without re-rolling the PRNG.
/// @param obj Borrowed heap or stack camera.
/// @param dt Frame duration in seconds; negative/non-finite values sanitize to zero.
void rt_camera3d_update_shake_for_frame(void *obj, double dt) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);

    if (!cam)
        return;
    apply_shake(cam, dt);
    camera_apply_shake_to_view(cam);
}

/// @brief Advance camera shake once for a renderer timing token and re-apply the shaken view.
/// @details Canvas3D may render multiple 3D passes before the next poll/flip updates delta time.
///   Calling the un-tokened shake update from every pass decays the shake and advances its PRNG
///   multiple times in one visible frame, which can make otherwise stable triangles appear to jump
///   or flicker. The tokened variant advances the stochastic state only when @p frame_token
///   changes; repeated calls with the same token only re-apply the current offset after other
///   camera code mutates the view.
/// @param obj         Camera3D handle.
/// @param dt          Frame delta in seconds; negative/NaN values are sanitized by the shake path.
/// @param frame_token Monotonic renderer timing token. Non-positive tokens fall back to the legacy
///                    un-tokened behavior for compatibility with direct internal callers.
void rt_camera3d_update_shake_for_frame_token(void *obj, double dt, int64_t frame_token) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);

    if (!cam)
        return;
    if (frame_token <= 0) {
        rt_camera3d_update_shake_for_frame(obj, dt);
        return;
    }
    if (cam->last_shake_update_token == frame_token) {
        camera_apply_shake_to_view(cam);
        return;
    }
    cam->last_shake_update_token = frame_token;
    rt_camera3d_update_shake_for_frame(obj, dt);
}

/// @brief Trigger a camera shake effect (exponentially decaying random offset).
/// @details The shake applies random XY offsets that decay over the given duration.
///          Used for explosions, impacts, and other feedback effects.
/// @param obj Borrowed heap or stack camera.
/// @param intensity Non-negative initial offset magnitude in world units.
/// @param duration Non-negative effect duration in seconds.
/// @param decay Positive exponential decay rate; invalid values use 5.
void rt_camera3d_shake(void *obj, double intensity, double duration, double decay) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam)
        return;
    cam->shake_intensity = sanitize_nonnegative(intensity, 0.0);
    cam->shake_duration = sanitize_nonnegative(duration, 0.0);
    cam->shake_decay = isfinite(decay) && decay > 0.0 ? sanitize_nonnegative(decay, 5.0) : 5.0;
    cam->last_shake_update_token = 0;
    apply_shake(cam, 0.0);
    camera_apply_shake_to_view(cam);
}

/*==========================================================================
 * Smooth follow (third-person camera)
 *=========================================================================*/

/// @brief Smoothly interpolate the camera toward a target position over time.
/// @details Uses exponential decay (lerp with speed * dt) for natural smoothing.
///          The camera maintains a configurable offset from the target.
/// @param obj Borrowed heap or stack camera.
/// @param target_pos Borrowed `Vec3` follow target.
/// @param distance Non-negative trailing distance along current yaw.
/// @param height Signed vertical offset in world units.
/// @param speed Non-negative exponential convergence rate.
/// @param dt Non-negative elapsed time in seconds.
void rt_camera3d_smooth_follow(
    void *obj, void *target_pos, double distance, double height, double speed, double dt) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam || !rt_g3d_is_vec3(target_pos))
        return;

    height = clamp_abs_or(height, 0.0, CAMERA3D_WORLD_ABS_MAX);
    double tx = clamp_abs_or(rt_vec3_x(target_pos), 0.0, CAMERA3D_WORLD_ABS_MAX);
    double ty = clamp_abs_or(rt_vec3_y(target_pos), 0.0, CAMERA3D_WORLD_ABS_MAX) + height;
    double tz = clamp_abs_or(rt_vec3_z(target_pos), 0.0, CAMERA3D_WORLD_ABS_MAX);

    distance = sanitize_nonnegative(distance, 0.0);
    speed = sanitize_nonnegative(speed, 0.0);
    dt = sanitize_nonnegative(dt, 0.0);
    camera_sanitize_eye(cam);
    cam->fps_yaw = camera_wrap_degrees(cam->fps_yaw);

    /* Desired position: behind target using current yaw */
    double yaw_rad = cam->fps_yaw * (M_PI / 180.0);
    double desired[3] = {tx - sin(yaw_rad) * distance, ty, tz + cos(yaw_rad) * distance};

    /* Framerate-independent exponential damping */
    double t = camera_damping_factor(speed, dt);
    double base_eye[3] = {
        finite_or(cam->eye[0], 0.0), finite_or(cam->eye[1], 0.0), finite_or(cam->eye[2], 0.0)};
    cam->eye[0] =
        clamp_abs_or(base_eye[0] + (desired[0] - base_eye[0]) * t, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[1] =
        clamp_abs_or(base_eye[1] + (desired[1] - base_eye[1]) * t, 0.0, CAMERA3D_WORLD_ABS_MAX);
    cam->eye[2] =
        clamp_abs_or(base_eye[2] + (desired[2] - base_eye[2]) * t, 0.0, CAMERA3D_WORLD_ABS_MAX);

    double look_at[3] = {tx, ty - height * 0.3, tz};
    double up[3] = {0, 1, 0};
    build_look_at(cam->view, cam->eye, look_at, up);
    camera_sync_fps_angles_from_view(cam);
    camera_apply_shake_to_view(cam);
}

/*==========================================================================
 * Smooth look-at (gradual rotation toward target)
 *=========================================================================*/

/// @brief Smoothly rotate the camera toward a look-at target over time.
/// @param obj Borrowed heap or stack camera.
/// @param target Borrowed world-space `Vec3` target.
/// @param speed Non-negative exponential angular convergence rate.
/// @param dt Non-negative elapsed time in seconds.
void rt_camera3d_smooth_look_at(void *obj, void *target, double speed, double dt) {
    rt_camera3d *cam = rt_camera3d_checked_or_stack(obj);
    if (!cam || !rt_g3d_is_vec3(target))
        return;
    camera_sanitize_eye(cam);

    /* Current forward from view matrix */
    double cur_fwd[3] = {finite_or(-cam->view[8], 0.0),
                         finite_or(-cam->view[9], 0.0),
                         finite_or(-cam->view[10], -1.0)};
    camera_normalize_vec3_or(&cur_fwd[0], &cur_fwd[1], &cur_fwd[2], 0.0, 0.0, -1.0);

    /* Desired forward */
    double dx = clamp_abs_or(rt_vec3_x(target), 0.0, CAMERA3D_WORLD_ABS_MAX) -
                clamp_abs_or(cam->eye[0], 0.0, CAMERA3D_WORLD_ABS_MAX);
    double dy = clamp_abs_or(rt_vec3_y(target), 0.0, CAMERA3D_WORLD_ABS_MAX) -
                clamp_abs_or(cam->eye[1], 0.0, CAMERA3D_WORLD_ABS_MAX);
    double dz = clamp_abs_or(rt_vec3_z(target), 0.0, CAMERA3D_WORLD_ABS_MAX) -
                clamp_abs_or(cam->eye[2], 0.0, CAMERA3D_WORLD_ABS_MAX);
    speed = sanitize_nonnegative(speed, 0.0);
    dt = sanitize_nonnegative(dt, 0.0);
    camera_normalize_vec3_or(&dx, &dy, &dz, cur_fwd[0], cur_fwd[1], cur_fwd[2]);

    /* Exponential lerp toward desired */
    double t = camera_damping_factor(speed, dt);
    double new_fwd[3] = {cur_fwd[0] + (dx - cur_fwd[0]) * t,
                         cur_fwd[1] + (dy - cur_fwd[1]) * t,
                         cur_fwd[2] + (dz - cur_fwd[2]) * t};
    camera_normalize_vec3_or(
        &new_fwd[0], &new_fwd[1], &new_fwd[2], cur_fwd[0], cur_fwd[1], cur_fwd[2]);

    double look[3] = {clamp_abs_or(cam->eye[0] + new_fwd[0], 0.0, CAMERA3D_WORLD_ABS_MAX),
                      clamp_abs_or(cam->eye[1] + new_fwd[1], 0.0, CAMERA3D_WORLD_ABS_MAX),
                      clamp_abs_or(cam->eye[2] + new_fwd[2], -1.0, CAMERA3D_WORLD_ABS_MAX)};
    double up[3] = {0, 1, 0};
    build_look_at(cam->view, cam->eye, look, up);
    camera_sync_fps_angles_from_view(cam);
    camera_apply_shake_to_view(cam);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
