//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_animcontroller3d.c
// Purpose: High-level skeletal animation controller backing
//          `Zanna.Graphics3D.AnimController3D`. Provides named states,
//          transitions, events, root-motion extraction, and simple
//          masked overlay layers on top of the underlying AnimPlayer3D.
//
// Key invariants:
//   - Up to RT_ANIM_CONTROLLER3D_MAX_LAYERS overlay layers; layer 0 is the base.
//   - Every public entry validates handles via *_checked helpers.
//   - State / transition tables grow on demand via realloc.
//   - The event queue is a fixed-size ring buffer; oldest events drop on overflow.
//
// Ownership/Lifetime:
//   - Controller objects are heap-allocated and GC-managed.
//   - Skeleton and Animation references are retained on assignment, released
//     on slot replacement and during finalize.
//
// Links: src/runtime/graphics/3d/anim/rt_animcontroller3d.h (public API),
//        src/runtime/graphics/3d/anim/rt_skeleton3d.h (underlying AnimPlayer3D),
//        docs/zannalib/graphics/animation.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Assembles the graphics-enabled AnimController3D implementation.
/// @details The controller is split into private storage/lifecycle helpers,
///          pose-sampling helpers, and public ABI entry points included below.
///          When graphics support is disabled this translation unit exports no
///          controller implementation.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_animcontroller3d.h"

#include "rt_blendtree3d.h"
#include "rt_g3d_ref_slots.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_iksolver3d.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_quat.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// clang-format off
#include "rt_animcontroller3d_internals.inc"
#include "rt_animcontroller3d_sampling.inc"
#include "rt_animcontroller3d_api.inc"
// clang-format on
#endif
