//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_cloud.c
// Purpose: Implements the provider-neutral Zanna.Services.Cloud class on top
//          of the active provider's cloud operation table.
// Key invariants:
//   - Every member checks the main thread first, then traps on an empty file
//     name or a Write data argument that is not Bytes, then delegates.
//   - Without an active provider exposing cloud storage, queries return
//     neutral values, writes return false, and Read returns Err.
// Ownership/Lifetime:
//   - Read returns a caller-owned Result that retains the provider's Bytes;
//     this file releases its own reference after wrapping.
//   - Files returns a caller-owned Seq of caller-owned strings.
// Links: src/runtime/services/rt_services_cloud.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_cloud.c
 * @brief Implements Zanna.Services.Cloud.
 */

#include "rt_services_cloud.h"

#include "rt_bytes.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <stdio.h>

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's cloud operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_cloud_ops *cloud_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->cloud : NULL;
}

/// @brief Check the main thread and require a non-empty file name.
/// @param member Class-qualified member name.
/// @param name File name argument.
/// @return Borrowed file name bytes, or NULL after a trap.
static const char *cloud_enter(const char *member, rt_string name) {
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    return rt_services_internal_require_name(name, member, "file name");
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Cloud
//===----------------------------------------------------------------------===//

/// @brief Report whether cloud storage is enabled for the account and app.
/// @return 1 when enabled, otherwise 0.
int8_t rt_services_cloud_get_is_enabled(void) {
    if (!rt_services_provider_require_main_thread("Cloud.IsEnabled"))
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    return (ops && ops->is_enabled && ops->is_enabled()) ? 1 : 0;
}

/// @brief Write a cloud file.
/// @param name File name.
/// @param data Zanna.Collections.Bytes to store.
/// @return 1 when written, otherwise 0.
int8_t rt_services_cloud_write(rt_string name, void *data) {
    static const char member[] = "Cloud.Write";
    const char *text = cloud_enter(member, name);
    if (!text)
        return 0;
    if (!data || !rt_bytes_is_bytes(data)) {
        rt_services_internal_trap_argument(member, "data must be Zanna.Collections.Bytes");
        return 0;
    }
    const rt_services_cloud_ops *ops = cloud_ops();
    if (!ops || !ops->write)
        return 0;
    return ops->write(text, rt_bytes_data_const(data), rt_bytes_len(data)) ? 1 : 0;
}

/// @brief Read a whole cloud file.
/// @param name File name.
/// @return Caller-owned Result: Ok(Bytes) or Err(message).
void *rt_services_cloud_read(rt_string name) {
    const char *text = cloud_enter("Cloud.Read", name);
    if (!text)
        return rt_services_internal_err_result("Services: Cloud.Read was not completed");
    const rt_services_provider *provider = rt_services_internal_active_provider();
    if (!provider)
        return rt_services_internal_err_result(
            "Services: no platform services provider is started");
    if (!provider->cloud || !provider->cloud->read) {
        char message[192];
        snprintf(message,
                 sizeof(message),
                 "Services: provider '%s' does not support cloud storage",
                 provider->id);
        return rt_services_internal_err_result(message);
    }
    char message[RT_SERVICES_MESSAGE_CAPACITY];
    void *bytes = NULL;
    message[0] = '\0';
    if (!provider->cloud->read(text, &bytes, message, sizeof(message)) || !bytes) {
        if (bytes && rt_obj_release_check0(bytes))
            rt_obj_free(bytes);
        return rt_services_internal_err_result(
            message[0] ? message : "Services: cloud file could not be read");
    }
    void *result = rt_result_ok(bytes);
    if (rt_obj_release_check0(bytes))
        rt_obj_free(bytes);
    return result;
}

/// @brief Report whether a cloud file exists.
/// @param name File name.
/// @return 1 when it exists, otherwise 0.
int8_t rt_services_cloud_exists(rt_string name) {
    const char *text = cloud_enter("Cloud.Exists", name);
    if (!text)
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    return (ops && ops->exists && ops->exists(text)) ? 1 : 0;
}

/// @brief Delete a cloud file.
/// @param name File name.
/// @return 1 when deleted, otherwise 0.
int8_t rt_services_cloud_delete(rt_string name) {
    const char *text = cloud_enter("Cloud.Delete", name);
    if (!text)
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    return (ops && ops->remove && ops->remove(text)) ? 1 : 0;
}

/// @brief Read a cloud file's size.
/// @param name File name.
/// @return Size in bytes, or 0.
int64_t rt_services_cloud_size(rt_string name) {
    const char *text = cloud_enter("Cloud.Size", name);
    if (!text)
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    if (!ops || !ops->size)
        return 0;
    int64_t size = ops->size(text);
    return size > 0 ? size : 0;
}

/// @brief Read a cloud file's write time.
/// @param name File name.
/// @return Unix seconds, or 0.
int64_t rt_services_cloud_timestamp(rt_string name) {
    const char *text = cloud_enter("Cloud.Timestamp", name);
    if (!text)
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    if (!ops || !ops->timestamp)
        return 0;
    int64_t timestamp = ops->timestamp(text);
    return timestamp > 0 ? timestamp : 0;
}

/// @brief List the app's cloud files.
/// @return Caller-owned Seq of caller-owned names.
void *rt_services_cloud_files(void) {
    if (!rt_services_provider_require_main_thread("Cloud.Files"))
        return NULL;
    void *seq = rt_seq_new_owned();
    if (!seq)
        return NULL;
    const rt_services_cloud_ops *ops = cloud_ops();
    if (!ops || !ops->file_count || !ops->file_name_at)
        return seq;
    int64_t count = ops->file_count();
    for (int64_t i = 0; i < count; ++i) {
        rt_string file = ops->file_name_at(i);
        if (file)
            rt_seq_push_raw(seq, file);
    }
    return seq;
}

/// @brief Read the total cloud quota.
/// @return Bytes, or 0 when unavailable.
int64_t rt_services_cloud_get_quota_total(void) {
    if (!rt_services_provider_require_main_thread("Cloud.QuotaTotal"))
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    int64_t total = 0;
    int64_t available = 0;
    if (!ops || !ops->quota || !ops->quota(&total, &available))
        return 0;
    return total > 0 ? total : 0;
}

/// @brief Read the available cloud quota.
/// @return Bytes, or 0 when unavailable.
int64_t rt_services_cloud_get_quota_available(void) {
    if (!rt_services_provider_require_main_thread("Cloud.QuotaAvailable"))
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    int64_t total = 0;
    int64_t available = 0;
    if (!ops || !ops->quota || !ops->quota(&total, &available))
        return 0;
    return available > 0 ? available : 0;
}

/// @brief Begin a write batch.
/// @return 1 when started, otherwise 0.
int8_t rt_services_cloud_begin_batch(void) {
    if (!rt_services_provider_require_main_thread("Cloud.BeginBatch"))
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    return (ops && ops->begin_batch && ops->begin_batch()) ? 1 : 0;
}

/// @brief End the write batch.
/// @return 1 when ended, otherwise 0.
int8_t rt_services_cloud_end_batch(void) {
    if (!rt_services_provider_require_main_thread("Cloud.EndBatch"))
        return 0;
    const rt_services_cloud_ops *ops = cloud_ops();
    return (ops && ops->end_batch && ops->end_batch()) ? 1 : 0;
}
