//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_morphtarget3d.h
// Purpose: MorphTarget3D — per-vertex position/normal/tangent deltas blended by
//   weight. Enables facial animation, muscle flex, and shape-based deformation.
//
// Key invariants:
//   - Shape storage grows on demand; GPU backends may still fall back to CPU
//     morphing when the active shape count exceeds backend shader limits.
//   - vertex_count must match the mesh's vertex count at draw time.
//   - Morphed vertices are applied on GPU when supported, otherwise on CPU.
//   - Normal/tangent deltas are optional per shape (NULL = channel unchanged).
//
// Ownership/Lifetime:
//   - MorphTarget3D objects are GC-managed and own every shape name/delta array.
//   - Mesh bindings retain the container; packed GPU views remain borrowed.
//
// Links: plans/3d/16-morph-targets.md, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for mesh morph targets and morphed draw submission.
/// @details Every shape owns a required shape-major position-delta channel and
///          optional lazily allocated normal and tangent XYZ channels. Signed
///          weights are clamped to `[-1,1]`. Packed GPU payloads remain
///          container-owned and are rebuilt lazily after delta mutation;
///          backends fall back to tracked CPU deformation for unsupported
///          payloads, excessive shape counts, or tangent deltas.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a morph-target container for blendshape animation.
/// @param[in] vertex_count Positive fixed vertex count shared by every shape;
///                         must fit signed 32-bit and allocation-size bounds.
/// @return New GC-managed MorphTarget3D, or `NULL` after a runtime trap for
///         invalid size or allocation failure.
void *rt_morphtarget3d_new(int64_t vertex_count);
/// @brief Deep-copy a morph-target container, including shapes, deltas, and weights.
/// @details Optional channels and current, previous, snapshot, and frame-history
///          state are copied into independent storage after finite/canonical
///          repair. Packed payloads are rebuilt lazily in the clone.
/// @param[in] mt Source MorphTarget3D.
/// @return New independent GC-managed clone, or `NULL` for an invalid source,
///         size error, or allocation failure.
void *rt_morphtarget3d_clone(void *mt);
/// @brief Deep copy with every position / normal / tangent delta reflected across
///        X = 0 (ADR 0306 `Mesh3D.Mirror`): the X lane of each delta is negated,
///        names, weights and the remaining lanes are copied verbatim.
/// @param mt Borrowed MorphTarget3D handle.
/// @return New GC-managed MorphTarget3D, or NULL on invalid input / allocation failure.
void *rt_morphtarget3d_clone_mirrored_x(void *mt);
/// @brief Internal (tests): one lane of a shape's position delta.
/// @param lane 0 = x, 1 = y, 2 = z.
/// @return The delta lane, or 0.0 for any invalid handle / index.
double rt_morphtarget3d_delta_lane_internal(void *mt, int64_t shape, int64_t vertex, int64_t lane);
/**
 * @brief Deep-copy a morph-target container through a simplified vertex map.
 *
 * Each output vertex `i` receives the position, normal, and tangent deltas from
 * source vertex `new_to_old[i]` for every shape. Shape names, current/previous
 * weights, motion snapshots, and motion-history state are preserved. The result
 * owns independent arrays and has exactly @p new_vertex_count vertices.
 *
 * @param mt Source MorphTarget3D handle.
 * @param new_to_old Borrowed output-to-source vertex index map.
 * @param new_vertex_count Number of output vertices and entries in @p new_to_old.
 * @return A new MorphTarget3D, or `NULL` for an invalid map/handle or allocation
 * failure. The source remains unchanged.
 */
void *rt_morphtarget3d_clone_remapped(void *mt,
                                      const uint32_t *new_to_old,
                                      uint32_t new_vertex_count);
/// @brief Register a named blendshape and allocate its delta arrays. Returns the new shape index.
/// @details The name is copied and truncated to 63 bytes; null or empty names
///          and duplicate names are accepted. The required position channel is
///          zero-initialized, while normal and tangent channels remain lazy.
/// @param[in,out] mt MorphTarget3D to extend.
/// @param[in] name Borrowed runtime name, or `NULL`.
/// @return Newly appended zero-based shape index, or `-1` for invalid input,
///         size/count overflow, or allocation failure.
int64_t rt_morphtarget3d_add_shape(void *mt, rt_string name);
/// @brief Set position delta for one vertex of one shape.
/// @details Invalid indexes are ignored. Each lane is narrowed to finite float;
///          non-finite or out-of-range values become zero. A changed write
///          invalidates packed payload caches; an identical effective value is
///          a cache-preserving no-op.
/// @param[in,out] mt MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Position-X delta.
/// @param[in] dy Position-Y delta.
/// @param[in] dz Position-Z delta.
void rt_morphtarget3d_set_delta(
    void *mt, int64_t shape, int64_t vertex, double dx, double dy, double dz);
/// @brief Set normal delta for one vertex of one shape (lazy-allocates the per-shape normal array).
/// @details Invalid indexes are ignored. Delta lanes use the same sanitization
///          as position deltas; allocation failure traps and leaves no write. A
///          zero edit against an absent channel remains sparse, and an identical
///          edit preserves payload caches. Morphed normals are renormalized at draw time.
/// @param[in,out] mt MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Normal-X delta.
/// @param[in] dy Normal-Y delta.
/// @param[in] dz Normal-Z delta.
void rt_morphtarget3d_set_normal_delta(
    void *mt, int64_t shape, int64_t vertex, double dx, double dy, double dz);
/// @brief Set tangent delta for one vertex of one shape (lazy-allocates tangent storage).
/// @details Only tangent XYZ is deformed; base handedness W is preserved and
///          the result is renormalized. A zero edit against an absent channel
///          remains sparse, and an identical edit preserves payload caches.
///          Any allocated tangent channel currently forces CPU morphing.
/// @param[in,out] mt MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Tangent-X delta.
/// @param[in] dy Tangent-Y delta.
/// @param[in] dz Tangent-Z delta.
void rt_morphtarget3d_set_tangent_delta(
    void *mt, int64_t shape, int64_t vertex, double dx, double dy, double dz);
/// @brief Set the signed blend weight for a shape by index.
/// @details Values clamp to `[-1,1]`; negative weights invert deltas and
///          non-finite input becomes zero. Invalid indexes are ignored.
/// @param[in,out] mt MorphTarget3D to configure.
/// @param[in] shape Zero-based shape index.
/// @param[in] weight Requested contribution (`0` off, `1` full target).
void rt_morphtarget3d_set_weight(void *mt, int64_t shape, double weight);
/// @brief Get the blend weight of the @p shape-th shape.
/// @param[in,out] mt MorphTarget3D to inspect; the stored lane is sanitized.
/// @param[in] shape Zero-based shape index.
/// @return Finite weight in `[-1,1]`, or zero for invalid input.
double rt_morphtarget3d_get_weight(void *mt, int64_t shape);
/// @brief Set the blend weight for a shape looked up by its name.
/// @details The name is canonicalized to 63 bytes and resolves the first exact
///          stored match. The most recent hit is memoized and revalidated.
///          Missing or empty names are ignored.
/// @param[in,out] mt MorphTarget3D to configure.
/// @param[in] name Borrowed nonempty runtime string.
/// @param[in] weight Requested signed contribution, clamped to `[-1,1]`.
void rt_morphtarget3d_set_weight_by_name(void *mt, rt_string name, double weight);
/// @brief Number of registered shapes.
/// @param[in] mt MorphTarget3D to inspect.
/// @return Sanitized readable shape count, or zero for an invalid handle.
int64_t rt_morphtarget3d_get_shape_count(void *mt);
/// @brief Borrow the packed position-delta array (shape-major) for GPU upload.
/// @details Rebuilds dirty payloads on demand. Layout is
///          `[shape][vertex][xyz]`. The pointer remains owned by the morph
///          target and may be replaced by a later packed-payload query after
///          mutation.
/// @param[in,out] mt MorphTarget3D to inspect.
/// @return Borrowed array of `shape_count * vertex_count * 3` floats, or
///         `NULL` for invalid/empty data or rebuild failure.
const float *rt_morphtarget3d_get_packed_deltas(void *mt);
/// @brief Largest position-delta vector length across every shape and vertex.
/// @details This unweighted value is cached against the payload generation.
/// @param[in,out] mt MorphTarget3D to inspect.
/// @return Largest finite Euclidean delta length, or zero.
double rt_morphtarget3d_get_max_position_delta(void *mt);
/// @brief Borrow the packed normal-delta array, or NULL if no shape has normal deltas.
/// @details Uses the same shape-major layout and borrowed lifetime as
///          @ref rt_morphtarget3d_get_packed_deltas.
/// @param[in,out] mt MorphTarget3D to inspect.
/// @return Borrowed packed normal array, or `NULL`.
const float *rt_morphtarget3d_get_packed_normal_deltas(void *mt);
/// @brief True when any shape carries tangent deltas that require CPU morphing.
/// @param[in] mt MorphTarget3D to inspect.
/// @return `1` when any safe shape owns tangent storage; otherwise `0`.
int64_t rt_morphtarget3d_has_tangent_deltas(void *mt);
/// @brief Blend active shapes over a base vertex array into a destination copy.
/// @details CPU pre-pass so deformation composes morph-then-skin exactly like
///          the GPU vertex path; the CPU-skinning fallback calls it before
///          palette skinning. Both pointers are `vgfx3d_vertex_t` arrays of
///          @p vertex_count entries, typed `void *` to keep this header free
///          of vgfx3d types. The destination is written only on success.
/// @param[in,out] morph MorphTarget3D whose vertex count must match.
/// @param[in] src_vertices Base bind-space vertex array.
/// @param[out] dst_vertices Destination receiving base plus weighted deltas.
/// @param[in] vertex_count Number of vertices in both arrays.
/// @return One when a blend was written; zero when the caller should use the
///         base vertices unchanged.
int8_t rt_morphtarget3d_blend_vertices_internal(void *morph,
                                                const void *src_vertices,
                                                void *dst_vertices,
                                                uint32_t vertex_count);
/// @brief Construct an immutable shape-major procedural morph payload.
/// @details Ownership transfers only on success; both arrays contain
/// `shape_count * vertex_count * 3` floats and are shared by GPU and software paths.
/// @param vertex_count Positive vertex count shared by all shapes.
/// @param shape_count Positive shape count up to 64.
/// @param owned_position_deltas Owned shape-major position deltas.
/// @param owned_normal_deltas Optional owned shape-major normal deltas.
/// @return New MorphTarget3D, or `NULL` with input ownership unchanged.
void *rt_morphtarget3d_new_packed_internal(int64_t vertex_count,
                                           int32_t shape_count,
                                           float *owned_position_deltas,
                                           float *owned_normal_deltas);
/// @brief Nonzero change generation that bumps whenever any delta changes.
/// @details Wrap from `UINT64_MAX` returns to one, deliberately skipping zero
///          so caches can reserve zero for “never observed.”
/// @param[in,out] mt MorphTarget3D to inspect.
/// @return Current nonzero generation, or zero for an invalid handle.
uint64_t rt_morphtarget3d_get_payload_generation(void *mt);
/// @brief Immutable process-local allocation identity for backend cache discrimination.
/// @param[in,out] mt MorphTarget3D to inspect.
/// @return Current nonzero allocation identity, or zero for an invalid handle.
uint64_t rt_morphtarget3d_get_identity_serial(void *mt);
/// @brief Bind a MorphTarget3D to a Mesh3D (vertex counts must match exactly).
/// @details A successful binding retains the container, releases any previous
///          binding, and marks mesh geometry changed. Pass `NULL` to detach;
///          detaching an already-unbound mesh is a no-op. Invalid handles and
///          count mismatches leave the binding unchanged.
/// @param[in,out] mesh Mesh3D to configure.
/// @param[in] morph_targets Borrowed matching MorphTarget3D, or `NULL`.
void rt_mesh3d_set_morph_targets(void *mesh, void *morph_targets);
/// @brief Draw a mesh with morph targets applied (GPU path on Metal/OGL/D3D11, CPU otherwise).
/// @details The transform must be a live Mat4 and also serves as the temporal
///          motion key. Mesh and morph vertex counts must match. Deferred GPU
///          aliases and CPU scratch vertices are tracked on the canvas through
///          frame cleanup. Every accepted draw advances weight history once per
///          frame, including inactive and CPU-fallback draws, so later GPU motion
///          vectors never reuse stale history. A draw rejected during local
///          resource setup leaves history and preexisting frame retains unchanged.
/// @param[in,out] canvas Canvas3D receiving the draw.
/// @param[in] mesh Borrowed resident Mesh3D.
/// @param[in] transform Borrowed Mat4 model transform.
/// @param[in] material Borrowed material handle.
/// @param[in] morph_targets Borrowed matching MorphTarget3D.
void rt_canvas3d_draw_mesh_morphed(
    void *canvas, void *mesh, void *transform, void *material, void *morph_targets);

#ifdef __cplusplus
}
#endif
