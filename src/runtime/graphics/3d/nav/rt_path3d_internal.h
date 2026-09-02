//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_path3d_internal.h
// Purpose: Private bulk-storage helpers shared by Path3D and NavMesh3D.
// Key invariants:
//   - Bulk appends reserve all coordinate storage before publishing any point.
//   - Adopted interleaved storage remains directly readable and is converted only on growth.
//   - This header is internal and does not extend the scripting runtime ABI.
// Ownership/Lifetime:
//   - Bulk-append coordinate arrays are borrowed for the duration of each call.
//   - The adopt constructor takes ownership only when it returns a non-null path.
// Links: rt_path3d.c, rt_navmesh3d_query.inc
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t rt_path3d_append_xyz_batch_internal(void *path,
                                            const double *points_xyz,
                                            int64_t point_count);
void *rt_path3d_new_adopt_xyz_internal(double *points_xyz,
                                       int64_t point_count,
                                       int64_t point_capacity);
void rt_path3d_test_set_coordinate_alloc_failure(int8_t enabled);

#ifdef __cplusplus
}
#endif
