//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_internal.h
// Purpose: Private in-memory FBX bridge shared with Model3D asset loading.
// Key invariants:
//   - Input bytes are borrowed only for the synchronous parse.
//   - Relative textures resolve beside the supplied logical source path.
// Ownership/Lifetime:
//   - Successful assets are runtime-managed; the caller retains the byte span.
// Links: rt_fbx_loader.c, rt_model3d_loaders.inc
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *fbx_load_recoverable_bytes_internal(rt_string original_path,
                                          const uint8_t *bytes,
                                          size_t size);

#ifdef __cplusplus
}
#endif
