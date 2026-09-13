//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_cloud.h
// Purpose: Public C ABI for Zanna.Services.Cloud, provider-neutral per-user
//          file storage that the platform synchronizes between devices.
// Key invariants:
//   - Stateful entry points must run on the main thread.
//   - Operations are synchronous against the platform's local cache; the
//     platform uploads changes on its own schedule.
//   - An empty file name or a data argument that is not Bytes traps even
//     without a started provider. Provider limits (file size, name length,
//     quota) return false or an Err Result and record a diagnostic.
//   - Without a provider that supports the feature, queries are neutral,
//     writes return false, and Read returns Err.
// Ownership/Lifetime:
//   - Read returns a caller-owned Zanna.Result holding caller-owned Bytes.
//   - Files returns a caller-owned Seq of caller-owned strings.
//   - Write copies the bytes; the caller keeps ownership of its Bytes.
// Links: src/runtime/services/rt_services_cloud.c,
//        src/runtime/services/rt_services.h,
//        docs/zannalib/services.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_cloud.h
 * @brief Declares Zanna.Services.Cloud.
 * @details Names are flat, provider-validated file names such as
 *          "profile.sav". Batches group writes and deletes that belong to one
 *          logical save so the platform never syncs half of it.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Report whether cloud storage is enabled for both the user's account and the app.
/// @return 1 when writes will synchronize, otherwise 0.
int8_t rt_services_cloud_get_is_enabled(void);

/// @brief Write (create or replace) a cloud file.
/// @param name File name; empty traps.
/// @param data Zanna.Collections.Bytes to store; NULL or another type traps.
/// @return 1 when the file was written, otherwise 0.
int8_t rt_services_cloud_write(rt_string name, void *data);

/// @brief Read a whole cloud file.
/// @param name File name; empty traps.
/// @return Caller-owned Zanna.Result: Ok(Bytes) or Err(message).
void *rt_services_cloud_read(rt_string name);

/// @brief Report whether a cloud file exists.
/// @param name File name; empty traps.
/// @return 1 when it exists, otherwise 0.
int8_t rt_services_cloud_exists(rt_string name);

/// @brief Delete a cloud file locally and from the platform.
/// @param name File name; empty traps.
/// @return 1 when the file was deleted, otherwise 0.
int8_t rt_services_cloud_delete(rt_string name);

/// @brief Read a cloud file's size.
/// @param name File name; empty traps.
/// @return Size in bytes, or 0 when the file does not exist or storage is unavailable.
int64_t rt_services_cloud_size(rt_string name);

/// @brief Read when a cloud file was last written.
/// @param name File name; empty traps.
/// @return Seconds since the Unix epoch, or 0 when unknown.
int64_t rt_services_cloud_timestamp(rt_string name);

/// @brief List the app's cloud files.
/// @return Caller-owned Seq of caller-owned file names (empty when unavailable).
void *rt_services_cloud_files(void);

/// @brief Read the user's total cloud quota for the app.
/// @return Quota in bytes, or 0 when unavailable.
int64_t rt_services_cloud_get_quota_total(void);

/// @brief Read the cloud quota still available to the app.
/// @return Available bytes, or 0 when unavailable.
int64_t rt_services_cloud_get_quota_available(void);

/// @brief Begin grouping writes and deletes into one logical change.
/// @return 1 when the batch started, otherwise 0 (for example when one is already open).
int8_t rt_services_cloud_begin_batch(void);

/// @brief Finish the batch started by BeginBatch.
/// @return 1 when the batch ended, otherwise 0 (for example when none is open).
int8_t rt_services_cloud_end_batch(void);

#ifdef __cplusplus
}
#endif
