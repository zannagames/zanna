//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_asset.h
// Purpose: Runtime asset manager for loading embedded and packed resources.
//          Provides transparent asset resolution across embedded data (.rodata),
//          mounted .zpak pack files, and the filesystem.
//
// Key invariants:
//   - Resolution order: embedded → mounted packs (LIFO) → filesystem.
//   - Auto-discovery of .zpak files next to the executable on first use.
//   - Assets.Load() returns typed objects for recognized extensions and does
//     not downgrade malformed recognized data to Bytes.
//   - Assets.LoadBytes() always returns raw Bytes.
//   - Initialization and registry publication are serialized; loads retain a
//     selected pack before leaving the registry lock.
//
// Ownership/Lifetime:
//   - Returned objects are GC-managed.
//   - Mounted pack handles are held until unmount or process exit.
//   - An explicit embedded blob is borrowed and must remain valid for the
//     process lifetime.
//
// Links: src/runtime/io/rt_asset.c,
//        src/runtime/io/rt_asset_decode.c,
//        src/runtime/io/rt_zpak_reader.h,
//        src/runtime/io/rt_path_exe.c
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the process-wide layered asset manager.
 * @details Provides initialization, typed or raw loading, existence and size
 * queries, enumeration, and dynamic ZPAK mounting. Resolution snapshots
 * embedded, mounted, or loose-file sources while retaining any selected pack
 * through the complete load.
 */

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initialize the asset manager with an optional embedded ZPAK blob.
/// @details Called from the generated main trampoline when assets are
/// embedded. The first caller builds and atomically publishes the registry;
/// later calls are idempotent and ignore their blob arguments.
/// @param blob Optional borrowed pointer to a complete ZPAK image that remains
/// valid for the process lifetime.
/// @param size Byte length of @p blob.
void rt_asset_init(const uint8_t *blob, uint64_t size);

/// @brief Load an asset by name, returning a typed object based on extension.
/// @details Resolution is embedded, mounted packs in reverse mount order, then
/// a loose regular file. Recognized image/audio extensions return their stable
/// typed result or fail; unrecognized extensions return raw Bytes.
/// @param name Safe relative logical name, optionally prefixed by `asset://`.
/// @return Fresh GC-managed typed object or Bytes object, or `NULL` for an
/// invalid, missing, unreadable, malformed, or unallocatable asset.
void *rt_asset_load(rt_string name);

/// @brief Load an asset by name as raw Bytes, regardless of extension.
/// @param name Safe relative logical name, optionally prefixed by `asset://`.
/// @return Fresh GC-managed Bytes object, or `NULL` when resolution fails.
void *rt_asset_load_bytes(rt_string name);

/// @brief Load an asset by name into a malloc-owned raw buffer.
/// @details Runtime-internal convenience for asset-aware decoders that need to
///          parse bytes directly. Resolution matches LoadBytes().
/// @param name Safe relative asset name. The optional `asset://` scheme is
/// stripped before lookup.
/// @param out_size Receives the byte count on success and zero on failure; may
/// be `NULL`.
/// @return `malloc`-owned byte buffer, or `NULL` if unresolved. The caller
/// releases a successful result with `free`.
uint8_t *rt_asset_load_raw(rt_string name, size_t *out_size);

/// @brief Check if an asset exists (embedded, in pack, or on disk).
/// @param name Safe relative logical name, optionally prefixed by `asset://`.
/// @return 1 if found, 0 otherwise.
int64_t rt_asset_exists(rt_string name);

/// @brief Get the size of an asset in bytes.
/// @param name Safe relative logical name, optionally prefixed by `asset://`.
/// @return Uncompressed asset size in bytes, or -1 when invalid, absent, or
/// not representable by `int64_t`.
int64_t rt_asset_size(rt_string name);

/// @brief List all available asset names (embedded + all mounted packs).
/// @details Loose filesystem assets are not enumerable. Source order and
/// duplicate names are preserved.
/// @return Fresh GC-managed, element-owning `seq<str>`, or `NULL` on
/// allocation failure.
void *rt_asset_list(void);

/// @brief Mount a .zpak pack file for asset resolution.
/// @param path Runtime filesystem path to a valid ZPAK file.
/// @return 1 when mounted or already present, or 0 on failure or when the
/// 32-pack capacity is full.
int64_t rt_asset_mount(rt_string path);

/// @brief Unmount a previously mounted .zpak pack file.
/// @details Accepts an exact canonical path or a separator-free basename that
/// uniquely identifies one mounted pack.
/// @param path Runtime path or unique basename to remove.
/// @return 1 when removed, or 0 when no unique match exists.
int64_t rt_asset_unmount(rt_string path);

#ifdef __cplusplus
}
#endif
