//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_model3d.c
// Purpose: Model3D high-level asset wrapper over imported scene/resources.
//   Owns a template scene-graph root and per-asset reference arrays for
//   meshes, materials, skeletons, animations, and node animations imported
//   from VSCN, FBX, glTF, OBJ, or STL. Each `Instantiate()` clones the template
//   root into a fresh scene subtree that can be parented anywhere.
//
// Key invariants:
//   - The template root is a synthetic node — its children represent the
//     authored asset's top-level scene roots.
//   - Reference arrays are dedup'd by pointer at append time so a mesh
//     reused across many nodes only has one retain.
//   - `model_count_subtree` includes the template root; user-visible counts
//     subtract 1.
//   - Instancing deep-copies typed node metadata while sharing immutable assets.
//
// Ownership/Lifetime:
//   - Model3D is GC-managed; finalizer releases the template root and every
//     entry of the reference arrays.
//   - Cloning a node retains the source's mesh/material; only morph-enabled
//     meshes and typed metadata are deep-cloned for per-instance mutable state.
//
// Links: rt_model3d.h, rt_scene3d.h, rt_fbx_loader.h, rt_gltf.h, rt_asset.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Assembles Model3D ownership, asset-loader, and public API implementations.
/// @details Model3D stores an immutable imported template scene plus deduplicated retained asset
///          arrays. The included implementation fragments clone that template into independently
///          mutable instances while sharing immutable meshes, materials, skeletons, and animations
///          where their ownership contracts allow.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_model3d.h"

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_bytes.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_fbx_loader.h"
#include "rt_file_ext.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_mesh_simplify.h"
#include "rt_morphtarget3d.h"
#include "rt_numeric.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_pixels.h"
#include "rt_result.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_scene3d_vscn_internal.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_tempfile.h"
#include "rt_trap.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_fbx_get_scene_root(void *fbx);
extern void *rt_pixels_load(void *path);

/* Parallel LOD chain generation (GenerateLODs phase 2). */
extern void *rt_thread_start_fn(void (*entry)(void *), void *arg);
extern void rt_thread_join(void *thread);
extern int64_t rt_machine_cores(void);

#define MODEL3D_MAX_WALK_NODES 1048576
#define MODEL3D_OBJ_MAX_LINE_BYTES (1024u * 1024u)
#define MODEL3D_OBJ_MAX_PATH_BYTES (64u * 1024u)
#if defined(__clang__) || defined(__GNUC__)
#define MODEL3D_UNUSED_PRIVATE __attribute__((unused))
#else
#define MODEL3D_UNUSED_PRIVATE
#endif

// clang-format off
#include "rt_model3d_core.inc"
#include "rt_model3d_loaders.inc"
#include "rt_model3d_api.inc"
// clang-format on
#else
typedef int rt_model3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
