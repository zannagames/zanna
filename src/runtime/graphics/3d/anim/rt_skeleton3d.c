//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d.c
// Purpose: Skeleton3D (bone hierarchy + bind pose), Animation3D (keyframe
//   clips), and AnimPlayer3D (playback, sampling, crossfade, palette output).
//
// Key invariants:
//   - Authored bones may arrive in non-topological order; global-pose builders
//     resolve parent chains recursively and break cycles as roots.
//   - Palette computation: local → global (multiply up hierarchy) → * inverse_bind.
//   - Keyframe sampling: binary search for bracket, SLERP rotation, lerp pos/scale.
//   - Crossfade: blend per-bone local transforms between two animations.
//   - GPU vs CPU skinning gated per-backend by bone-count limits.
//
// Ownership/Lifetime:
//   - Skeleton3D / Animation3D / AnimPlayer3D are GC-managed.
//   - Animation keyframe arrays are owned heap allocations freed in the finalizer.
//   - Bone-name strings are retained on assignment.
//
// Links: rt_skeleton3d.h, vgfx3d_skinning.h, plans/3d/14-skeletal-animation.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Assembles the graphics-enabled skeletal-animation runtime.
/// @details The included private fragments implement matrix utilities,
///          Skeleton3D hierarchy construction, Animation3D clips, AnimPlayer3D
///          playback, CPU/GPU skinned drawing, and AnimBlend3D pose blending.
///          A graphics-disabled build exports only a translation-unit guard.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_skeleton3d.h"
#include "rt_animcontroller3d.h"
#include "rt_blendtree3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_heap.h"
#include "rt_instbatch3d.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_quat.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_skinning.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Largest finite magnitude accepted when narrowing general authored lanes.
#define SKELETON3D_FLOAT_ABS_MAX 3.40282346638528859812e38
/// Safety clamp for animation times, durations, speeds, and transform lanes.
#define SKELETON3D_ANIM_ABS_MAX 1.0e12f

/// @brief Should we hand bone matrices to the GPU instead of skinning on the CPU?
///
/// Driven by the backend's `gpu_skinning` capability bit (its draw path
/// consumes bone palettes in the vertex shader) while the active palette fits
/// the shader-visible upload limit. The software backend leaves the bit clear
/// and takes the CPU-skin path.
/// @param[in] backend Active rendering backend and capability table.
/// @param[in] bone_count Number of matrices required by the draw.
/// @return Nonzero only when GPU skinning is enabled and the positive bone
///         count fits the shader-visible palette limit.
static int vgfx3d_backend_prefers_gpu_skinning(const vgfx3d_backend_t *backend,
                                               int32_t bone_count) {
    return backend && backend->gpu_skinning && bone_count > 0 && bone_count <= VGFX3D_MAX_BONES;
}

// clang-format off
#include "rt_skeleton3d_matrix.inc"
#include "rt_skeleton3d_skeleton.inc"
#include "rt_skeleton3d_animation.inc"
#include "rt_skeleton3d_player.inc"
#include "rt_skeleton3d_skinning.inc"
#include "rt_skeleton3d_blend.inc"
// clang-format on
#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
