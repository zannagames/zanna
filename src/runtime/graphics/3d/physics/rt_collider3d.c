//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_collider3d.c
// Purpose: Collider3D runtime implementation for reusable 3D collision shapes.
//   Implements Zanna.Graphics3D.Collider3D — box / sphere / capsule / convex
//   hull / triangle mesh / compound / heightfield primitives that physics
//   bodies attach to. Caches a local-space AABB for broadphase queries and
//   exposes raw accessors for the physics core's narrow-phase pipeline.
//
// Key invariants:
//   - `bounds_min/max` are kept current after every mutation via
//     `collider3d_recompute_bounds`.
//   - Compound colliders are acyclic — `add_child` rejects self-reference and
//     traverses the existing tree to detect cycles before appending.
//   - Triangle-mesh and heightfield colliders are flagged static-only because
//     dynamic-vs-mesh is numerically unstable.
//   - All quaternions stored on child transforms are normalized at insert
//     time; non-finite inputs collapse to identity.
//
// Ownership/Lifetime:
//   - Colliders are GC-managed; the finalizer releases the mesh ref, every
//     child collider, and all owned scratch arrays.
//   - Compounds retain their children (ref-count incremented in add_child).
//   - Heightfield height buffers and child-transform arrays are owned and
//     freed in the finalizer.
//
// Links: rt_collider3d.h, rt_canvas3d_internal.h, rt_pixels.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_collider3d.c
 * @brief Implements reusable primitive, mesh, compound, and heightfield colliders.
 *
 * Collider3D owns shape-specific geometry, maintains a revisioned local-space
 * AABB, and exposes sanitized raw accessors used by broadphase and narrow-phase
 * physics. Compound nodes retain acyclic child trees with local transforms;
 * mesh-backed and heightfield shapes are marked static-only where required.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_quickhull3d.h"

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COLLIDER3D_COORD_ABS_MAX 1000000000000.0
#define COLLIDER3D_EXTENT_MAX 1000000000.0

static volatile uint64_t g_collider3d_geometry_epoch = 1u;

/// @brief Atomically advance the process-wide collider-geometry change epoch.
static void collider3d_note_global_geometry_change(void) {
    (void)rt_atomic_fetch_add_u64(&g_collider3d_geometry_epoch, UINT64_C(1), __ATOMIC_RELEASE);
}

/// @brief Read the process-wide epoch used to invalidate cached collider-dependent structures.
/// @return Current atomically published geometry epoch.
uint64_t rt_collider3d_global_geometry_epoch(void) {
    return rt_atomic_load_u64(&g_collider3d_geometry_epoch, __ATOMIC_ACQUIRE);
}

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void *rt_vec3_new(double x, double y, double z);
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);

typedef struct {
    double position[3];
    double rotation[4];
    double scale[3];
} rt_collider3d_child;

typedef struct {
    void *vptr;
    int32_t type;
    int8_t static_only;
    double half_extents[3];
    double radius;
    double height;
    rt_mesh3d *mesh;
    float *heightfield_heights;
    uint8_t *heightfield_holes; /* optional bitmask over (w-1)*(d-1) cells */
    int32_t heightfield_width;
    int32_t heightfield_depth;
    double heightfield_scale[3];
    double heightfield_min;
    double heightfield_max;
    void **children;
    rt_collider3d_child *child_transforms;
    int32_t child_count;
    int32_t child_capacity;
    double bounds_min[3];
    double bounds_max[3];
    uint64_t bounds_revision;
    uint32_t mesh_bounds_revision;
    /* Physics material overrides (appended; -1 = unset, body value applies). */
    double material_friction;
    double material_restitution;
    int64_t surface_type; /* Game3D.Surfaces registry id; 0 = untyped */
} rt_collider3d;

/// @brief Safe-cast an opaque handle to rt_collider3d, or NULL if not one.
/// @param obj Candidate runtime handle.
/// @return Borrowed collider payload, or NULL when the handle has a different class.
static rt_collider3d *collider3d_checked(void *obj) {
    return (rt_collider3d *)rt_g3d_checked_or_null(obj, RT_G3D_COLLIDER3D_CLASS_ID);
}

/// @brief Check whether an integer is a recognized Collider3D shape discriminator.
/// @param type Candidate shape discriminator.
/// @return Nonzero when @p type lies in the supported contiguous enum range.
static int collider3d_type_is_valid(int32_t type) {
    return type >= RT_COLLIDER3D_TYPE_BOX && type <= RT_COLLIDER3D_TYPE_HEIGHTFIELD;
}

/// @brief Derive a bounded child count from a compound's allocation invariants.
/// @param collider Compound collider to inspect.
/// @param require_transforms Nonzero to require the parallel transform array.
/// @return Safe number of readable child slots, or zero for invalid storage.
static int32_t collider3d_safe_child_count(const rt_collider3d *collider, int require_transforms) {
    if (!collider || collider->type != RT_COLLIDER3D_TYPE_COMPOUND || !collider->children ||
        collider->child_count <= 0 || collider->child_capacity <= 0)
        return 0;
    if (require_transforms && !collider->child_transforms)
        return 0;
    return collider->child_count < collider->child_capacity ? collider->child_count
                                                            : collider->child_capacity;
}

/// @brief Validate and borrow the Mesh3D retained by a mesh-backed collider.
/// @param collider Collider whose mesh slot is inspected.
/// @return Borrowed Mesh3D payload, or NULL for an absent or invalid mesh handle.
static rt_mesh3d *collider3d_mesh_or_null(const rt_collider3d *collider) {
    if (!collider || !collider->mesh)
        return NULL;
    return (rt_mesh3d *)rt_g3d_checked_or_null(collider->mesh, RT_G3D_MESH3D_CLASS_ID);
}

typedef struct {
    void *vptr;
    double position[3];
    double rotation[4];
    double scale[3];
    double matrix[16];
    int8_t dirty;
} rt_transform3d_view;

/// @brief Initialize a 3-component vector with bounded finite x/y/z components.
/// @param dst Three-component destination; a null pointer is ignored.
/// @param x Candidate X component.
/// @param y Candidate Y component.
/// @param z Candidate Z component.
static void vec3_set(double *dst, double x, double y, double z) {
    if (!dst)
        return;
    dst[0] = isfinite(x) ? x : 0.0;
    dst[1] = isfinite(y) ? y : 0.0;
    dst[2] = isfinite(z) ? z : 0.0;
    for (int i = 0; i < 3; ++i) {
        if (dst[i] > COLLIDER3D_COORD_ABS_MAX)
            dst[i] = COLLIDER3D_COORD_ABS_MAX;
        if (dst[i] < -COLLIDER3D_COORD_ABS_MAX)
            dst[i] = -COLLIDER3D_COORD_ABS_MAX;
    }
}

/// @brief Copy and sanitize a three-component vector.
/// @param dst Three-component destination.
/// @param src Optional source; NULL produces the origin.
static void vec3_copy(double *dst, const double *src) {
    if (!src) {
        vec3_set(dst, 0.0, 0.0, 0.0);
        return;
    }
    vec3_set(dst, src[0], src[1], src[2]);
}

/// @brief Clamp a `double` to the inclusive range `[lo, hi]`.
/// @param value Value to clamp.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return @p value limited to the supplied range.
static double clampd(double value, double lo, double hi) {
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Return a positive finite extent; invalid or near-zero values fall back to 1.0.
/// @param value Candidate extent, interpreted by absolute magnitude.
/// @return Sanitized extent in `(0, COLLIDER3D_EXTENT_MAX]`.
static double collider3d_extent_or_unit(double value) {
    if (!isfinite(value))
        return 1.0;
    value = fabs(value);
    if (value > COLLIDER3D_EXTENT_MAX)
        return COLLIDER3D_EXTENT_MAX;
    return value > 1e-12 ? value : 1.0;
}

/// @brief Return the absolute value of a finite scale, clamped to 1.0 for near-zero or non-finite
/// values.
/// @param value Candidate scale component.
/// @return Sanitized positive scale in `(0, COLLIDER3D_EXTENT_MAX]`.
static double collider3d_scale_or_unit(double value) {
    if (!isfinite(value))
        return 1.0;
    value = fabs(value);
    if (value > COLLIDER3D_EXTENT_MAX)
        return COLLIDER3D_EXTENT_MAX;
    return value > 1e-12 ? value : 1.0;
}

/// @brief Return `value` if finite, otherwise return `fallback`; used to sanitize transform fields
/// read from Zia objects.
/// @param value Candidate transform component.
/// @param fallback Replacement for a non-finite candidate.
/// @return Finite component clamped to the supported coordinate magnitude.
static double collider3d_transform_component_or(double value, double fallback) {
    value = isfinite(value) ? value : fallback;
    if (!isfinite(value))
        value = 0.0;
    if (value > COLLIDER3D_COORD_ABS_MAX)
        return COLLIDER3D_COORD_ABS_MAX;
    if (value < -COLLIDER3D_COORD_ABS_MAX)
        return -COLLIDER3D_COORD_ABS_MAX;
    return value;
}

/// @brief Set a quaternion to the identity rotation `(0, 0, 0, 1)` — no rotation.
/// @details Layout is `(x, y, z, w)` with `w` as the scalar part. Used
///          to initialize child transforms before reading from a real
///          Transform3D source.
/// @param q Four-component XYZW destination.
static void quat_identity(double *q) {
    q[0] = 0.0;
    q[1] = 0.0;
    q[2] = 0.0;
    q[3] = 1.0;
}

/// @brief Normalize a quaternion in-place; replaces with identity if length is near-zero or
/// non-finite.
/// @param q Four-component XYZW quaternion to normalize; a null pointer is ignored.
static void quat_normalize_local(double *q) {
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        quat_identity(q);
        return;
    }
    double len_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!isfinite(len_sq) || len_sq < 1e-24) {
        quat_identity(q);
        return;
    }
    double inv_len = 1.0 / sqrt(len_sq);
    q[0] *= inv_len;
    q[1] *= inv_len;
    q[2] *= inv_len;
    q[3] *= inv_len;
}

/// @brief Hamilton quaternion product `out = a * b`.
/// @details Applied left-to-right: rotating a vector by `out` is
///          equivalent to rotating first by `b`, then by `a`. The
///          formula expanded here is the standard `(s, v)` form
///          inlined for the (x, y, z, w) memory layout this file uses.
/// @param a Left-hand XYZW quaternion.
/// @param b Right-hand XYZW quaternion.
/// @param out Receives the XYZW Hamilton product.
static void quat_mul(const double *a, const double *b, double *out) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/// @brief Quaternion conjugate `out = (-x, -y, -z, w)`.
/// @details For a unit quaternion, the conjugate is the inverse — i.e.,
///          the rotation by the same angle around the opposite axis.
///          Cheaper than computing the true inverse `(conj/||q||²)` so
///          long as the input is normalized (which all rotations here
///          are).
/// @param q Input XYZW quaternion.
/// @param out Receives its XYZW conjugate.
static void quat_conjugate(const double *q, double *out) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Rotate a 3D vector by a unit quaternion via the conjugation formula `q * v * q⁻¹`.
/// @details Treats `v` as a pure quaternion `(x, y, z, 0)`, conjugates
///          on both sides, and extracts the vector part. This is the
///          textbook (and stable) way to apply a quaternion rotation
///          to a vector. Slightly slower than the optimized
///          "two-cross-product" form but easier to verify against
///          first principles.
/// @param q Unit XYZW rotation quaternion.
/// @param v Three-component input vector.
/// @param out Receives the rotated vector.
static void quat_rotate_vec3(const double *q, const double *v, double *out) {
    double qv[4] = {v[0], v[1], v[2], 0.0};
    double q_conj[4];
    double tmp[4];
    double rotated[4];
    quat_conjugate(q, q_conj);
    quat_mul(q, qv, tmp);
    quat_mul(tmp, q_conj, rotated);
    out[0] = rotated[0];
    out[1] = rotated[1];
    out[2] = rotated[2];
}

/// @brief Apply scale-then-rotate-then-translate to a single local point.
/// @details Standard SRT decomposition order — scale axis-by-axis, then
///          rotate by the quaternion, then translate. Reversing the
///          order would produce a different (and usually wrong) result
///          since rotation around the origin is not commutative with
///          translation.
/// @param position Three-component translation.
/// @param rotation Unit XYZW rotation quaternion.
/// @param scale Three-component scale.
/// @param local_point Point in the source local space.
/// @param out Receives the transformed and sanitized point.
static void transform_point_raw(const double *position,
                                const double *rotation,
                                const double *scale,
                                const double *local_point,
                                double *out) {
    double scaled[3] = {
        local_point[0] * scale[0], local_point[1] * scale[1], local_point[2] * scale[2]};
    double rotated[3];
    quat_rotate_vec3(rotation, scaled, rotated);
    vec3_set(out, position[0] + rotated[0], position[1] + rotated[1], position[2] + rotated[2]);
}

/// @brief Transform an axis-aligned bounding box and recompute the AABB in the new frame.
/// @details Iterates the 8 corners of the input AABB, transforms each
///          via SRT (`transform_point_raw`), and accumulates the new
///          axis-aligned extent. After rotation, the original AABB is
///          generally an OBB in the destination frame; the returned
///          AABB is the tightest axis-aligned box that contains it
///          (8 corners is the minimum needed — the OBB extrema sit at
///          corners, not edge midpoints).
/// @param bounds_min Source AABB minimum corner.
/// @param bounds_max Source AABB maximum corner.
/// @param position Destination-frame translation.
/// @param rotation Destination-frame unit XYZW rotation.
/// @param scale Destination-frame per-axis scale.
/// @param out_min Receives the transformed AABB minimum.
/// @param out_max Receives the transformed AABB maximum.
static void transform_bounds_raw(const double *bounds_min,
                                 const double *bounds_max,
                                 const double *position,
                                 const double *rotation,
                                 const double *scale,
                                 double *out_min,
                                 double *out_max) {
    double local_min[3];
    double local_max[3];
    double corner[3];
    double world[3];
    vec3_copy(local_min, bounds_min);
    vec3_copy(local_max, bounds_max);
    out_min[0] = out_min[1] = out_min[2] = DBL_MAX;
    out_max[0] = out_max[1] = out_max[2] = -DBL_MAX;
    for (int mask = 0; mask < 8; ++mask) {
        corner[0] = (mask & 1) ? local_max[0] : local_min[0];
        corner[1] = (mask & 2) ? local_max[1] : local_min[1];
        corner[2] = (mask & 4) ? local_max[2] : local_min[2];
        transform_point_raw(position, rotation, scale, corner, world);
        for (int axis = 0; axis < 3; ++axis) {
            if (world[axis] < out_min[axis])
                out_min[axis] = world[axis];
            if (world[axis] > out_max[axis])
                out_max[axis] = world[axis];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(out_min[axis]) || !isfinite(out_max[axis]) || out_min[axis] == DBL_MAX ||
            out_max[axis] == -DBL_MAX) {
            out_min[axis] = 0.0;
            out_max[axis] = 0.0;
        }
        if (out_min[axis] > out_max[axis]) {
            double tmp = out_min[axis];
            out_min[axis] = out_max[axis];
            out_max[axis] = tmp;
        }
        out_min[axis] = collider3d_transform_component_or(out_min[axis], 0.0);
        out_max[axis] = collider3d_transform_component_or(out_max[axis], 0.0);
    }
}

static void collider3d_recompute_bounds(rt_collider3d *collider);

/// @brief GC finalizer — release the mesh ref, every child collider, and all owned arrays.
/// @details Called when the collider's refcount drops to zero. Walks
///          the children array first (each child is itself a
///          GC-managed collider with its own finalizer), then frees
///          the dense child / child-transform arrays and the
///          heightfield-heights buffer. Nulled pointers prevent
///          double-free if the GC sweeps twice during shutdown.
/// @param obj Collider payload whose retained references and owned allocations are released.
static void collider3d_finalizer(void *obj) {
    rt_collider3d *collider = (rt_collider3d *)obj;
    if (!collider)
        return;
    rt_g3d_ref_slot_release_class(&collider->mesh, RT_G3D_MESH3D_CLASS_ID);
    int32_t child_count = collider3d_safe_child_count(collider, 0);
    for (int32_t i = 0; i < child_count; ++i)
        rt_g3d_ref_slot_release_class(&collider->children[i], RT_G3D_COLLIDER3D_CLASS_ID);
    free(collider->children);
    free(collider->child_transforms);
    free(collider->heightfield_heights);
    free(collider->heightfield_holes);
    collider->children = NULL;
    collider->child_transforms = NULL;
    collider->heightfield_heights = NULL;
    collider->heightfield_holes = NULL;
}

/// @brief Allocate a zeroed collider, set its type, and install the finalizer.
/// @details Single chokepoint for collider construction so all six
///          shape constructors (`new_box`, `new_sphere`, `new_capsule`,
///          `new_convex_hull`, `new_mesh`, `new_compound`,
///          `new_heightfield`) share identical initialization and GC
///          ownership setup. Bounds default to a degenerate `(0,0,0)`
///          AABB; each shape's constructor fills them in via
///          `recompute_bounds`.
/// @param type Valid Collider3D shape discriminator to install.
/// @return Initialized runtime-managed collider payload, or NULL after reporting allocation
/// failure.
static rt_collider3d *collider3d_alloc(int32_t type) {
    rt_collider3d *collider =
        (rt_collider3d *)rt_obj_new_i64(RT_G3D_COLLIDER3D_CLASS_ID, (int64_t)sizeof(rt_collider3d));
    if (!collider) {
        rt_trap("Collider3D.New: allocation failed");
        return NULL;
    }
    memset(collider, 0, sizeof(*collider));
    collider->type = type;
    collider->bounds_min[0] = collider->bounds_min[1] = collider->bounds_min[2] = 0.0;
    collider->bounds_max[0] = collider->bounds_max[1] = collider->bounds_max[2] = 0.0;
    collider->material_friction = -1.0;    /* unset: body friction applies */
    collider->material_restitution = -1.0; /* unset: body restitution applies */
    collider->surface_type = 0;
    rt_obj_set_finalizer(collider, collider3d_finalizer);
    return collider;
}

/// @brief Copy position / rotation / scale from a Transform3D into a child slot.
/// @details Defaults to the identity (origin, identity rotation, unit
///          scale) if `transform_obj` is null, so a compound child added
///          without an explicit local transform sits at the parent's
///          origin. The cast to `rt_transform3d_view` is safe because
///          `rt_transform3d` and the view share the prefix layout for
///          (vptr, position, rotation, scale).
/// @param dst Child-transform record to initialize.
/// @param transform_obj Optional Transform3D-compatible object; NULL selects identity.
static void collider3d_set_from_transform(rt_collider3d_child *dst, void *transform_obj) {
    vec3_set(dst->position, 0.0, 0.0, 0.0);
    quat_identity(dst->rotation);
    vec3_set(dst->scale, 1.0, 1.0, 1.0);
    if (!transform_obj)
        return;
    {
        rt_transform3d_view *xf = (rt_transform3d_view *)transform_obj;
        dst->position[0] = collider3d_transform_component_or(xf->position[0], 0.0);
        dst->position[1] = collider3d_transform_component_or(xf->position[1], 0.0);
        dst->position[2] = collider3d_transform_component_or(xf->position[2], 0.0);
        dst->scale[0] = collider3d_transform_component_or(xf->scale[0], 1.0);
        dst->scale[1] = collider3d_transform_component_or(xf->scale[1], 1.0);
        dst->scale[2] = collider3d_transform_component_or(xf->scale[2], 1.0);
        for (int axis = 0; axis < 3; ++axis) {
            if (fabs(dst->scale[axis]) > COLLIDER3D_EXTENT_MAX)
                dst->scale[axis] =
                    dst->scale[axis] < 0.0 ? -COLLIDER3D_EXTENT_MAX : COLLIDER3D_EXTENT_MAX;
        }
        dst->rotation[0] = xf->rotation[0];
        dst->rotation[1] = xf->rotation[1];
        dst->rotation[2] = xf->rotation[2];
        dst->rotation[3] = xf->rotation[3];
        quat_normalize_local(dst->rotation);
    }
}

/// @brief Recursively check whether `needle` appears in a compound tree within the public depth
///   contract.
/// @param root Candidate compound subtree to search.
/// @param needle Collider identity whose presence would form a cycle.
/// @param depth One-based compound depth the candidate would have after attachment.
/// @param too_deep Set when the candidate subtree would exceed the nesting limit.
/// @return One when @p needle is found, otherwise zero.
static int collider3d_contains_child(rt_collider3d *root,
                                     rt_collider3d *needle,
                                     int32_t depth,
                                     int *too_deep) {
    if (!root || !needle || root->type != RT_COLLIDER3D_TYPE_COMPOUND)
        return 0;
    if (depth > RT_COLLIDER3D_MAX_COMPOUND_DEPTH) {
        if (too_deep)
            *too_deep = 1;
        return 0;
    }
    int32_t child_count = collider3d_safe_child_count(root, 0);
    for (int32_t i = 0; i < child_count; ++i) {
        rt_collider3d *child = collider3d_checked(root->children[i]);
        if (!child)
            continue;
        if (child == needle)
            return 1;
        if (collider3d_contains_child(child, needle, depth + 1, too_deep))
            return 1;
    }
    return 0;
}

/// @brief Recompute the collider's local-space AABB based on its shape parameters.
/// @details Shape-by-shape:
///          - Box → ±half_extents on each axis.
///          - Sphere → ±radius on every axis.
///          - Capsule → radius on X/Z, half-height on Y (capsule axis is +Y).
///          - Convex hull / mesh → mirror the underlying mesh AABB.
///          - Compound → union of every child AABB after applying the
///            child's local transform.
///          - Heightfield → footprint width × heights min/max × footprint depth.
///          Called after any geometry mutation (set radius, add child,
///          rebuild heights) so subsequent broadphase queries use the
///          current bounds.
/// @param collider Collider whose local bounds are refreshed in place.
/// @param depth Current one-based compound traversal depth.
static void collider3d_recompute_bounds_at_depth(rt_collider3d *collider, int32_t depth) {
    if (!collider)
        return;
    /* Corrupt/shared graphs can outgrow the public limit after attachment. Stop before the C stack
     * can grow without bound and retain the deepest node's last valid cached bounds. */
    if (depth > RT_COLLIDER3D_MAX_COMPOUND_DEPTH && collider->type == RT_COLLIDER3D_TYPE_COMPOUND)
        return;

    rt_mesh3d *mesh = collider3d_mesh_or_null(collider);
    if ((collider->type == RT_COLLIDER3D_TYPE_CONVEX_HULL ||
         collider->type == RT_COLLIDER3D_TYPE_MESH) &&
        mesh && collider->bounds_revision != 0 &&
        collider->mesh_bounds_revision == mesh->geometry_revision && !mesh->bounds_dirty) {
        return;
    }

    switch (collider->type) {
        case RT_COLLIDER3D_TYPE_BOX:
            collider->half_extents[0] = collider3d_extent_or_unit(collider->half_extents[0]);
            collider->half_extents[1] = collider3d_extent_or_unit(collider->half_extents[1]);
            collider->half_extents[2] = collider3d_extent_or_unit(collider->half_extents[2]);
            collider->bounds_min[0] = -collider->half_extents[0];
            collider->bounds_min[1] = -collider->half_extents[1];
            collider->bounds_min[2] = -collider->half_extents[2];
            collider->bounds_max[0] = collider->half_extents[0];
            collider->bounds_max[1] = collider->half_extents[1];
            collider->bounds_max[2] = collider->half_extents[2];
            break;
        case RT_COLLIDER3D_TYPE_SPHERE:
            collider->radius = collider3d_extent_or_unit(collider->radius);
            collider->bounds_min[0] = collider->bounds_min[1] = collider->bounds_min[2] =
                -collider->radius;
            collider->bounds_max[0] = collider->bounds_max[1] = collider->bounds_max[2] =
                collider->radius;
            break;
        case RT_COLLIDER3D_TYPE_CAPSULE:
            collider->radius = collider3d_extent_or_unit(collider->radius);
            collider->height = collider3d_extent_or_unit(collider->height);
            if (collider->height < collider->radius * 2.0)
                collider->height = collider->radius * 2.0;
            collider->bounds_min[0] = -collider->radius;
            collider->bounds_min[1] = -collider->height * 0.5;
            collider->bounds_min[2] = -collider->radius;
            collider->bounds_max[0] = collider->radius;
            collider->bounds_max[1] = collider->height * 0.5;
            collider->bounds_max[2] = collider->radius;
            break;
        case RT_COLLIDER3D_TYPE_CONVEX_HULL:
        case RT_COLLIDER3D_TYPE_MESH:
            if (mesh) {
                rt_mesh3d_refresh_bounds(mesh);
                collider->mesh_bounds_revision = mesh->geometry_revision;
                vec3_set(
                    collider->bounds_min, mesh->aabb_min[0], mesh->aabb_min[1], mesh->aabb_min[2]);
                vec3_set(
                    collider->bounds_max, mesh->aabb_max[0], mesh->aabb_max[1], mesh->aabb_max[2]);
            } else {
                collider->mesh_bounds_revision = 0;
                vec3_set(collider->bounds_min, 0.0, 0.0, 0.0);
                vec3_set(collider->bounds_max, 0.0, 0.0, 0.0);
            }
            break;
        case RT_COLLIDER3D_TYPE_HEIGHTFIELD: {
            double half_width = 0.0;
            double half_depth = 0.0;
            collider->heightfield_scale[0] =
                collider3d_scale_or_unit(collider->heightfield_scale[0]);
            collider->heightfield_scale[1] =
                collider3d_scale_or_unit(collider->heightfield_scale[1]);
            collider->heightfield_scale[2] =
                collider3d_scale_or_unit(collider->heightfield_scale[2]);
            collider->heightfield_min =
                collider3d_transform_component_or(collider->heightfield_min, 0.0);
            collider->heightfield_max =
                collider3d_transform_component_or(collider->heightfield_max, 0.0);
            if (collider->heightfield_min > collider->heightfield_max) {
                double tmp = collider->heightfield_min;
                collider->heightfield_min = collider->heightfield_max;
                collider->heightfield_max = tmp;
            }
            if (collider->heightfield_width > 1)
                half_width =
                    ((double)(collider->heightfield_width - 1) * collider->heightfield_scale[0]) *
                    0.5;
            if (collider->heightfield_depth > 1)
                half_depth =
                    ((double)(collider->heightfield_depth - 1) * collider->heightfield_scale[2]) *
                    0.5;
            collider->bounds_min[0] = -half_width;
            collider->bounds_min[1] = collider->heightfield_min * collider->heightfield_scale[1];
            collider->bounds_min[2] = -half_depth;
            collider->bounds_max[0] = half_width;
            collider->bounds_max[1] = collider->heightfield_max * collider->heightfield_scale[1];
            collider->bounds_max[2] = half_depth;
            break;
        }
        case RT_COLLIDER3D_TYPE_COMPOUND: {
            int32_t child_count = collider3d_safe_child_count(collider, 1);
            if (child_count == 0) {
                vec3_set(collider->bounds_min, 0.0, 0.0, 0.0);
                vec3_set(collider->bounds_max, 0.0, 0.0, 0.0);
                break;
            }
            collider->bounds_min[0] = collider->bounds_min[1] = collider->bounds_min[2] = DBL_MAX;
            collider->bounds_max[0] = collider->bounds_max[1] = collider->bounds_max[2] = -DBL_MAX;
            for (int32_t i = 0; i < child_count; ++i) {
                double child_min[3];
                double child_max[3];
                rt_collider3d *child = collider3d_checked(collider->children[i]);
                if (!child)
                    continue;
                collider3d_recompute_bounds_at_depth(child, depth + 1);
                vec3_copy(child_min, child->bounds_min);
                vec3_copy(child_max, child->bounds_max);
                transform_bounds_raw(child_min,
                                     child_max,
                                     collider->child_transforms[i].position,
                                     collider->child_transforms[i].rotation,
                                     collider->child_transforms[i].scale,
                                     child_min,
                                     child_max);
                for (int axis = 0; axis < 3; ++axis) {
                    if (child_min[axis] < collider->bounds_min[axis])
                        collider->bounds_min[axis] = child_min[axis];
                    if (child_max[axis] > collider->bounds_max[axis])
                        collider->bounds_max[axis] = child_max[axis];
                }
            }
            if (collider->bounds_min[0] == DBL_MAX) {
                vec3_set(collider->bounds_min, 0.0, 0.0, 0.0);
                vec3_set(collider->bounds_max, 0.0, 0.0, 0.0);
            }
            break;
        }
        default:
            vec3_set(collider->bounds_min, 0.0, 0.0, 0.0);
            vec3_set(collider->bounds_max, 0.0, 0.0, 0.0);
            break;
    }
    for (int axis = 0; axis < 3; ++axis) {
        collider->bounds_min[axis] =
            collider3d_transform_component_or(collider->bounds_min[axis], 0.0);
        collider->bounds_max[axis] =
            collider3d_transform_component_or(collider->bounds_max[axis], 0.0);
        if (collider->bounds_min[axis] > collider->bounds_max[axis]) {
            double tmp = collider->bounds_min[axis];
            collider->bounds_min[axis] = collider->bounds_max[axis];
            collider->bounds_max[axis] = tmp;
        }
    }
    collider->bounds_revision =
        collider->bounds_revision == UINT64_MAX ? 1u : collider->bounds_revision + 1u;
}

/// @brief Refresh local bounds with a bounded compound-tree traversal.
/// @param collider Collider whose cached local AABB and revision are updated.
static void collider3d_recompute_bounds(rt_collider3d *collider) {
    collider3d_recompute_bounds_at_depth(collider, 1);
}

/// @brief Construct an axis-aligned box collider with half-extents (hx, hy, hz). Negative
/// values are taken as their absolute value. Box AABB is fully cached for fast queries.
/// @param hx X-axis half-extent.
/// @param hy Y-axis half-extent.
/// @param hz Z-axis half-extent.
/// @return Newly allocated box collider, or NULL on allocation failure.
void *rt_collider3d_new_box(double hx, double hy, double hz) {
    rt_collider3d *collider = collider3d_alloc(RT_COLLIDER3D_TYPE_BOX);
    if (!collider)
        return NULL;
    collider->half_extents[0] = collider3d_extent_or_unit(hx);
    collider->half_extents[1] = collider3d_extent_or_unit(hy);
    collider->half_extents[2] = collider3d_extent_or_unit(hz);
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Construct a sphere collider centered on the local origin with the given `radius`.
/// @param radius Sphere radius, sanitized to a positive supported extent.
/// @return Newly allocated sphere collider, or NULL on allocation failure.
void *rt_collider3d_new_sphere(double radius) {
    rt_collider3d *collider = collider3d_alloc(RT_COLLIDER3D_TYPE_SPHERE);
    if (!collider)
        return NULL;
    collider->radius = collider3d_extent_or_unit(radius);
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Construct a Y-axis capsule collider with total `height` including hemispherical caps.
/// Values below the capsule diameter collapse to a sphere-like capsule.
/// @param radius Capsule radius, sanitized to a positive supported extent.
/// @param height Total tip-to-tip height, floored to twice the sanitized radius.
/// @return Newly allocated capsule collider, or NULL on allocation failure.
void *rt_collider3d_new_capsule(double radius, double height) {
    rt_collider3d *collider = collider3d_alloc(RT_COLLIDER3D_TYPE_CAPSULE);
    if (!collider)
        return NULL;
    collider->radius = collider3d_extent_or_unit(radius);
    collider->height = collider3d_extent_or_unit(height);
    if (collider->height < collider->radius * 2.0)
        collider->height = collider->radius * 2.0;
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Shared constructor for mesh-backed collider types (convex hull / triangle mesh).
/// @details `rt_collider3d_new_convex_hull` and `rt_collider3d_new_mesh` differ
///   only in their type tag and static-body restriction: convex hulls accept
///   dynamic bodies because GJK works cleanly against them, while triangle
///   meshes are forced static because dynamic triangle-mesh-vs-anything is
///   numerically unstable and performance-prohibitive. Retains the mesh so
///   subsequent mesh mutation (vertex edits) doesn't dangle through this
///   collider, and recomputes local-space AABB bounds immediately so
///   broadphase queries see a valid envelope.
/// @param mesh Mesh3D handle retained as the shape's geometry.
/// @param type Convex-hull or triangle-mesh collider discriminator.
/// @param static_only Nonzero to forbid use on dynamic bodies.
/// @return Retained collider pointer or NULL after `rt_trap` on missing mesh.
static void *collider3d_new_mesh_like(void *mesh, int32_t type, int8_t static_only) {
    rt_collider3d *collider;
    rt_mesh3d *mesh_impl = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh_impl) {
        rt_trap("Collider3D.NewMesh/NewConvexHull: mesh must be non-null");
        return NULL;
    }
    collider = collider3d_alloc(type);
    if (!collider)
        return NULL;
    collider->mesh = mesh_impl;
    collider->static_only = static_only;
    rt_obj_retain_maybe(mesh);
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Wrap a Mesh3D as a *convex hull* collider — the mesh's vertex set is treated as a
/// convex polytope (caller's responsibility to ensure convexity). Suitable for dynamic bodies.
/// @param mesh Mesh3D handle retained as convex support geometry.
/// @return Newly allocated convex-hull collider, or NULL after invalid input or allocation
/// failure.
void *rt_collider3d_new_convex_hull(void *mesh) {
    return collider3d_new_mesh_like(mesh, RT_COLLIDER3D_TYPE_CONVEX_HULL, 0);
}

/// @brief Build a *reduced* convex hull collider from an arbitrary mesh.
/// @details Runs quickhull over the mesh's vertices (so interior vertices of
///   concave art meshes stop costing GJK support scans), then — when the hull
///   still exceeds @p max_verts — reduces the vertex set with farthest-point
///   selection and re-hulls it so the final polytope stays convex and closed.
///   The resulting hull is materialized as a fresh Mesh3D (with faces, so
///   raycasts against the collider keep working) that the collider owns.
///   Clamps @p max_verts to 8–255. Traps on a null or degenerate (flat /
///   collinear) mesh.
/// @param mesh Source Mesh3D whose vertex cloud is hulled.
/// @param max_verts Requested maximum hull vertices, clamped to the inclusive range 8 through 255.
/// @return Newly allocated convex-hull collider owning a reduced mesh, or NULL after failure is
/// reported.
void *rt_collider3d_new_convex_hull_reduced(void *mesh, int64_t max_verts) {
    rt_mesh3d *mesh_impl = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh_impl) {
        rt_trap("Collider3D.NewConvexHullReduced: mesh must be non-null");
        return NULL;
    }
    if (mesh_impl->vertex_count < 4) {
        rt_trap("Collider3D.NewConvexHullReduced: mesh needs at least 4 vertices");
        return NULL;
    }
    if (max_verts < 8)
        max_verts = 8;
    if (max_verts > 255)
        max_verts = 255;

    /* Gather positions (authoritative doubles when present). */
    int32_t n = (int32_t)mesh_impl->vertex_count;
    double *cloud = (double *)malloc(sizeof(double) * 3u * (size_t)n);
    if (!cloud) {
        rt_trap("Collider3D.NewConvexHullReduced: allocation failed");
        return NULL;
    }
    for (int32_t i = 0; i < n; i++) {
        if (mesh_impl->positions64) {
            cloud[i * 3 + 0] = mesh_impl->positions64[i * 3 + 0];
            cloud[i * 3 + 1] = mesh_impl->positions64[i * 3 + 1];
            cloud[i * 3 + 2] = mesh_impl->positions64[i * 3 + 2];
        } else {
            cloud[i * 3 + 0] = (double)mesh_impl->vertices[i].pos[0];
            cloud[i * 3 + 1] = (double)mesh_impl->vertices[i].pos[1];
            cloud[i * 3 + 2] = (double)mesh_impl->vertices[i].pos[2];
        }
    }

    double *hull_verts = NULL;
    int32_t hull_vert_count = 0;
    int32_t *hull_indices = NULL;
    int32_t hull_index_count = 0;
    int ok = rt_quickhull3d_build(
        cloud, n, &hull_verts, &hull_vert_count, &hull_indices, &hull_index_count);
    free(cloud);
    if (!ok) {
        rt_trap("Collider3D.NewConvexHullReduced: mesh is degenerate (flat or collinear)");
        return NULL;
    }

    /* Reduce and re-hull when the exact hull is still too rich. */
    if (hull_vert_count > (int32_t)max_verts) {
        double *reduced = (double *)malloc(sizeof(double) * 3u * (size_t)max_verts);
        if (!reduced) {
            free(hull_verts);
            free(hull_indices);
            rt_trap("Collider3D.NewConvexHullReduced: allocation failed");
            return NULL;
        }
        int32_t reduced_count =
            rt_quickhull3d_reduce(hull_verts, hull_vert_count, (int32_t)max_verts, reduced);
        free(hull_verts);
        free(hull_indices);
        hull_verts = NULL;
        hull_indices = NULL;
        ok = reduced_count >= 4 && rt_quickhull3d_build(reduced,
                                                        reduced_count,
                                                        &hull_verts,
                                                        &hull_vert_count,
                                                        &hull_indices,
                                                        &hull_index_count);
        free(reduced);
        if (!ok) {
            rt_trap("Collider3D.NewConvexHullReduced: hull reduction failed");
            return NULL;
        }
    }

    /* Materialize the hull as a fresh mesh the collider owns. */
    void *hull_mesh = rt_mesh3d_new();
    if (!hull_mesh) {
        free(hull_verts);
        free(hull_indices);
        rt_trap("Collider3D.NewConvexHullReduced: allocation failed");
        return NULL;
    }
    for (int32_t i = 0; i < hull_vert_count; i++) {
        rt_mesh3d_add_vertex(hull_mesh,
                             hull_verts[i * 3 + 0],
                             hull_verts[i * 3 + 1],
                             hull_verts[i * 3 + 2],
                             0.0,
                             1.0,
                             0.0,
                             0.0,
                             0.0);
    }
    for (int32_t i = 0; i + 2 < hull_index_count; i += 3) {
        rt_mesh3d_add_triangle(
            hull_mesh, hull_indices[i], hull_indices[i + 1], hull_indices[i + 2]);
    }
    rt_mesh3d_recalc_normals(hull_mesh);
    free(hull_verts);
    free(hull_indices);

    void *collider = collider3d_new_mesh_like(hull_mesh, RT_COLLIDER3D_TYPE_CONVEX_HULL, 0);
    /* The collider retains the hull mesh; drop the constructor's reference. */
    if (rt_obj_release_check0(hull_mesh))
        rt_obj_free(hull_mesh);
    return collider;
}

/// @brief Wrap a Mesh3D as a *triangle-mesh* collider — uses every triangle for collision tests.
/// Marked static-only: cannot be attached to dynamic rigid bodies (use the mesh as level geometry).
/// @param mesh Mesh3D handle retained as triangle geometry.
/// @return Newly allocated static-only mesh collider, or NULL after invalid input or allocation
/// failure.
void *rt_collider3d_new_mesh(void *mesh) {
    return collider3d_new_mesh_like(mesh, RT_COLLIDER3D_TYPE_MESH, 1);
}

/// @brief Build a heightfield collider from a Pixels heightmap. Heights are decoded with 16-bit
/// precision (R = high byte, G = low byte) into [0, 1] then scaled by `scale_y`. `scale_x` and
/// `scale_z` are the per-cell spacing in world units. Static-only. Traps on invalid heightmap
/// (< 2×2 or null buffer).
/// @param heightmap Pixels handle containing at least a two-by-two 16-bit RG height grid.
/// @param scale_x X-axis spacing between adjacent samples.
/// @param scale_y Height multiplier applied to decoded normalized samples.
/// @param scale_z Z-axis spacing between adjacent samples.
/// @return Newly allocated static-only heightfield collider, or NULL after invalid input or
/// allocation failure is reported.
void *rt_collider3d_new_heightfield(void *heightmap,
                                    double scale_x,
                                    double scale_y,
                                    double scale_z) {
    const uint32_t *raw;
    int64_t width;
    int64_t height;
    size_t sample_count;
    rt_collider3d *collider;
    if (!heightmap) {
        rt_trap("Collider3D.NewHeightfield: heightmap must be non-null");
        return NULL;
    }
    if (!rt_pixels_checked_impl_or_null(heightmap)) {
        rt_trap("Collider3D.NewHeightfield: heightmap must be a valid Pixels object");
        return NULL;
    }
    width = rt_pixels_width(heightmap);
    height = rt_pixels_height(heightmap);
    raw = rt_pixels_raw_buffer(heightmap);
    if (width < 2 || height < 2 || width > INT32_MAX || height > INT32_MAX || !raw) {
        rt_trap("Collider3D.NewHeightfield: heightmap must be a valid Pixels object");
        return NULL;
    }
    if ((uint64_t)width > SIZE_MAX / (uint64_t)height / sizeof(float)) {
        rt_trap("Collider3D.NewHeightfield: heightmap is too large");
        return NULL;
    }
    sample_count = (size_t)width * (size_t)height;
    collider = collider3d_alloc(RT_COLLIDER3D_TYPE_HEIGHTFIELD);
    if (!collider)
        return NULL;
    collider->static_only = 1;
    collider->heightfield_width = (int32_t)width;
    collider->heightfield_depth = (int32_t)height;
    collider->heightfield_scale[0] = collider3d_scale_or_unit(scale_x);
    collider->heightfield_scale[1] = collider3d_scale_or_unit(scale_y);
    collider->heightfield_scale[2] = collider3d_scale_or_unit(scale_z);
    collider->heightfield_heights = (float *)calloc(sample_count, sizeof(float));
    if (!collider->heightfield_heights) {
        if (rt_obj_release_check0(collider))
            rt_obj_free(collider);
        rt_trap("Collider3D.NewHeightfield: allocation failed");
        return NULL;
    }
    collider->heightfield_min = DBL_MAX;
    collider->heightfield_max = -DBL_MAX;
    for (int64_t z = 0; z < height; ++z) {
        for (int64_t x = 0; x < width; ++x) {
            size_t sample_index = (size_t)z * (size_t)width + (size_t)x;
            uint32_t pixel = raw[sample_index];
            uint32_t hi = (pixel >> 24) & 0xFFu;
            uint32_t lo = (pixel >> 16) & 0xFFu;
            double h = (double)((hi << 8) | lo) / 65535.0;
            collider->heightfield_heights[sample_index] = (float)h;
            if (h < collider->heightfield_min)
                collider->heightfield_min = h;
            if (h > collider->heightfield_max)
                collider->heightfield_max = h;
        }
    }
    if (collider->heightfield_min == DBL_MAX) {
        collider->heightfield_min = 0.0;
        collider->heightfield_max = 0.0;
    }
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Construct an empty compound collider — a container holding child colliders, each
/// with its own local transform. Use `_add_child` to populate, then attach to a rigid body.
/// @return Newly allocated empty compound collider, or NULL on allocation failure.
void *rt_collider3d_new_compound(void) {
    rt_collider3d *collider = collider3d_alloc(RT_COLLIDER3D_TYPE_COMPOUND);
    if (!collider)
        return NULL;
    collider3d_recompute_bounds(collider);
    return collider;
}

/// @brief Grow a compound collider's child arrays without partial mutation.
/// @details The child handle array and child-transform sidecar must grow together. Allocating both
///          replacements before committing avoids leaving the compound with one grown array and one
///          old array if the second allocation fails.
/// @param compound Compound collider whose child storage should grow.
/// @param new_capacity Replacement capacity, already overflow-checked by the caller.
/// @return Non-zero on success; zero after trapping on allocation failure.
static int collider3d_grow_compound_children(rt_collider3d *compound, int32_t new_capacity) {
    void **new_children;
    rt_collider3d_child *new_transforms;
    size_t live_count;
    if (!compound || new_capacity <= compound->child_capacity)
        return 1;
    new_children = (void **)calloc((size_t)new_capacity, sizeof(*new_children));
    new_transforms = (rt_collider3d_child *)calloc((size_t)new_capacity, sizeof(*new_transforms));
    if (!new_children || !new_transforms) {
        free(new_children);
        free(new_transforms);
        rt_trap("Collider3D.AddChild: allocation failed");
        return 0;
    }
    live_count = (size_t)compound->child_count;
    if (live_count > 0) {
        memcpy(new_children, compound->children, live_count * sizeof(*new_children));
        memcpy(new_transforms, compound->child_transforms, live_count * sizeof(*new_transforms));
    }
    free(compound->children);
    free(compound->child_transforms);
    compound->children = new_children;
    compound->child_transforms = new_transforms;
    compound->child_capacity = new_capacity;
    return 1;
}

/// @brief Append a child collider to a compound, transformed by `local_transform` (a Transform3D).
/// Children are retained for the compound's lifetime. Recomputes the compound's AABB to enclose
/// the new child. Traps if the parent isn't compound, child is null, or self-reference is
/// attempted.
/// @param compound_obj Compound Collider3D handle to modify.
/// @param child_obj Collider3D handle retained in the next child slot.
/// @param local_transform Optional Transform3D defining child-to-compound position, rotation, and
/// scale.
void rt_collider3d_add_child(void *compound_obj, void *child_obj, void *local_transform) {
    rt_collider3d *compound = collider3d_checked(compound_obj);
    rt_collider3d *child = collider3d_checked(child_obj);
    int32_t new_capacity;
    int too_deep = 0;
    if (!compound)
        return;
    if (compound->type != RT_COLLIDER3D_TYPE_COMPOUND) {
        rt_trap("Collider3D.AddChild: target collider is not compound");
        return;
    }
    if (!child) {
        rt_trap("Collider3D.AddChild: child collider must be non-null");
        return;
    }
    if (local_transform && !rt_g3d_has_class(local_transform, RT_G3D_TRANSFORM3D_CLASS_ID)) {
        rt_trap("Collider3D.AddChild: local_transform must be a Transform3D");
        return;
    }
    if (child == compound) {
        rt_trap("Collider3D.AddChild: a collider cannot contain itself");
        return;
    }
    if (collider3d_contains_child(child, compound, 2, &too_deep)) {
        rt_trap("Collider3D.AddChild: adding this child would create a cycle");
        return;
    }
    if (too_deep) {
        rt_trap("Collider3D.AddChild: compound nesting exceeds 64 levels");
        return;
    }
    if (compound->child_count >= compound->child_capacity) {
        if (compound->child_capacity >= INT32_MAX / 2) {
            rt_trap("Collider3D.AddChild: too many children");
            return;
        }
        new_capacity = compound->child_capacity > 0 ? compound->child_capacity * 2 : 4;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(void *) ||
            (size_t)new_capacity > SIZE_MAX / sizeof(rt_collider3d_child)) {
            rt_trap("Collider3D.AddChild: allocation overflow");
            return;
        }
        if (!collider3d_grow_compound_children(compound, new_capacity))
            return;
    }
    rt_obj_retain_maybe(child_obj);
    compound->children[compound->child_count] = child;
    collider3d_set_from_transform(&compound->child_transforms[compound->child_count],
                                  local_transform);
    compound->child_count++;
    collider3d_recompute_bounds(compound);
    collider3d_note_global_geometry_change();
}

/// @brief Return the collider's discriminator (RT_COLLIDER3D_TYPE_BOX, _SPHERE, ...). -1 if NULL.
/// @param collider Collider3D handle to inspect.
/// @return Valid shape discriminator, or -1 for an invalid handle or corrupt type.
int64_t rt_collider3d_get_type(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    return (shape && collider3d_type_is_valid(shape->type)) ? shape->type : -1;
}

/// @brief Return the AABB minimum corner in *local* space as a fresh Vec3. Returns origin
/// for a NULL handle. Re-derives the bounds from the underlying shape data first.
/// @param collider Collider3D handle to query.
/// @return Newly allocated Vec3 containing the refreshed local minimum, or the origin for an
/// invalid handle.
void *rt_collider3d_get_local_bounds_min(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return rt_vec3_new(0.0, 0.0, 0.0);
    collider3d_recompute_bounds(shape);
    return rt_vec3_new(shape->bounds_min[0], shape->bounds_min[1], shape->bounds_min[2]);
}

/// @brief Return the AABB maximum corner in *local* space as a fresh Vec3.
/// @param collider Collider3D handle to query.
/// @return Newly allocated Vec3 containing the refreshed local maximum, or the origin for an
/// invalid handle.
void *rt_collider3d_get_local_bounds_max(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return rt_vec3_new(0.0, 0.0, 0.0);
    collider3d_recompute_bounds(shape);
    return rt_vec3_new(shape->bounds_max[0], shape->bounds_max[1], shape->bounds_max[2]);
}

/// @brief Internal: write the local AABB into the two raw double[3] arrays. Faster than the
/// Vec3-returning getters when the physics core needs the bounds many times per frame.
/// @param collider Collider3D handle to query.
/// @param min_out Required three-component destination for the local minimum.
/// @param max_out Required three-component destination for the local maximum.
void rt_collider3d_get_local_bounds_raw(void *collider, double *min_out, double *max_out) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!min_out || !max_out) {
        return;
    }
    if (!shape) {
        vec3_set(min_out, 0.0, 0.0, 0.0);
        vec3_set(max_out, 0.0, 0.0, 0.0);
        return;
    }
    collider3d_recompute_bounds(shape);
    vec3_copy(min_out, shape->bounds_min);
    vec3_copy(max_out, shape->bounds_max);
}

/// @brief Return the collider's bounds revision counter (recomputing bounds first if stale).
/// @details The counter changes whenever the world AABB changes, letting the broadphase detect
///          movement without re-reading the bounds themselves.
/// @param collider Collider3D handle to inspect.
/// @return Refreshed nonzero bounds revision, or zero for an invalid handle.
uint64_t rt_collider3d_get_bounds_revision_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return 0;
    collider3d_recompute_bounds(shape);
    return shape->bounds_revision;
}

/// @brief Internal: transform the local AABB by (position, rotation quat, scale) and write the
/// resulting world-space AABB into `min_out` / `max_out`. Defaults: zero pos, identity rotation,
/// unit scale when individual params are NULL.
/// @param collider Collider3D handle whose local bounds are transformed.
/// @param position Optional three-component world translation.
/// @param rotation Optional unit XYZW world rotation.
/// @param scale Optional three-component world scale.
/// @param min_out Required output receiving the world-space AABB minimum.
/// @param max_out Required output receiving the world-space AABB maximum.
void rt_collider3d_compute_world_aabb_raw(void *collider,
                                          const double *position,
                                          const double *rotation,
                                          const double *scale,
                                          double *min_out,
                                          double *max_out) {
    double local_min[3];
    double local_max[3];
    double identity_rotation[4];
    double unit_scale[3];
    if (!min_out || !max_out)
        return;
    rt_collider3d_get_local_bounds_raw(collider, local_min, local_max);
    quat_identity(identity_rotation);
    vec3_set(unit_scale, 1.0, 1.0, 1.0);
    if (!position) {
        double zero_pos[3] = {0.0, 0.0, 0.0};
        transform_bounds_raw(local_min,
                             local_max,
                             zero_pos,
                             rotation ? rotation : identity_rotation,
                             scale ? scale : unit_scale,
                             min_out,
                             max_out);
        return;
    }
    transform_bounds_raw(local_min,
                         local_max,
                         position,
                         rotation ? rotation : identity_rotation,
                         scale ? scale : unit_scale,
                         min_out,
                         max_out);
}

/// @brief Internal: 1 if the collider can only be used on static bodies (mesh, heightfield).
/// @param collider Collider3D handle to inspect.
/// @return One for a valid static-only collider, otherwise zero.
int8_t rt_collider3d_is_static_only_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    return (shape && shape->static_only) ? 1 : 0;
}

/// @brief Internal: fill `half_extents_out[3]` with the box's half-extents. Zeros for non-box.
/// @param collider Collider3D handle to inspect.
/// @param half_extents_out Required three-component output.
void rt_collider3d_get_box_half_extents_raw(void *collider, double *half_extents_out) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!half_extents_out) {
        return;
    }
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_BOX) {
        vec3_set(half_extents_out, 0.0, 0.0, 0.0);
        return;
    }
    shape->half_extents[0] = collider3d_extent_or_unit(shape->half_extents[0]);
    shape->half_extents[1] = collider3d_extent_or_unit(shape->half_extents[1]);
    shape->half_extents[2] = collider3d_extent_or_unit(shape->half_extents[2]);
    vec3_copy(half_extents_out, shape->half_extents);
}

/// @brief Reset an existing box collider's dimensions for reusable internal query shapes.
/// @param collider Box Collider3D handle to mutate.
/// @param hx Replacement X-axis half-extent.
/// @param hy Replacement Y-axis half-extent.
/// @param hz Replacement Z-axis half-extent.
void rt_collider3d_reset_box_raw(void *collider, double hx, double hy, double hz) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_BOX)
        return;
    shape->half_extents[0] = collider3d_extent_or_unit(hx);
    shape->half_extents[1] = collider3d_extent_or_unit(hy);
    shape->half_extents[2] = collider3d_extent_or_unit(hz);
    collider3d_recompute_bounds(shape);
}

/// @brief Internal: sphere/capsule radius. Returns 0 for unsupported shapes.
/// @param collider Collider3D handle to inspect.
/// @return Sanitized radius for a sphere or capsule, otherwise zero.
double rt_collider3d_get_radius_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return 0.0;
    if (shape->type != RT_COLLIDER3D_TYPE_SPHERE && shape->type != RT_COLLIDER3D_TYPE_CAPSULE)
        return 0.0;
    return collider3d_extent_or_unit(shape->radius);
}

/// @brief Reset an existing sphere collider's radius for reusable internal query shapes.
/// @param collider Sphere Collider3D handle to mutate.
/// @param radius Replacement radius.
void rt_collider3d_reset_sphere_raw(void *collider, double radius) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_SPHERE)
        return;
    shape->radius = collider3d_extent_or_unit(radius);
    collider3d_recompute_bounds(shape);
}

/// @brief Reset an existing capsule collider's radius/height and cached bounds
///        (crouch/stand resizes and reusable internal query shapes). Height
///        includes both hemispherical caps and is floored to 2*radius.
/// @param collider Capsule Collider3D handle to mutate.
/// @param radius Replacement radius.
/// @param height Replacement total tip-to-tip height.
void rt_collider3d_reset_capsule_raw(void *collider, double radius, double height) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_CAPSULE)
        return;
    shape->radius = collider3d_extent_or_unit(radius);
    shape->height = collider3d_extent_or_unit(height);
    if (shape->height < shape->radius * 2.0)
        shape->height = shape->radius * 2.0;
    collider3d_recompute_bounds(shape);
}

/// @brief Internal: capsule total height including hemispherical caps. 0 for non-capsule.
/// @param collider Collider3D handle to inspect.
/// @return Sanitized capsule height, or zero for any other shape.
double rt_collider3d_get_height_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return 0.0;
    if (shape->type != RT_COLLIDER3D_TYPE_CAPSULE)
        return 0.0;
    return collider3d_extent_or_unit(shape->height);
}

/// @brief Internal: borrow the underlying Mesh3D for convex-hull / triangle-mesh colliders.
/// Returns NULL for primitive shapes. Caller must NOT release — collider retains ownership.
/// @param collider Collider3D handle to inspect.
/// @return Borrowed Mesh3D payload for a valid mesh-backed shape, otherwise NULL.
void *rt_collider3d_get_mesh_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape)
        return NULL;
    if (shape->type != RT_COLLIDER3D_TYPE_CONVEX_HULL && shape->type != RT_COLLIDER3D_TYPE_MESH)
        return NULL;
    return collider3d_mesh_or_null(shape);
}

/// @brief Internal: number of child colliders in a compound. 0 for non-compound shapes.
/// @param collider Collider3D handle to inspect.
/// @return Safely bounded child count, or zero for a non-compound or invalid shape.
int64_t rt_collider3d_get_child_count_raw(void *collider) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_COMPOUND)
        return 0;
    return collider3d_safe_child_count(shape, 0);
}

/// @brief Internal: borrow the i-th child collider from a compound. NULL if out of range.
/// @param collider Compound Collider3D handle to inspect.
/// @param index Zero-based child index.
/// @return Borrowed child collider payload, or NULL when unavailable.
void *rt_collider3d_get_child_raw(void *collider, int64_t index) {
    rt_collider3d *shape = collider3d_checked(collider);
    int32_t child_count;
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_COMPOUND)
        return NULL;
    child_count = collider3d_safe_child_count(shape, 0);
    if (index < 0 || index >= child_count)
        return NULL;
    return collider3d_checked(shape->children[index]);
}

/// @brief Internal: copy the i-th compound child's local TRS into the output buffers
/// (position_out[3], rotation_out[4] quaternion, scale_out[3]). Outputs default to identity.
/// @param compound Compound Collider3D handle to inspect.
/// @param index Zero-based child index.
/// @param position_out Optional three-component position output.
/// @param rotation_out Optional four-component XYZW rotation output.
/// @param scale_out Optional three-component scale output.
void rt_collider3d_get_child_transform_raw(
    void *compound, int64_t index, double *position_out, double *rotation_out, double *scale_out) {
    rt_collider3d *shape = collider3d_checked(compound);
    if (position_out)
        vec3_set(position_out, 0.0, 0.0, 0.0);
    if (rotation_out)
        quat_identity(rotation_out);
    if (scale_out)
        vec3_set(scale_out, 1.0, 1.0, 1.0);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_COMPOUND)
        return;
    int32_t child_count = collider3d_safe_child_count(shape, 1);
    if (index < 0 || index >= child_count)
        return;
    if (position_out)
        vec3_copy(position_out, shape->child_transforms[index].position);
    if (rotation_out) {
        memcpy(rotation_out, shape->child_transforms[index].rotation, sizeof(double) * 4);
        quat_normalize_local(rotation_out);
    }
    if (scale_out) {
        scale_out[0] =
            collider3d_transform_component_or(shape->child_transforms[index].scale[0], 1.0);
        scale_out[1] =
            collider3d_transform_component_or(shape->child_transforms[index].scale[1], 1.0);
        scale_out[2] =
            collider3d_transform_component_or(shape->child_transforms[index].scale[2], 1.0);
    }
}

/// @brief Clamped heightfield cell fetch for nearest-neighbor sampling.
/// @details Heights are stored row-major as `heights[z * width + x]` after
///   decoding the 16-bit-per-pixel heightmap. Out-of-range indices clamp to
///   the nearest edge cell (effective "repeat edge" wrap) so the bilinear
///   sampler that wraps this helper never produces a discontinuity at the
///   heightfield boundary — it just extends the rim height outward. Null
///   colliders or uninitialized heightfields return 0.0 as a safe fallback.
/// @param collider Heightfield collider to sample.
/// @param x Integer sample-column index, clamped to the nearest edge.
/// @param z Integer sample-row index, clamped to the nearest edge.
/// @return Stored normalized height, or zero for unavailable storage.
static double collider3d_heightfield_height_at(const rt_collider3d *collider,
                                               int32_t x,
                                               int32_t z) {
    if (!collider || !collider->heightfield_heights || collider->heightfield_width <= 0 ||
        collider->heightfield_depth <= 0)
        return 0.0;
    if (x < 0)
        x = 0;
    if (z < 0)
        z = 0;
    if (x >= collider->heightfield_width)
        x = collider->heightfield_width - 1;
    if (z >= collider->heightfield_depth)
        z = collider->heightfield_depth - 1;
    return collider->heightfield_heights[z * collider->heightfield_width + x];
}

/// @brief Install (or clear) a heightfield hole bitmask (bit per cell, row-major).
/// @details The mask is copied. Cell dimensions must match the heightfield's
///   (width-1) x (depth-1) grid; a NULL mask clears holes. Internal handoff from
///   Terrain3D.SetHole via rt_terrain3d_get_hole_mask_raw.
/// @param collider Heightfield Collider3D handle to modify.
/// @param mask Optional packed row-major cell bits to copy; NULL clears all holes.
/// @param cells_x Number of cell columns represented by @p mask.
/// @param cells_z Number of cell rows represented by @p mask.
/// @return 1 on success, 0 for a non-heightfield collider or dimension mismatch.
int8_t rt_collider3d_heightfield_set_holes_raw(void *collider,
                                               const uint8_t *mask,
                                               int32_t cells_x,
                                               int32_t cells_z) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_HEIGHTFIELD)
        return 0;
    if (!mask) {
        free(shape->heightfield_holes);
        shape->heightfield_holes = NULL;
        return 1;
    }
    if (cells_x != shape->heightfield_width - 1 || cells_z != shape->heightfield_depth - 1 ||
        cells_x <= 0 || cells_z <= 0)
        return 0;
    size_t mask_bytes = ((size_t)cells_x * (size_t)cells_z + 7u) / 8u;
    uint8_t *copy = (uint8_t *)malloc(mask_bytes);
    if (!copy)
        return 0;
    memcpy(copy, mask, mask_bytes);
    free(shape->heightfield_holes);
    shape->heightfield_holes = copy;
    return 1;
}

/// @brief Per-collider friction override (-1 = unset: the body's friction applies).
/// @param obj Collider3D handle to modify.
/// @param friction Non-negative override capped at 16; negative or non-finite values clear it.
void rt_collider3d_set_friction(void *obj, double friction) {
    rt_collider3d *shape = collider3d_checked(obj);
    if (!shape)
        return;
    if (!isfinite(friction) || friction < 0.0)
        shape->material_friction = -1.0;
    else
        shape->material_friction = friction > 16.0 ? 16.0 : friction;
}

/// @brief Per-collider friction override, or -1 when unset.
/// @param obj Collider3D handle to inspect.
/// @return Stored friction override, or -1 when unset or invalid.
double rt_collider3d_get_friction(void *obj) {
    rt_collider3d *shape = collider3d_checked(obj);
    return shape ? shape->material_friction : -1.0;
}

/// @brief Per-collider restitution override (-1 = unset: the body's value applies).
/// @param obj Collider3D handle to modify.
/// @param restitution Override clamped to `[0,1]`; negative or non-finite values clear it.
void rt_collider3d_set_restitution(void *obj, double restitution) {
    rt_collider3d *shape = collider3d_checked(obj);
    if (!shape)
        return;
    if (!isfinite(restitution) || restitution < 0.0)
        shape->material_restitution = -1.0;
    else
        shape->material_restitution = restitution > 1.0 ? 1.0 : restitution;
}

/// @brief Per-collider restitution override, or -1 when unset.
/// @param obj Collider3D handle to inspect.
/// @return Stored restitution override, or -1 when unset or invalid.
double rt_collider3d_get_restitution(void *obj) {
    rt_collider3d *shape = collider3d_checked(obj);
    return shape ? shape->material_restitution : -1.0;
}

/// @brief Surface-type tag (Game3D.Surfaces registry id; 0 = untyped).
/// @param obj Collider3D handle to modify.
/// @param surface_type Registry identifier; negative values normalize to zero.
void rt_collider3d_set_surface_type(void *obj, int64_t surface_type) {
    rt_collider3d *shape = collider3d_checked(obj);
    if (shape)
        shape->surface_type = surface_type < 0 ? 0 : surface_type;
}

/// @brief Surface-type tag, 0 when untyped or invalid.
/// @param obj Collider3D handle to inspect.
/// @return Non-negative surface registry identifier, or zero when invalid or untyped.
int64_t rt_collider3d_get_surface_type(void *obj) {
    rt_collider3d *shape = collider3d_checked(obj);
    return shape ? shape->surface_type : 0;
}

/// @brief Internal: effective friction for one contact side (collider override or body).
/// @param collider Collider3D handle whose material override is consulted.
/// @param body_friction Body-level fallback friction.
/// @return Collider override when set, otherwise @p body_friction.
double rt_collider3d_effective_friction_raw(void *collider, double body_friction) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (shape && shape->material_friction >= 0.0)
        return shape->material_friction;
    return body_friction;
}

/// @brief Internal: effective restitution for one contact side.
/// @param collider Collider3D handle whose material override is consulted.
/// @param body_restitution Body-level fallback restitution.
/// @return Collider override when set, otherwise @p body_restitution.
double rt_collider3d_effective_restitution_raw(void *collider, double body_restitution) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (shape && shape->material_restitution >= 0.0)
        return shape->material_restitution;
    return body_restitution;
}

/// @brief Bilinearly sample a heightfield at a local-space XZ position.
/// @details The scaled height and non-uniform-scale-correct surface normal are written to optional
/// outputs. Out-of-bounds, holed, invalid, or non-finite queries return zero after initializing
/// outputs to height zero and the upward unit normal.
/// @param collider Heightfield Collider3D handle to sample.
/// @param local_x Local-space X coordinate.
/// @param local_z Local-space Z coordinate.
/// @param height_out Optional output receiving the scaled local Y height.
/// @param normal_out Optional three-component output receiving the unit surface normal.
/// @return One when the query lies on a non-holed heightfield cell, otherwise zero.
int8_t rt_collider3d_sample_heightfield_raw(
    void *collider, double local_x, double local_z, double *height_out, double *normal_out) {
    rt_collider3d *shape = collider3d_checked(collider);
    double half_width;
    double half_depth;
    double grid_x;
    double grid_z;
    int32_t x0;
    int32_t z0;
    int32_t x1;
    int32_t z1;
    double tx;
    double tz;
    double h00;
    double h10;
    double h01;
    double h11;
    double h0;
    double h1;
    double h;
    double dx;
    double dz;
    double normal[3];
    double normal_len;
    if (height_out)
        *height_out = 0.0;
    if (normal_out) {
        normal_out[0] = 0.0;
        normal_out[1] = 1.0;
        normal_out[2] = 0.0;
    }
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_HEIGHTFIELD || !shape->heightfield_heights)
        return 0;
    if (!isfinite(local_x) || !isfinite(local_z))
        return 0;
    shape->heightfield_scale[0] = collider3d_scale_or_unit(shape->heightfield_scale[0]);
    shape->heightfield_scale[1] = collider3d_scale_or_unit(shape->heightfield_scale[1]);
    shape->heightfield_scale[2] = collider3d_scale_or_unit(shape->heightfield_scale[2]);

    half_width = ((double)(shape->heightfield_width - 1) * shape->heightfield_scale[0]) * 0.5;
    half_depth = ((double)(shape->heightfield_depth - 1) * shape->heightfield_scale[2]) * 0.5;
    if (shape->heightfield_scale[0] <= 1e-12 || shape->heightfield_scale[2] <= 1e-12)
        return 0;

    grid_x = (local_x + half_width) / shape->heightfield_scale[0];
    grid_z = (local_z + half_depth) / shape->heightfield_scale[2];
    if (!isfinite(grid_x) || !isfinite(grid_z))
        return 0;
    if (grid_x < 0.0 || grid_z < 0.0 || grid_x > (double)(shape->heightfield_width - 1) ||
        grid_z > (double)(shape->heightfield_depth - 1))
        return 0;

    x0 = (int32_t)floor(grid_x);
    z0 = (int32_t)floor(grid_z);
    if (shape->heightfield_holes) {
        /* Holed cells report no surface: physics falls through the carved pit. */
        int32_t cells_x = shape->heightfield_width - 1;
        int32_t cells_z = shape->heightfield_depth - 1;
        int32_t hx = x0 < 0 ? 0 : (x0 >= cells_x ? cells_x - 1 : x0);
        int32_t hz = z0 < 0 ? 0 : (z0 >= cells_z ? cells_z - 1 : z0);
        if (cells_x > 0 && cells_z > 0) {
            int64_t bit = (int64_t)hz * cells_x + hx;
            if ((shape->heightfield_holes[bit >> 3] >> (bit & 7)) & 1)
                return 0;
        }
    }
    x1 = x0 + 1;
    z1 = z0 + 1;
    if (x1 >= shape->heightfield_width)
        x1 = shape->heightfield_width - 1;
    if (z1 >= shape->heightfield_depth)
        z1 = shape->heightfield_depth - 1;
    tx = clampd(grid_x - (double)x0, 0.0, 1.0);
    tz = clampd(grid_z - (double)z0, 0.0, 1.0);
    h00 = collider3d_heightfield_height_at(shape, x0, z0);
    h10 = collider3d_heightfield_height_at(shape, x1, z0);
    h01 = collider3d_heightfield_height_at(shape, x0, z1);
    h11 = collider3d_heightfield_height_at(shape, x1, z1);
    h0 = h00 + (h10 - h00) * tx;
    h1 = h01 + (h11 - h01) * tx;
    h = h0 + (h1 - h0) * tz;
    if (height_out)
        *height_out = collider3d_transform_component_or(h * shape->heightfield_scale[1], 0.0);

    // Finite-difference heightfield normal. The Y component must carry both
    // horizontal scales; the X/Z components each carry the perpendicular
    // horizontal scale so the normal is correct under non-uniform scaling.
    // Previously normal[1] used only heightfield_scale[0], which produced
    // wrong slope responses for stretched heightfields.
    dx = ((h10 - h00) * (1.0 - tz) + (h11 - h01) * tz) * shape->heightfield_scale[1];
    dz = ((h01 - h00) * (1.0 - tx) + (h11 - h10) * tx) * shape->heightfield_scale[1];
    normal[0] = -dx * shape->heightfield_scale[2];
    normal[1] = shape->heightfield_scale[0] * shape->heightfield_scale[2];
    normal[2] = -dz * shape->heightfield_scale[0];
    normal_len = sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (isfinite(normal_len) && normal_len > 1e-12) {
        normal[0] /= normal_len;
        normal[1] /= normal_len;
        normal[2] /= normal_len;
    } else {
        normal[0] = 0.0;
        normal[1] = 1.0;
        normal[2] = 0.0;
    }
    if (normal_out) {
        normal_out[0] = normal[0];
        normal_out[1] = normal[1];
        normal_out[2] = normal[2];
    }
    return 1;
}

/// @brief Report a heightfield collider's grid width, depth, and horizontal scale.
/// @param collider Heightfield Collider3D handle to inspect.
/// @param width_out Optional output receiving sample-column count.
/// @param depth_out Optional output receiving sample-row count.
/// @param scale_out Optional three-component output receiving sanitized sample scale.
/// @return 1 with the out-params set, or 0 if the collider is not a heightfield.
int8_t rt_collider3d_get_heightfield_info_raw(void *collider,
                                              int32_t *width_out,
                                              int32_t *depth_out,
                                              double *scale_out) {
    rt_collider3d *shape = collider3d_checked(collider);
    if (!shape || shape->type != RT_COLLIDER3D_TYPE_HEIGHTFIELD || !shape->heightfield_heights)
        return 0;
    if (shape->heightfield_width < 2 || shape->heightfield_depth < 2)
        return 0;
    if (width_out)
        *width_out = shape->heightfield_width;
    if (depth_out)
        *depth_out = shape->heightfield_depth;
    if (scale_out) {
        scale_out[0] = collider3d_scale_or_unit(shape->heightfield_scale[0]);
        scale_out[1] = collider3d_scale_or_unit(shape->heightfield_scale[1]);
        scale_out[2] = collider3d_scale_or_unit(shape->heightfield_scale[2]);
    }
    return 1;
}

#else
typedef int rt_collider3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
