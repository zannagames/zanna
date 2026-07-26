//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_machine_linux_cgroup.h
// Purpose: Resolve Linux cgroup controller bases from procfs mount metadata.
//
// Key invariants:
//   - Mount roots are combined with the process membership path.
//   - V1 cpu, cpuset, and memory controllers resolve independently.
//
// Ownership/Lifetime:
//   - Results are copied into caller-owned fixed buffers.
//
// Links: src/runtime/system/rt_machine.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_machine_linux_cgroup.h
 * @brief Declares Linux cgroup path resolution for machine resource queries.
 * @details The resolver maps procfs mount and membership metadata into
 *          caller-owned fixed buffers for the unified hierarchy and the v1
 *          CPU, cpuset, and memory controllers, leaving unresolved fields
 *          empty.
 */

#pragma once

/// @brief Fixed capacity, including the terminator, of each resolved path.
enum { RT_MACHINE_CGROUP_PATH_CAPACITY = 4096 };

/// @brief Resolved directories for Linux cgroup v2 and selected v1 controllers.
/// @details Empty fields indicate that the corresponding hierarchy or
///          controller could not be resolved for the process.
typedef struct rt_machine_linux_cgroup_paths {
    char unified[RT_MACHINE_CGROUP_PATH_CAPACITY]; ///< Cgroup-v2 unified directory.
    char cpu[RT_MACHINE_CGROUP_PATH_CAPACITY];     ///< Cgroup-v1 CPU directory.
    char cpuset[RT_MACHINE_CGROUP_PATH_CAPACITY];  ///< Cgroup-v1 cpuset directory.
    char memory[RT_MACHINE_CGROUP_PATH_CAPACITY];  ///< Cgroup-v1 memory directory.
} rt_machine_linux_cgroup_paths_t;

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Resolve controller directories for the current Linux cgroup membership.
/// @details Parses proc cgroup membership and mountinfo records, decodes procfs
///          path escapes, verifies membership beneath each mount root, and
///          resolves v1 cpu, cpuset, and memory controllers independently. On
///          Linux, valid @p out storage is cleared before files are read. The
///          non-Linux implementation returns zero and leaves @p out unchanged.
/// @param mountinfo_path NUL-terminated path to a proc mountinfo-format file.
/// @param membership_path NUL-terminated path to a proc cgroup-format file.
/// @param out Caller-owned fixed-buffer result structure.
/// @return 1 when at least one directory is resolved on Linux, otherwise 0.
int zanna_machine_linux_resolve_cgroups(const char *mountinfo_path,
                                        const char *membership_path,
                                        rt_machine_linux_cgroup_paths_t *out);

#ifdef __cplusplus
}
#endif
