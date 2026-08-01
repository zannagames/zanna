//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_blendtree3d.h
// Purpose: Parametric 1D/2D animation blend tree layered over AnimBlend3D.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for one- and two-dimensional animation blend trees.
/// @details A BlendTree3D owns an AnimBlend3D bound to one retained
///          Skeleton3D. Each added Animation3D becomes a backend state mapped
///          to a parameter-space coordinate. One-dimensional trees interpolate
///          between horizontal neighbors; two-dimensional trees default to
///          freeform Delaunay/barycentric weighting and can opt into legacy
///          inverse-distance weighting. Trees accept at most 16 samples.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a 1D blend tree bound to a Skeleton3D.
/// @param[in] skeleton Borrowed Skeleton3D handle retained by the owned
///                     AnimBlend3D backend.
/// @return New GC-managed BlendTree3D, or `NULL` for an invalid skeleton or
///         allocation/backend-construction failure.
void *rt_blend_tree3d_new_1d(void *skeleton);
/// @brief Create a 2D blend tree bound to a Skeleton3D.
/// @details The new tree starts in freeform-directional weighting mode.
/// @param[in] skeleton Borrowed Skeleton3D handle retained by the owned
///                     AnimBlend3D backend.
/// @return New GC-managed BlendTree3D, or `NULL` for an invalid skeleton or
///         allocation/backend-construction failure.
void *rt_blend_tree3d_new_2d(void *skeleton);
/// @brief Add an animation sample at parameter coordinate (x, y). Returns sample index or -1.
/// @details Coordinates are sanitized to finite values in
///          `[-1000000,1000000]`; a 1D tree stores but ignores @p y.
///          The Animation3D must be compatible with the backend skeleton and is
///          retained by the new AnimBlend3D state. Weights are recomputed
///          immediately.
/// @param[in,out] tree BlendTree3D to extend.
/// @param[in] animation Borrowed Animation3D sample clip.
/// @param[in] x Horizontal parameter coordinate.
/// @param[in] y Vertical parameter coordinate.
/// @return Newly appended zero-based sample index, or `-1` for invalid input,
///         skeleton incompatibility, backend failure, or the 16-sample limit.
int64_t rt_blend_tree3d_add_sample(void *tree, void *animation, double x, double y);
/// @brief Set the current blend parameters.
/// @details Both coordinates are sanitized to finite values in
///          `[-1000000,1000000]`, and sample weights are recomputed
///          synchronously.
/// @param[in,out] tree BlendTree3D to configure.
/// @param[in] x Horizontal parameter coordinate.
/// @param[in] y Vertical parameter coordinate; ignored by 1D weighting.
void rt_blend_tree3d_set_param(void *tree, double x, double y);
/// @brief Recompute sample weights and advance the internal AnimBlend3D.
/// @details Negative or non-finite elapsed time becomes zero, which evaluates
///          the current weights without advancing backend playback clocks.
/// @param[in,out] tree BlendTree3D to evaluate.
/// @param[in] dt Requested elapsed time in seconds.
void rt_blend_tree3d_update(void *tree, double dt);
/// @brief Number of samples currently registered.
/// @param[in] tree BlendTree3D to inspect.
/// @return Sanitized readable sample count, or zero for an invalid handle.
int64_t rt_blend_tree3d_get_sample_count(void *tree);
/// @brief Internal helper: borrow the underlying AnimBlend3D handle.
/// @param[in] tree BlendTree3D to inspect.
/// @return Borrowed backend handle, or `NULL`; the caller must not release it.
void *rt_blend_tree3d_get_blend(void *tree);

/// @brief 2D weighting mode: 0 = freeform-directional (default), 1 = legacy IDW.
/// @details Exactly one selects inverse-distance-squared weighting; every
///          other value selects freeform mode. One-dimensional trees ignore
///          the request. Changed 2D modes recompute weights immediately.
/// @param[in,out] tree BlendTree3D to configure.
/// @param[in] mode Requested weighting-mode value.
void rt_blend_tree3d_set_blend_mode(void *tree, int64_t mode);

/// @brief Current 2D weighting mode.
/// @param[in] tree BlendTree3D to inspect.
/// @return Zero for freeform mode or an invalid handle; one for legacy IDW.
int64_t rt_blend_tree3d_get_blend_mode(void *tree);

#ifdef __cplusplus
}
#endif
