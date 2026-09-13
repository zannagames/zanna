//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_cloud.c
// Purpose: Binds ISteamRemoteStorage (Steam Cloud) for the Steam provider's
//          Zanna.Services.Cloud operations.
// Key invariants:
//   - The interface is usable only when its accessor and every export the
//     provider calls resolved; otherwise one diagnostic names what is missing.
//   - Calls are the synchronous remote storage functions; they touch Steam's
//     local cache and Steam uploads changes itself.
//   - Files larger than Steam's 100 MiB FileWrite limit are rejected with a
//     diagnostic before calling Steam.
// Ownership/Lifetime:
//   - Read returns a new Zanna.Collections.Bytes owned by the caller.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_cloud.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_cloud.c
 * @brief Implements Steam Cloud file storage.
 */

#include "rt_bytes.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_services_provider.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"

#include <stdio.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Binding
//===----------------------------------------------------------------------===//

/// @brief Resolve one export into a typed field, remembering the first missing name.
#define STEAM_BIND(field, type, name, missing)                                                     \
    do {                                                                                           \
        void *symbol_ = rt_services_steam_symbol(name);                                            \
        if (!symbol_ && !(missing))                                                                \
            (missing) = (name);                                                                    \
        (field) = symbol_ ? RT_FN_PTR_CAST((type)symbol_) : NULL;                                  \
    } while (0)

/// @brief Open and bind ISteamRemoteStorage.
void rt_services_steam_bind_cloud(void) {
    steam_remote_storage_api api;
    memset(&api, 0, sizeof(api));
    const char *missing = NULL;

    void *accessor = rt_services_steam_symbol(RT_STEAM_SYMBOL_REMOTE_STORAGE_V016);
    if (accessor)
        api.self = (RT_FN_PTR_CAST((rt_steam_accessor_fn)accessor))();
    if (!api.self) {
        rt_services_steam_report_missing(RT_STEAM_SYMBOL_REMOTE_STORAGE_V016, "cloud storage");
        return;
    }
    STEAM_BIND(
        api.file_write, rt_steam_file_write_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_WRITE, missing);
    STEAM_BIND(
        api.file_read, rt_steam_file_read_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_READ, missing);
    STEAM_BIND(api.file_exists,
               rt_steam_self_str_bool_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_EXISTS,
               missing);
    STEAM_BIND(api.file_delete,
               rt_steam_self_str_bool_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_DELETE,
               missing);
    STEAM_BIND(
        api.file_size, rt_steam_file_size_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_SIZE, missing);
    STEAM_BIND(api.file_timestamp,
               rt_steam_file_timestamp_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_TIMESTAMP,
               missing);
    STEAM_BIND(
        api.file_count, rt_steam_file_count_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_COUNT, missing);
    STEAM_BIND(api.file_name_and_size,
               rt_steam_file_name_and_size_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_FILE_NAME_AND_SIZE,
               missing);
    STEAM_BIND(api.quota, rt_steam_quota_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_QUOTA, missing);
    STEAM_BIND(api.enabled_for_account,
               rt_steam_self_bool_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_ENABLED_FOR_ACCOUNT,
               missing);
    STEAM_BIND(api.enabled_for_app,
               rt_steam_self_bool_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_ENABLED_FOR_APP,
               missing);
    STEAM_BIND(api.begin_batch,
               rt_steam_self_bool_fn,
               RT_STEAM_SYMBOL_REMOTE_STORAGE_BEGIN_BATCH,
               missing);
    STEAM_BIND(
        api.end_batch, rt_steam_self_bool_fn, RT_STEAM_SYMBOL_REMOTE_STORAGE_END_BATCH, missing);
    if (missing) {
        rt_services_steam_report_missing(missing, "cloud storage");
        return;
    }
    g_steam.remote_storage = api;
}

#undef STEAM_BIND

//===----------------------------------------------------------------------===//
// Operations
//===----------------------------------------------------------------------===//

/// @brief Report whether the remote storage interface can be called.
/// @return 1 when started and bound, otherwise 0.
static int steam_cloud_ready(void) {
    return g_steam.started && g_steam.remote_storage.self;
}

/// @brief Report whether Steam Cloud is enabled for the account and the app.
/// @return 1 when enabled, otherwise 0.
static int8_t steam_cloud_is_enabled(void) {
    if (!steam_cloud_ready())
        return 0;
    void *self = g_steam.remote_storage.self;
    return (g_steam.remote_storage.enabled_for_account(self) &&
            g_steam.remote_storage.enabled_for_app(self))
               ? 1
               : 0;
}

/// @brief Write a file.
/// @param name File name.
/// @param data Bytes, or NULL when @p size is 0.
/// @param size Byte count.
/// @return 1 when written, otherwise 0.
static int8_t steam_cloud_write(const char *name, const uint8_t *data, int64_t size) {
    static const uint8_t empty = 0;
    if (!steam_cloud_ready())
        return 0;
    if (size > RT_STEAM_CLOUD_FILE_MAX_BYTES) {
        rt_services_provider_add_diagnostic(
            "Steam: cloud file '%s' is %lld bytes; Steam Cloud accepts at most 104857600 bytes "
            "per file",
            name,
            (long long)size);
        return 0;
    }
    if (g_steam.remote_storage.file_write(
            g_steam.remote_storage.self, name, data ? (const void *)data : &empty, (int32_t)size))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: FileWrite('%s', %lld bytes) failed; check the file name, the Steam Cloud quota, "
        "and the app's file count limit",
        name,
        (long long)size);
    return 0;
}

/// @brief Read a whole file into new Bytes.
/// @param name File name.
/// @param out_bytes Receives caller-owned Bytes on success.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 on success, otherwise 0.
static int8_t steam_cloud_read(const char *name,
                               void **out_bytes,
                               char *message,
                               size_t message_capacity) {
    if (!steam_cloud_ready()) {
        snprintf(message, message_capacity, "Steam: cloud storage is unavailable");
        return 0;
    }
    void *self = g_steam.remote_storage.self;
    if (!g_steam.remote_storage.file_exists(self, name)) {
        snprintf(message, message_capacity, "Steam: cloud file '%s' does not exist", name);
        return 0;
    }
    int32_t size = g_steam.remote_storage.file_size(self, name);
    if (size < 0) {
        snprintf(message, message_capacity, "Steam: cloud file '%s' size is unavailable", name);
        return 0;
    }
    void *bytes = rt_bytes_new((int64_t)size);
    if (!bytes) {
        snprintf(message, message_capacity, "Steam: out of memory reading cloud file '%s'", name);
        return 0;
    }
    if (size > 0) {
        int32_t read = g_steam.remote_storage.file_read(self, name, rt_bytes_data(bytes), size);
        if (read != size) {
            if (rt_obj_release_check0(bytes))
                rt_obj_free(bytes);
            snprintf(message,
                     message_capacity,
                     "Steam: FileRead('%s') returned %d of %d bytes",
                     name,
                     (int)read,
                     (int)size);
            return 0;
        }
    }
    *out_bytes = bytes;
    return 1;
}

/// @brief Report whether a file exists.
/// @param name File name.
/// @return 1 when it exists, otherwise 0.
static int8_t steam_cloud_exists(const char *name) {
    if (!steam_cloud_ready())
        return 0;
    return g_steam.remote_storage.file_exists(g_steam.remote_storage.self, name) ? 1 : 0;
}

/// @brief Delete a file.
/// @param name File name.
/// @return 1 when deleted, otherwise 0.
static int8_t steam_cloud_remove(const char *name) {
    if (!steam_cloud_ready())
        return 0;
    if (g_steam.remote_storage.file_delete(g_steam.remote_storage.self, name))
        return 1;
    rt_services_provider_add_diagnostic("Steam: FileDelete('%s') failed", name);
    return 0;
}

/// @brief Read a file size.
/// @param name File name.
/// @return Size in bytes, or 0 when missing.
static int64_t steam_cloud_size(const char *name) {
    if (!steam_cloud_ready())
        return 0;
    return (int64_t)g_steam.remote_storage.file_size(g_steam.remote_storage.self, name);
}

/// @brief Read a file's write time.
/// @param name File name.
/// @return Unix seconds, or 0 when unknown.
static int64_t steam_cloud_timestamp(const char *name) {
    if (!steam_cloud_ready())
        return 0;
    return g_steam.remote_storage.file_timestamp(g_steam.remote_storage.self, name);
}

/// @brief Count the app's cloud files.
/// @return File count.
static int64_t steam_cloud_file_count(void) {
    if (!steam_cloud_ready())
        return 0;
    return (int64_t)g_steam.remote_storage.file_count(g_steam.remote_storage.self);
}

/// @brief Read the file name at an index.
/// @param index Non-negative index.
/// @return Owned name, or NULL when out of range.
static rt_string steam_cloud_file_name_at(int64_t index) {
    if (!steam_cloud_ready() || index > INT32_MAX)
        return NULL;
    int32_t size = 0;
    const char *name =
        g_steam.remote_storage.file_name_and_size(g_steam.remote_storage.self, (int)index, &size);
    return (name && *name) ? rt_const_cstr(name) : NULL;
}

/// @brief Read the Steam Cloud quota.
/// @param out_total Receives the total quota in bytes.
/// @param out_available Receives the available bytes.
/// @return 1 when read, otherwise 0.
static int8_t steam_cloud_quota(int64_t *out_total, int64_t *out_available) {
    if (!steam_cloud_ready())
        return 0;
    uint64_t total = 0;
    uint64_t available = 0;
    if (!g_steam.remote_storage.quota(g_steam.remote_storage.self, &total, &available))
        return 0;
    *out_total = total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
    *out_available = available > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)available;
    return 1;
}

/// @brief Begin a write batch.
/// @return 1 when started, otherwise 0.
static int8_t steam_cloud_begin_batch(void) {
    if (!steam_cloud_ready())
        return 0;
    return g_steam.remote_storage.begin_batch(g_steam.remote_storage.self) ? 1 : 0;
}

/// @brief End the write batch.
/// @return 1 when ended, otherwise 0.
static int8_t steam_cloud_end_batch(void) {
    if (!steam_cloud_ready())
        return 0;
    return g_steam.remote_storage.end_batch(g_steam.remote_storage.self) ? 1 : 0;
}

/// @brief Steam Cloud operations.
const rt_services_cloud_ops rt_services_steam_cloud_ops = {
    .is_enabled = steam_cloud_is_enabled,
    .write = steam_cloud_write,
    .read = steam_cloud_read,
    .exists = steam_cloud_exists,
    .remove = steam_cloud_remove,
    .size = steam_cloud_size,
    .timestamp = steam_cloud_timestamp,
    .file_count = steam_cloud_file_count,
    .file_name_at = steam_cloud_file_name_at,
    .quota = steam_cloud_quota,
    .begin_batch = steam_cloud_begin_batch,
    .end_batch = steam_cloud_end_batch,
};
