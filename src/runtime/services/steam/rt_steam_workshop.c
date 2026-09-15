//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_workshop.c
// Purpose: Binds ISteamUGC (Steam Workshop) for the Steam provider's
//          Zanna.Services.Workshop operations, Workshop requests (queries,
//          subscriptions, item creation, update submission, deletion), and
//          Workshop callbacks.
// Key invariants:
//   - The group is usable only when its accessor, every export, and the app id
//     query resolved; otherwise one diagnostic names what is missing.
//   - SteamUGC_v021 (SDK 1.62 onward) and SteamUGC_v020 declare
//     GetNumSubscribedItems and GetSubscribedItems differently; the binding
//     calls the form that matches the accessor it opened, and asks v021 to
//     leave locally disabled items out.
//   - Item ids are decimal PublishedFileId_t values in 1..2^64-1 and update ids
//     decimal UGCUpdateHandle_t values below 2^64-1; malformed ids trap while
//     Steam is the active provider.
//   - A query's UGCQueryHandle_t is released on every path: send failure,
//     completion, and provider stop.
// Ownership/Lifetime:
//   - Query results are copied into heap buffers that live until the request
//     is finished, then freed.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_workshop.h,
//        docs/adr/0366-platform-services-workshop.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_workshop.c
 * @brief Implements the Steam Workshop for Zanna.Services.Workshop.
 */

#include "rt_platform.h"
#include "rt_services.h"
#include "rt_services_provider.h"
#include "rt_services_workshop.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>
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

/// @brief Open and bind ISteamUGC.
void rt_services_steam_bind_workshop(void) {
    steam_ugc_api api;
    memset(&api, 0, sizeof(api));
    void *accessor = rt_services_steam_symbol(RT_STEAM_SYMBOL_UGC_V021);
    if (accessor) {
        api.self = (RT_FN_PTR_CAST((rt_steam_accessor_fn)accessor))();
        api.version = 21;
    }
    if (!api.self) {
        accessor = rt_services_steam_symbol(RT_STEAM_SYMBOL_UGC_V020);
        if (accessor) {
            api.self = (RT_FN_PTR_CAST((rt_steam_accessor_fn)accessor))();
            api.version = 20;
        }
    }
    if (!api.self) {
        rt_services_steam_report_missing(RT_STEAM_SYMBOL_UGC_V021 " or " RT_STEAM_SYMBOL_UGC_V020,
                                         "Workshop");
        return;
    }
    const char *missing = NULL;
    STEAM_BIND(api.query_user, rt_steam_ugc_query_user_fn, RT_STEAM_SYMBOL_UGC_QUERY_USER, missing);
    STEAM_BIND(api.query_all, rt_steam_ugc_query_all_fn, RT_STEAM_SYMBOL_UGC_QUERY_ALL, missing);
    STEAM_BIND(api.query_details,
               rt_steam_ugc_query_details_fn,
               RT_STEAM_SYMBOL_UGC_QUERY_DETAILS,
               missing);
    STEAM_BIND(api.send_query, rt_steam_self_u64_call_fn, RT_STEAM_SYMBOL_UGC_SEND_QUERY, missing);
    STEAM_BIND(
        api.query_result, rt_steam_ugc_query_result_fn, RT_STEAM_SYMBOL_UGC_QUERY_RESULT, missing);
    STEAM_BIND(api.preview_url,
               rt_steam_ugc_query_text_fn,
               RT_STEAM_SYMBOL_UGC_QUERY_PREVIEW_URL,
               missing);
    STEAM_BIND(api.query_metadata,
               rt_steam_ugc_query_text_fn,
               RT_STEAM_SYMBOL_UGC_QUERY_METADATA,
               missing);
    STEAM_BIND(
        api.release_query, rt_steam_self_u64_bool_fn, RT_STEAM_SYMBOL_UGC_RELEASE_QUERY, missing);
    STEAM_BIND(api.add_required_tag,
               rt_steam_self_u64_str_bool_fn,
               RT_STEAM_SYMBOL_UGC_ADD_REQUIRED_TAG,
               missing);
    STEAM_BIND(api.set_search_text,
               rt_steam_self_u64_str_bool_fn,
               RT_STEAM_SYMBOL_UGC_SET_SEARCH_TEXT,
               missing);
    STEAM_BIND(api.return_long_description,
               rt_steam_self_u64_flag_bool_fn,
               RT_STEAM_SYMBOL_UGC_RETURN_LONG_DESCRIPTION,
               missing);
    STEAM_BIND(api.return_metadata,
               rt_steam_self_u64_flag_bool_fn,
               RT_STEAM_SYMBOL_UGC_RETURN_METADATA,
               missing);
    STEAM_BIND(
        api.create_item, rt_steam_ugc_create_item_fn, RT_STEAM_SYMBOL_UGC_CREATE_ITEM, missing);
    STEAM_BIND(
        api.start_update, rt_steam_ugc_start_update_fn, RT_STEAM_SYMBOL_UGC_START_UPDATE, missing);
    STEAM_BIND(
        api.set_title, rt_steam_self_u64_str_bool_fn, RT_STEAM_SYMBOL_UGC_SET_TITLE, missing);
    STEAM_BIND(api.set_description,
               rt_steam_self_u64_str_bool_fn,
               RT_STEAM_SYMBOL_UGC_SET_DESCRIPTION,
               missing);
    STEAM_BIND(
        api.set_metadata, rt_steam_self_u64_str_bool_fn, RT_STEAM_SYMBOL_UGC_SET_METADATA, missing);
    STEAM_BIND(api.set_visibility,
               rt_steam_ugc_set_visibility_fn,
               RT_STEAM_SYMBOL_UGC_SET_VISIBILITY,
               missing);
    STEAM_BIND(api.set_tags, rt_steam_ugc_set_tags_fn, RT_STEAM_SYMBOL_UGC_SET_TAGS, missing);
    STEAM_BIND(
        api.set_content, rt_steam_self_u64_str_bool_fn, RT_STEAM_SYMBOL_UGC_SET_CONTENT, missing);
    STEAM_BIND(
        api.set_preview, rt_steam_self_u64_str_bool_fn, RT_STEAM_SYMBOL_UGC_SET_PREVIEW, missing);
    STEAM_BIND(api.submit_update,
               rt_steam_ugc_submit_update_fn,
               RT_STEAM_SYMBOL_UGC_SUBMIT_UPDATE,
               missing);
    STEAM_BIND(api.update_progress,
               rt_steam_ugc_update_progress_fn,
               RT_STEAM_SYMBOL_UGC_UPDATE_PROGRESS,
               missing);
    STEAM_BIND(api.subscribe, rt_steam_self_u64_call_fn, RT_STEAM_SYMBOL_UGC_SUBSCRIBE, missing);
    STEAM_BIND(
        api.unsubscribe, rt_steam_self_u64_call_fn, RT_STEAM_SYMBOL_UGC_UNSUBSCRIBE, missing);
    STEAM_BIND(api.delete_item, rt_steam_self_u64_call_fn, RT_STEAM_SYMBOL_UGC_DELETE, missing);
    if (api.version == 21) {
        STEAM_BIND(api.subscribed_count_v021,
                   rt_steam_ugc_subscribed_count_v021_fn,
                   RT_STEAM_SYMBOL_UGC_SUBSCRIBED_COUNT,
                   missing);
        STEAM_BIND(api.subscribed_items_v021,
                   rt_steam_ugc_subscribed_items_v021_fn,
                   RT_STEAM_SYMBOL_UGC_SUBSCRIBED_ITEMS,
                   missing);
    } else {
        STEAM_BIND(api.subscribed_count_v020,
                   rt_steam_ugc_subscribed_count_v020_fn,
                   RT_STEAM_SYMBOL_UGC_SUBSCRIBED_COUNT,
                   missing);
        STEAM_BIND(api.subscribed_items_v020,
                   rt_steam_ugc_subscribed_items_v020_fn,
                   RT_STEAM_SYMBOL_UGC_SUBSCRIBED_ITEMS,
                   missing);
    }
    STEAM_BIND(api.item_state, rt_steam_self_u64_u32_fn, RT_STEAM_SYMBOL_UGC_ITEM_STATE, missing);
    STEAM_BIND(
        api.install_info, rt_steam_ugc_install_info_fn, RT_STEAM_SYMBOL_UGC_INSTALL_INFO, missing);
    STEAM_BIND(api.download_info,
               rt_steam_ugc_download_info_fn,
               RT_STEAM_SYMBOL_UGC_DOWNLOAD_INFO,
               missing);
    STEAM_BIND(api.download, rt_steam_self_u64_flag_bool_fn, RT_STEAM_SYMBOL_UGC_DOWNLOAD, missing);
    if (missing) {
        rt_services_steam_report_missing(missing, "Workshop");
        return;
    }
    if (!g_steam.utils.self || !g_steam.utils.get_app_id) {
        rt_services_provider_add_diagnostic(
            "Steam: the Workshop needs ISteamUtils for the app id; Workshop disabled");
        return;
    }
    api.ready = 1;
    g_steam.ugc = api;
}

#undef STEAM_BIND

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Report whether the Workshop group can be called.
/// @return 1 when started and bound, otherwise 0.
static int steam_workshop_ready(void) {
    return g_steam.started && g_steam.ugc.ready;
}

/// @brief Read the app id the Workshop queries and publishes for.
/// @return AppId_t.
static uint32_t steam_workshop_app_id(void) {
    return g_steam.utils.get_app_id(g_steam.utils.self);
}

/// @brief Parse an update id, which may be 0.
/// @param text Candidate decimal text.
/// @param out Receives the handle.
/// @return 1 when @p text is an integer in 0..18446744073709551614, otherwise 0.
static int steam_workshop_parse_update(const char *text, uint64_t *out) {
    if (strcmp(text, "0") == 0) {
        *out = 0;
        return 1;
    }
    return rt_services_steam_parse_u64(text, out) && *out != RT_STEAM_UGC_INVALID_HANDLE;
}

/// @brief Report a Steam call that returned false, with the id it named.
/// @param method Steam method name.
/// @param id Id text the call named.
static void steam_workshop_call_failed(const char *method, const char *id) {
    rt_services_provider_add_diagnostic("Steam: %s('%s') failed", method, id);
}

/// @brief Copy a fixed-size Steam text field into a new heap string.
/// @param text Field bytes, possibly unterminated.
/// @param capacity Field size in bytes.
/// @return Heap copy, or NULL when allocation failed.
static char *steam_workshop_copy_field(const char *text, size_t capacity) {
    size_t length = 0;
    while (length < capacity && text[length])
        ++length;
    char *copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

//===----------------------------------------------------------------------===//
// Operations
//===----------------------------------------------------------------------===//

/// @brief Trap unless an item id is a decimal PublishedFileId_t.
/// @param member Class-qualified member name.
/// @param item_id Non-empty candidate.
/// @return 1 when well formed, 0 after the trap.
static int8_t steam_workshop_check_item_id(const char *member, const char *item_id) {
    uint64_t value = 0;
    if (rt_services_steam_parse_u64(item_id, &value))
        return 1;
    char message[320];
    snprintf(message,
             sizeof(message),
             "Services.%s: Steam Workshop item id '%s' must be an integer in "
             "1..18446744073709551615",
             member,
             item_id);
    rt_trap(message);
    return 0;
}

/// @brief Trap unless an update id is a decimal UGCUpdateHandle_t.
/// @param member Class-qualified member name.
/// @param update_id Non-empty candidate.
/// @return 1 when well formed, 0 after the trap.
static int8_t steam_workshop_check_update_id(const char *member, const char *update_id) {
    uint64_t value = 0;
    if (steam_workshop_parse_update(update_id, &value))
        return 1;
    char message[320];
    snprintf(message,
             sizeof(message),
             "Services.%s: Steam Workshop update id '%s' must be an integer in "
             "0..18446744073709551614",
             member,
             update_id);
    rt_trap(message);
    return 0;
}

/// @brief Count subscribed items.
/// @return Count.
static int64_t steam_workshop_subscribed_count(void) {
    if (!steam_workshop_ready())
        return 0;
    return g_steam.ugc.version == 21
               ? (int64_t)g_steam.ugc.subscribed_count_v021(g_steam.ugc.self, false)
               : (int64_t)g_steam.ugc.subscribed_count_v020(g_steam.ugc.self);
}

/// @brief Write the id of a subscribed item.
/// @param index Non-negative index.
/// @param out_id Destination.
/// @param id_capacity Size of @p out_id.
/// @return 1 when @p index names an item.
static int8_t steam_workshop_subscribed_id_at(int64_t index, char *out_id, size_t id_capacity) {
    const int64_t count = steam_workshop_subscribed_count();
    if (count <= 0 || index >= count || count > UINT32_MAX)
        return 0;
    uint64_t *ids = (uint64_t *)calloc((size_t)count, sizeof(uint64_t));
    if (!ids)
        return 0;
    const uint32_t written =
        g_steam.ugc.version == 21
            ? g_steam.ugc.subscribed_items_v021(g_steam.ugc.self, ids, (uint32_t)count, false)
            : g_steam.ugc.subscribed_items_v020(g_steam.ugc.self, ids, (uint32_t)count);
    const int found = index < (int64_t)written && ids[index] != 0;
    if (found)
        snprintf(out_id, id_capacity, "%llu", (unsigned long long)ids[index]);
    free(ids);
    return found ? 1 : 0;
}

/// @brief Read an item's state flags.
/// @param item_id Decimal item id.
/// @return RT_SERVICES_WORKSHOP_STATE_* flags (EItemState uses the same bits).
static int64_t steam_workshop_item_state(const char *item_id) {
    uint64_t id = 0;
    if (!steam_workshop_ready() || !rt_services_steam_parse_u64(item_id, &id))
        return 0;
    const uint32_t flags = g_steam.ugc.item_state(g_steam.ugc.self, id);
    return (int64_t)(flags & (uint32_t)(RT_SERVICES_WORKSHOP_STATE_SUBSCRIBED |
                                        RT_SERVICES_WORKSHOP_STATE_INSTALLED |
                                        RT_SERVICES_WORKSHOP_STATE_NEEDS_UPDATE |
                                        RT_SERVICES_WORKSHOP_STATE_DOWNLOADING |
                                        RT_SERVICES_WORKSHOP_STATE_DOWNLOAD_PENDING));
}

/// @brief Read where an installed item lives.
/// @param item_id Decimal item id.
/// @param out_folder Destination folder.
/// @param folder_capacity Size of @p out_folder.
/// @param out_size Receives the size on disk.
/// @param out_time Receives the install time.
/// @return 1 when installed.
static int8_t steam_workshop_install_info(const char *item_id,
                                          char *out_folder,
                                          size_t folder_capacity,
                                          int64_t *out_size,
                                          int64_t *out_time) {
    uint64_t id = 0;
    if (!steam_workshop_ready() || !rt_services_steam_parse_u64(item_id, &id) ||
        folder_capacity > UINT32_MAX)
        return 0;
    uint64_t size = 0;
    uint32_t timestamp = 0;
    out_folder[0] = '\0';
    if (!g_steam.ugc.install_info(
            g_steam.ugc.self, id, &size, out_folder, (uint32_t)folder_capacity, &timestamp))
        return 0;
    out_folder[folder_capacity - 1] = '\0';
    *out_size = size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)size;
    *out_time = (int64_t)timestamp;
    return 1;
}

/// @brief Read download progress.
/// @param item_id Decimal item id.
/// @param out_downloaded Receives bytes downloaded.
/// @param out_total Receives bytes to download.
/// @return 1 when reported.
static int8_t steam_workshop_download_info(const char *item_id,
                                           int64_t *out_downloaded,
                                           int64_t *out_total) {
    uint64_t id = 0;
    if (!steam_workshop_ready() || !rt_services_steam_parse_u64(item_id, &id))
        return 0;
    uint64_t downloaded = 0;
    uint64_t total = 0;
    if (!g_steam.ugc.download_info(g_steam.ugc.self, id, &downloaded, &total))
        return 0;
    *out_downloaded = downloaded > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)downloaded;
    *out_total = total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
    return 1;
}

/// @brief Download or update an item.
/// @param item_id Decimal item id.
/// @param high_priority Nonzero for high priority.
/// @return 1 when queued.
static int8_t steam_workshop_download(const char *item_id, int8_t high_priority) {
    uint64_t id = 0;
    if (!steam_workshop_ready() || !rt_services_steam_parse_u64(item_id, &id))
        return 0;
    if (g_steam.ugc.download(g_steam.ugc.self, id, high_priority != 0))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: DownloadItem('%s') failed; check the item id and that Steam is online", item_id);
    return 0;
}

/// @brief Begin an update of an item.
/// @param item_id Decimal item id.
/// @param out_update_id Receives the update id.
/// @param id_capacity Size of @p out_update_id.
/// @return 1 when started.
static int8_t steam_workshop_start_update(const char *item_id,
                                          char *out_update_id,
                                          size_t id_capacity) {
    uint64_t id = 0;
    if (!steam_workshop_ready() || !rt_services_steam_parse_u64(item_id, &id))
        return 0;
    const uint64_t handle = g_steam.ugc.start_update(g_steam.ugc.self, steam_workshop_app_id(), id);
    if (handle == RT_STEAM_UGC_INVALID_HANDLE) {
        steam_workshop_call_failed("StartItemUpdate", item_id);
        return 0;
    }
    snprintf(out_update_id, id_capacity, "%llu", (unsigned long long)handle);
    return 1;
}

/// @brief Replace a pending update's tags from a comma-separated list.
/// @param handle UGCUpdateHandle_t.
/// @param tags Comma-separated tags; spaces around tags are ignored.
/// @return 1 when Steam accepted the tags.
static int steam_workshop_set_tags(uint64_t handle, const char *tags) {
    const size_t length = strlen(tags);
    char *copy = (char *)malloc(length + 1);
    const char **list = (const char **)calloc(length / 2 + 2, sizeof(const char *));
    if (!copy || !list) {
        free(copy);
        free((void *)list);
        return 0;
    }
    memcpy(copy, tags, length + 1);
    int32_t count = 0;
    char *cursor = copy;
    while (*cursor) {
        char *comma = strchr(cursor, ',');
        if (comma)
            *comma = '\0';
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        char *end = cursor + strlen(cursor);
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*cursor)
            list[count++] = cursor;
        if (!comma)
            break;
        cursor = comma + 1;
    }
    rt_steam_param_string_array array;
    memset(&array, 0, sizeof(array));
    array.strings = list;
    array.count = count;
    const int accepted = g_steam.ugc.set_tags(g_steam.ugc.self, handle, &array, false) ? 1 : 0;
    free((void *)list);
    free(copy);
    return accepted;
}

/// @brief Set a text field of a pending update.
/// @param update_id Decimal update id.
/// @param field Field to set.
/// @param text Borrowed text (an absolute path for content and preview).
/// @return 1 when accepted.
static int8_t steam_workshop_set_update_text(const char *update_id,
                                             rt_services_workshop_field field,
                                             const char *text) {
    uint64_t handle = 0;
    if (!steam_workshop_ready() || !steam_workshop_parse_update(update_id, &handle))
        return 0;
    const char *method = "SetItemTitle";
    int accepted = 0;
    switch (field) {
        case RT_SERVICES_WORKSHOP_FIELD_TITLE:
            accepted = g_steam.ugc.set_title(g_steam.ugc.self, handle, text);
            break;
        case RT_SERVICES_WORKSHOP_FIELD_DESCRIPTION:
            method = "SetItemDescription";
            accepted = g_steam.ugc.set_description(g_steam.ugc.self, handle, text);
            break;
        case RT_SERVICES_WORKSHOP_FIELD_METADATA:
            method = "SetItemMetadata";
            accepted = g_steam.ugc.set_metadata(g_steam.ugc.self, handle, text);
            break;
        case RT_SERVICES_WORKSHOP_FIELD_TAGS:
            method = "SetItemTags";
            accepted = steam_workshop_set_tags(handle, text);
            break;
        case RT_SERVICES_WORKSHOP_FIELD_CONTENT:
            method = "SetItemContent";
            accepted = g_steam.ugc.set_content(g_steam.ugc.self, handle, text);
            break;
        case RT_SERVICES_WORKSHOP_FIELD_PREVIEW:
            method = "SetItemPreview";
            accepted = g_steam.ugc.set_preview(g_steam.ugc.self, handle, text);
            break;
        default:
            return 0;
    }
    if (accepted)
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: %s(%s) failed; check the update id and the length of the text (%zu bytes)",
        method,
        update_id,
        strlen(text));
    return 0;
}

/// @brief Set the visibility of a pending update.
/// @param update_id Decimal update id.
/// @param visibility WorkshopVisibility value (same ordinals as
///        ERemoteStoragePublishedFileVisibility).
/// @return 1 when accepted.
static int8_t steam_workshop_set_update_visibility(const char *update_id, int64_t visibility) {
    uint64_t handle = 0;
    if (!steam_workshop_ready() || !steam_workshop_parse_update(update_id, &handle))
        return 0;
    if (g_steam.ugc.set_visibility(g_steam.ugc.self, handle, (int)visibility))
        return 1;
    steam_workshop_call_failed("SetItemVisibility", update_id);
    return 0;
}

/// @brief Read the progress of a submitted update.
/// @param update_id Decimal update id.
/// @param out_status Receives the WorkshopUpdateStatus (same ordinals as EItemUpdateStatus).
/// @param out_processed Receives bytes processed.
/// @param out_total Receives bytes to process.
/// @return 1 when written.
static int8_t steam_workshop_update_progress(const char *update_id,
                                             int64_t *out_status,
                                             int64_t *out_processed,
                                             int64_t *out_total) {
    uint64_t handle = 0;
    if (!steam_workshop_ready() || !steam_workshop_parse_update(update_id, &handle))
        return 0;
    uint64_t processed = 0;
    uint64_t total = 0;
    const int status = g_steam.ugc.update_progress(g_steam.ugc.self, handle, &processed, &total);
    *out_status = (int64_t)status;
    *out_processed = processed > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)processed;
    *out_total = total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
    return 1;
}

/// @brief Steam Workshop operations.
const rt_services_workshop_ops rt_services_steam_workshop_ops = {
    .check_item_id = steam_workshop_check_item_id,
    .check_update_id = steam_workshop_check_update_id,
    .subscribed_count = steam_workshop_subscribed_count,
    .subscribed_id_at = steam_workshop_subscribed_id_at,
    .item_state = steam_workshop_item_state,
    .install_info = steam_workshop_install_info,
    .download_info = steam_workshop_download_info,
    .download = steam_workshop_download,
    .start_update = steam_workshop_start_update,
    .set_update_text = steam_workshop_set_update_text,
    .set_update_visibility = steam_workshop_set_update_visibility,
    .update_progress = steam_workshop_update_progress,
};

//===----------------------------------------------------------------------===//
// Requests
//===----------------------------------------------------------------------===//

/// @brief Map a WorkshopQuery value to EUGCQuery.
/// @param order WorkshopQuery value.
/// @return EUGCQuery value.
static int steam_workshop_query_type(int64_t order) {
    switch (order) {
        case RT_SERVICES_WORKSHOP_QUERY_NEWEST:
            return 1;
        case RT_SERVICES_WORKSHOP_QUERY_TRENDING:
            return 3;
        case RT_SERVICES_WORKSHOP_QUERY_MOST_SUBSCRIBED:
            return 12;
        case RT_SERVICES_WORKSHOP_QUERY_RECENTLY_UPDATED:
            return 19;
        case RT_SERVICES_WORKSHOP_QUERY_TEXT_SEARCH:
            return 11;
        case RT_SERVICES_WORKSHOP_QUERY_POPULAR:
        default:
            return 0;
    }
}

/// @brief Map a WorkshopList value to EUserUGCList.
/// @param list WorkshopList value.
/// @return EUserUGCList value.
static int steam_workshop_user_list(int64_t list) {
    switch (list) {
        case RT_SERVICES_WORKSHOP_LIST_SUBSCRIBED:
            return 6;
        case RT_SERVICES_WORKSHOP_LIST_FAVORITED:
            return 5;
        case RT_SERVICES_WORKSHOP_LIST_VOTED_UP:
            return 2;
        case RT_SERVICES_WORKSHOP_LIST_PLAYED:
            return 7;
        case RT_SERVICES_WORKSHOP_LIST_PUBLISHED:
        default:
            return 0;
    }
}

/// @brief Release a Workshop query handle.
/// @param handle UGCQueryHandle_t, or RT_STEAM_UGC_INVALID_HANDLE.
static void steam_workshop_release_query(uint64_t handle) {
    if (handle != RT_STEAM_UGC_INVALID_HANDLE && g_steam.ugc.release_query)
        g_steam.ugc.release_query(g_steam.ugc.self, handle);
}

/// @brief Release the query handle an operation owns.
/// @param op Operation being freed.
void rt_services_steam_workshop_op_release(steam_request_op *op) {
    if (op && op->stage == STEAM_OP_UGC_QUERY) {
        steam_workshop_release_query(op->ugc_query);
        op->ugc_query = RT_STEAM_UGC_INVALID_HANDLE;
    }
}

/// @brief Add each tag of a comma-separated list as a required tag.
/// @param handle Query handle.
/// @param tags Comma-separated tags.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message.
/// @return 1 when every tag was added.
static int steam_workshop_add_tags(uint64_t handle,
                                   const char *tags,
                                   char *message,
                                   size_t message_capacity) {
    const char *cursor = tags;
    char tag[256];
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        const char *end = comma ? comma : cursor + strlen(cursor);
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        const char *last = end;
        while (last > cursor && (last[-1] == ' ' || last[-1] == '\t'))
            --last;
        const size_t length = (size_t)(last - cursor);
        if (length >= sizeof(tag)) {
            snprintf(message,
                     message_capacity,
                     "Steam: Workshop tags hold at most 255 bytes (got %zu)",
                     length);
            return 0;
        }
        if (length > 0) {
            memcpy(tag, cursor, length);
            tag[length] = '\0';
            if (!g_steam.ugc.add_required_tag(g_steam.ugc.self, handle, tag)) {
                snprintf(message, message_capacity, "Steam: AddRequiredTag('%s') failed", tag);
                return 0;
            }
        }
        if (!comma)
            break;
        cursor = comma + 1;
    }
    return 1;
}

/// @brief Create the Steam query a WorkshopQuery request describes.
/// @param args Validated arguments.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message.
/// @return Query handle, or RT_STEAM_UGC_INVALID_HANDLE.
static uint64_t steam_workshop_create_query(const rt_services_request_args *args,
                                            char *message,
                                            size_t message_capacity) {
    const uint32_t app = steam_workshop_app_id();
    if ((args->query || args->list) && args->page > (int64_t)UINT32_MAX) {
        snprintf(message,
                 message_capacity,
                 "Steam: Workshop page %lld is out of range",
                 (long long)args->page);
        return RT_STEAM_UGC_INVALID_HANDLE;
    }
    if (args->query) {
        return g_steam.ugc.query_all(g_steam.ugc.self,
                                     steam_workshop_query_type(args->query),
                                     RT_STEAM_UGC_MATCHING_ITEMS_READY_TO_USE,
                                     app,
                                     app,
                                     (uint32_t)args->page);
    }
    if (args->list) {
        if (!g_steam.user.self || !g_steam.user.get_steam_id) {
            snprintf(message,
                     message_capacity,
                     "Steam: Workshop.QueryUser needs the user identity (see "
                     "Platform.Diagnostics)");
            return RT_STEAM_UGC_INVALID_HANDLE;
        }
        const uint32_t account =
            (uint32_t)(g_steam.user.get_steam_id(g_steam.user.self) & UINT64_C(0xFFFFFFFF));
        const int sort = args->list == RT_SERVICES_WORKSHOP_LIST_SUBSCRIBED
                             ? RT_STEAM_UGC_SORT_SUBSCRIPTION_DATE_DESC
                             : RT_STEAM_UGC_SORT_CREATION_DESC;
        return g_steam.ugc.query_user(g_steam.ugc.self,
                                      account,
                                      steam_workshop_user_list(args->list),
                                      RT_STEAM_UGC_MATCHING_ITEMS_READY_TO_USE,
                                      sort,
                                      app,
                                      app,
                                      (uint32_t)args->page);
    }
    uint64_t ids[RT_SERVICES_REQUEST_ITEM_CAPACITY];
    uint32_t count = 0;
    const char *cursor = args->items;
    while (*cursor) {
        char entry[32];
        const char *comma = strchr(cursor, ',');
        const size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length < sizeof(entry) && count < RT_SERVICES_REQUEST_ITEM_CAPACITY) {
            memcpy(entry, cursor, length);
            entry[length] = '\0';
            if (rt_services_steam_parse_u64(entry, &ids[count]))
                ++count;
        }
        if (!comma)
            break;
        cursor = comma + 1;
    }
    if (count > RT_STEAM_UGC_RESULTS_PER_PAGE) {
        snprintf(message,
                 message_capacity,
                 "Steam: Workshop.QueryItems takes at most %d item ids (got %u)",
                 RT_STEAM_UGC_RESULTS_PER_PAGE,
                 (unsigned)count);
        return RT_STEAM_UGC_INVALID_HANDLE;
    }
    return g_steam.ugc.query_details(g_steam.ugc.self, ids, count);
}

/// @brief Start a Workshop query request.
/// @param args Validated arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message.
/// @return 1 when started.
static int8_t steam_workshop_begin_query(const rt_services_request_args *args,
                                         uint64_t *out_handle,
                                         char *message,
                                         size_t message_capacity) {
    message[0] = '\0';
    const uint64_t handle = steam_workshop_create_query(args, message, message_capacity);
    if (handle == RT_STEAM_UGC_INVALID_HANDLE) {
        if (!message[0])
            snprintf(message, message_capacity, "Steam: the Workshop query could not be created");
        return 0;
    }
    if (!steam_workshop_add_tags(handle, args->tags, message, message_capacity)) {
        steam_workshop_release_query(handle);
        return 0;
    }
    if (args->search[0] && !g_steam.ugc.set_search_text(g_steam.ugc.self, handle, args->search)) {
        steam_workshop_release_query(handle);
        snprintf(message, message_capacity, "Steam: SetSearchText failed");
        return 0;
    }
    (void)g_steam.ugc.return_long_description(g_steam.ugc.self, handle, true);
    (void)g_steam.ugc.return_metadata(g_steam.ugc.self, handle, true);
    steam_request_op *op = rt_services_steam_op_alloc(STEAM_OP_UGC_QUERY);
    if (!op) {
        steam_workshop_release_query(handle);
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    const rt_steam_api_call call = g_steam.ugc.send_query(g_steam.ugc.self, handle);
    if (call == 0) {
        rt_services_steam_op_free(op);
        steam_workshop_release_query(handle);
        snprintf(message,
                 message_capacity,
                 "Steam: SendQueryUGCRequest returned an invalid call handle");
        return 0;
    }
    op->ugc_query = handle;
    op->call = call;
    *out_handle = op->token;
    return 1;
}

/// @brief Start a Workshop request.
/// @param args Validated arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message.
/// @return 1 when started.
int8_t rt_services_steam_begin_workshop_request(const rt_services_request_args *args,
                                                uint64_t *out_handle,
                                                char *message,
                                                size_t message_capacity) {
    if (!steam_workshop_ready()) {
        snprintf(message,
                 message_capacity,
                 "Steam: the Workshop is unavailable (see Platform.Diagnostics)");
        return 0;
    }
    if (args->kind == RT_SERVICES_REQUEST_WORKSHOP_QUERY)
        return steam_workshop_begin_query(args, out_handle, message, message_capacity);

    uint64_t id = 0;
    steam_op_stage stage = STEAM_OP_UGC_CREATE;
    const char *method = "CreateItem";
    switch (args->kind) {
        case RT_SERVICES_REQUEST_WORKSHOP_SUBSCRIBE:
            stage = STEAM_OP_UGC_SUBSCRIBE;
            method = "SubscribeItem";
            break;
        case RT_SERVICES_REQUEST_WORKSHOP_UNSUBSCRIBE:
            stage = STEAM_OP_UGC_UNSUBSCRIBE;
            method = "UnsubscribeItem";
            break;
        case RT_SERVICES_REQUEST_WORKSHOP_DELETE:
            stage = STEAM_OP_UGC_DELETE;
            method = "DeleteItem";
            break;
        case RT_SERVICES_REQUEST_WORKSHOP_SUBMIT:
            stage = STEAM_OP_UGC_SUBMIT;
            method = "SubmitItemUpdate";
            break;
        case RT_SERVICES_REQUEST_WORKSHOP_CREATE:
            break;
        default:
            snprintf(message,
                     message_capacity,
                     "Steam: request kind %lld is not supported",
                     (long long)args->kind);
            return 0;
    }
    if (stage == STEAM_OP_UGC_SUBMIT) {
        if (!steam_workshop_parse_update(args->item_id, &id)) {
            snprintf(message, message_capacity, "Steam: invalid update id '%s'", args->item_id);
            return 0;
        }
    } else if (stage != STEAM_OP_UGC_CREATE && !rt_services_steam_parse_u64(args->item_id, &id)) {
        snprintf(message, message_capacity, "Steam: invalid Workshop item id '%s'", args->item_id);
        return 0;
    }
    steam_request_op *op = rt_services_steam_op_alloc(stage);
    if (!op) {
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    op->ugc_query = RT_STEAM_UGC_INVALID_HANDLE;
    snprintf(op->name, sizeof(op->name), "%s", args->item_id);
    rt_steam_api_call call = 0;
    switch (stage) {
        case STEAM_OP_UGC_SUBSCRIBE:
            call = g_steam.ugc.subscribe(g_steam.ugc.self, id);
            break;
        case STEAM_OP_UGC_UNSUBSCRIBE:
            call = g_steam.ugc.unsubscribe(g_steam.ugc.self, id);
            break;
        case STEAM_OP_UGC_DELETE:
            call = g_steam.ugc.delete_item(g_steam.ugc.self, id);
            break;
        case STEAM_OP_UGC_SUBMIT:
            call = g_steam.ugc.submit_update(g_steam.ugc.self, id, args->text);
            break;
        default:
            call = g_steam.ugc.create_item(
                g_steam.ugc.self, steam_workshop_app_id(), RT_STEAM_UGC_FILE_TYPE_COMMUNITY);
            break;
    }
    if (call == 0) {
        rt_services_steam_op_free(op);
        snprintf(message, message_capacity, "Steam: %s returned an invalid call handle", method);
        return 0;
    }
    op->call = call;
    *out_handle = op->token;
    return 1;
}

//===----------------------------------------------------------------------===//
// Completions
//===----------------------------------------------------------------------===//

/// @brief Heap copies of one query result's text fields.
typedef struct steam_workshop_item_text {
    char id[RT_SERVICES_WORKSHOP_ID_CAPACITY];    ///< Item id.
    char owner[RT_SERVICES_WORKSHOP_ID_CAPACITY]; ///< Author id.
    char preview[RT_STEAM_UGC_URL_CAPACITY];      ///< Preview URL.
    char *title;                                  ///< Heap title.
    char *description;                            ///< Heap description.
    char *tags;                                   ///< Heap tags.
    char *metadata;                               ///< Heap metadata.
} steam_workshop_item_text;

/// @brief Complete a Workshop query.
/// @param op Operation in the query stage.
/// @param completed Decoded SteamAPICallCompleted_t.
static void steam_workshop_query_completed(steam_request_op *op,
                                           const rt_steam_api_call_completed *completed) {
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    const uint64_t handle = op->ugc_query;
    op->ugc_query = RT_STEAM_UGC_INVALID_HANDLE;
    rt_steam_ugc_query_completed done;
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_UGC_QUERY_COMPLETED,
                                             &done,
                                             sizeof(done),
                                             "SendQueryUGCRequest",
                                             error,
                                             sizeof(error))) {
        steam_workshop_release_query(handle);
        rt_services_steam_op_fail(op, error);
        return;
    }
    if (done.result != RT_STEAM_RESULT_OK) {
        steam_workshop_release_query(handle);
        snprintf(
            error, sizeof(error), "Steam: the Workshop query failed (EResult %d)", done.result);
        rt_services_provider_complete_request(op->token, 0, done.result, 0, error);
        rt_services_steam_op_free(op);
        return;
    }
    uint32_t count = done.returned;
    if (count > RT_SERVICES_REQUEST_ITEM_CAPACITY)
        count = RT_SERVICES_REQUEST_ITEM_CAPACITY;
    rt_services_workshop_item *items =
        count ? (rt_services_workshop_item *)calloc(count, sizeof(rt_services_workshop_item))
              : NULL;
    steam_workshop_item_text *texts =
        count ? (steam_workshop_item_text *)calloc(count, sizeof(steam_workshop_item_text)) : NULL;
    rt_steam_ugc_details *details = (rt_steam_ugc_details *)malloc(sizeof(rt_steam_ugc_details));
    char *metadata = (char *)malloc(RT_STEAM_UGC_METADATA_CAPACITY);
    int64_t item_count = 0;
    int out_of_memory = count && (!items || !texts);
    if (!details || !metadata)
        out_of_memory = 1;
    for (uint32_t i = 0; !out_of_memory && i < count; ++i) {
        memset(details, 0, sizeof(*details));
        if (!g_steam.ugc.query_result(g_steam.ugc.self, handle, i, details))
            continue;
        steam_workshop_item_text *text = &texts[item_count];
        snprintf(text->id, sizeof(text->id), "%llu", (unsigned long long)details->file_id);
        snprintf(text->owner, sizeof(text->owner), "%llu", (unsigned long long)details->owner);
        text->title = steam_workshop_copy_field(details->title, sizeof(details->title));
        text->description =
            steam_workshop_copy_field(details->description, sizeof(details->description));
        text->tags = steam_workshop_copy_field(details->tags, sizeof(details->tags));
        if (!g_steam.ugc.preview_url(
                g_steam.ugc.self, handle, i, text->preview, sizeof(text->preview)))
            text->preview[0] = '\0';
        text->preview[sizeof(text->preview) - 1] = '\0';
        metadata[0] = '\0';
        if (!g_steam.ugc.query_metadata(
                g_steam.ugc.self, handle, i, metadata, RT_STEAM_UGC_METADATA_CAPACITY))
            metadata[0] = '\0';
        text->metadata = steam_workshop_copy_field(metadata, RT_STEAM_UGC_METADATA_CAPACITY);
        if (!text->title || !text->description || !text->tags || !text->metadata) {
            out_of_memory = 1;
            ++item_count;
            break;
        }
        rt_services_workshop_item *item = &items[item_count];
        item->id = text->id;
        item->title = text->title;
        item->description = text->description;
        item->owner_id = text->owner;
        item->tags = text->tags;
        item->preview_url = text->preview;
        item->metadata = text->metadata;
        item->created = (int64_t)details->created;
        item->updated = (int64_t)details->updated;
        item->visibility = details->visibility >= 0 && details->visibility <= 3
                               ? (int64_t)details->visibility
                               : RT_SERVICES_WORKSHOP_VISIBILITY_PRIVATE;
        item->votes_up = (int64_t)details->votes_up;
        item->votes_down = (int64_t)details->votes_down;
        item->size = details->total_files_size != 0
                         ? (details->total_files_size > (uint64_t)INT64_MAX
                                ? INT64_MAX
                                : (int64_t)details->total_files_size)
                         : (details->file_size > 0 ? (int64_t)details->file_size : 0);
        item->score =
            details->score >= 0.0f && details->score <= 1.0f ? (double)details->score : 0.0;
        ++item_count;
    }
    steam_workshop_release_query(handle);
    if (out_of_memory) {
        rt_services_steam_op_fail(op, "Steam: out of memory reading Workshop query results");
    } else {
        rt_services_request_result result;
        memset(&result, 0, sizeof(result));
        result.succeeded = 1;
        result.result_code = RT_STEAM_RESULT_OK;
        result.value = (int64_t)done.total;
        result.flag = done.cached ? 1 : 0;
        result.items = items;
        result.item_count = item_count;
        rt_services_provider_finish_request(op->token, &result);
        rt_services_steam_op_free(op);
    }
    for (int64_t i = 0; texts && i < item_count; ++i) {
        free(texts[i].title);
        free(texts[i].description);
        free(texts[i].tags);
        free(texts[i].metadata);
    }
    free(texts);
    free(items);
    free(details);
    free(metadata);
}

/// @brief Complete a request whose call result names one item.
/// @param op Operation in the subscribe, unsubscribe, or delete stage.
/// @param completed Decoded SteamAPICallCompleted_t.
static void steam_workshop_file_completed(steam_request_op *op,
                                          const rt_steam_api_call_completed *completed) {
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    const int callback_id = op->stage == STEAM_OP_UGC_SUBSCRIBE ? RT_STEAM_CB_UGC_SUBSCRIBE_RESULT
                            : op->stage == STEAM_OP_UGC_UNSUBSCRIBE
                                ? RT_STEAM_CB_UGC_UNSUBSCRIBE_RESULT
                                : RT_STEAM_CB_UGC_DELETE_ITEM_RESULT;
    const char *method = op->stage == STEAM_OP_UGC_SUBSCRIBE     ? "SubscribeItem"
                         : op->stage == STEAM_OP_UGC_UNSUBSCRIBE ? "UnsubscribeItem"
                                                                 : "DeleteItem";
    rt_steam_ugc_file_result done;
    if (!rt_services_steam_fetch_call_result(
            completed, callback_id, &done, sizeof(done), method, error, sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.result_code = done.result;
    result.text = op->name;
    if (done.result == RT_STEAM_RESULT_OK) {
        result.succeeded = 1;
    } else {
        snprintf(error,
                 sizeof(error),
                 "Steam: %s('%s') failed (EResult %d)",
                 method,
                 op->name,
                 done.result);
        result.error = error;
    }
    // Finish before freeing the slot: result.text points into op->name.
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
}

/// @brief Complete an item creation or update submission.
/// @param op Operation in the create or submit stage.
/// @param completed Decoded SteamAPICallCompleted_t.
static void steam_workshop_publish_completed(steam_request_op *op,
                                             const rt_steam_api_call_completed *completed) {
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    char id[RT_SERVICES_WORKSHOP_ID_CAPACITY];
    int32_t code = 0;
    uint64_t file_id = 0;
    uint8_t needs_agreement = 0;
    const int create = op->stage == STEAM_OP_UGC_CREATE;
    const char *method = create ? "CreateItem" : "SubmitItemUpdate";
    if (create) {
        rt_steam_ugc_create_item_result done;
        if (!rt_services_steam_fetch_call_result(completed,
                                                 RT_STEAM_CB_UGC_CREATE_ITEM_RESULT,
                                                 &done,
                                                 sizeof(done),
                                                 method,
                                                 error,
                                                 sizeof(error))) {
            rt_services_steam_op_fail(op, error);
            return;
        }
        code = done.result;
        file_id = done.file_id;
        needs_agreement = done.needs_agreement;
    } else {
        rt_steam_ugc_submit_item_update_result done;
        if (!rt_services_steam_fetch_call_result(completed,
                                                 RT_STEAM_CB_UGC_SUBMIT_ITEM_UPDATE_RESULT,
                                                 &done,
                                                 sizeof(done),
                                                 method,
                                                 error,
                                                 sizeof(error))) {
            rt_services_steam_op_fail(op, error);
            return;
        }
        code = done.result;
        file_id = done.file_id;
        needs_agreement = done.needs_agreement;
    }
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.result_code = code;
    id[0] = '\0';
    if (file_id != 0)
        snprintf(id, sizeof(id), "%llu", (unsigned long long)file_id);
    result.text = id;
    result.flag = needs_agreement ? 1 : 0;
    if (code == RT_STEAM_RESULT_OK) {
        result.succeeded = 1;
    } else {
        snprintf(error, sizeof(error), "Steam: %s failed (EResult %d)", method, code);
        result.error = error;
    }
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
}

/// @brief Complete a Workshop request.
/// @param op Operation in a Workshop stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_workshop_call_completed(steam_request_op *op,
                                               const rt_steam_api_call_completed *completed) {
    switch (op->stage) {
        case STEAM_OP_UGC_QUERY:
            steam_workshop_query_completed(op, completed);
            break;
        case STEAM_OP_UGC_SUBSCRIBE:
        case STEAM_OP_UGC_UNSUBSCRIBE:
        case STEAM_OP_UGC_DELETE:
            steam_workshop_file_completed(op, completed);
            break;
        case STEAM_OP_UGC_CREATE:
        case STEAM_OP_UGC_SUBMIT:
            steam_workshop_publish_completed(op, completed);
            break;
        default:
            break;
    }
}

//===----------------------------------------------------------------------===//
// Callbacks
//===----------------------------------------------------------------------===//

/// @brief Decode Workshop callbacks.
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a Workshop callback, otherwise 0.
int rt_services_steam_workshop_callback(const rt_steam_callback_msg *msg) {
    char id[RT_SERVICES_WORKSHOP_ID_CAPACITY];
    switch (msg->callback_id) {
        case RT_STEAM_CB_UGC_ITEM_INSTALLED: {
            rt_steam_ugc_item_installed payload;
            if (!steam_workshop_ready() || !rt_services_steam_payload_matches(msg, sizeof(payload)))
                return 1;
            memcpy(&payload, msg->param, sizeof(payload));
            if (payload.app_id != steam_workshop_app_id())
                return 1;
            snprintf(id, sizeof(id), "%llu", (unsigned long long)payload.file_id);
            rt_services_provider_emit_event(RT_SERVICES_EVENT_WORKSHOP_ITEM_INSTALLED, 0, 0, 0, id);
            return 1;
        }
        case RT_STEAM_CB_UGC_DOWNLOAD_ITEM_RESULT: {
            rt_steam_ugc_download_item_result payload;
            if (!steam_workshop_ready() || !rt_services_steam_payload_matches(msg, sizeof(payload)))
                return 1;
            memcpy(&payload, msg->param, sizeof(payload));
            if (payload.app_id != steam_workshop_app_id())
                return 1;
            snprintf(id, sizeof(id), "%llu", (unsigned long long)payload.file_id);
            rt_services_provider_emit_event(RT_SERVICES_EVENT_WORKSHOP_ITEM_DOWNLOADED,
                                            payload.result,
                                            0,
                                            payload.result == RT_STEAM_RESULT_OK ? 1 : 0,
                                            id);
            return 1;
        }
        case RT_STEAM_CB_UGC_FILE_SUBSCRIBED:
        case RT_STEAM_CB_UGC_FILE_UNSUBSCRIBED: {
            rt_steam_ugc_file_subscription payload;
            if (!steam_workshop_ready() || !rt_services_steam_payload_matches(msg, sizeof(payload)))
                return 1;
            memcpy(&payload, msg->param, sizeof(payload));
            if (payload.app_id != steam_workshop_app_id())
                return 1;
            snprintf(id, sizeof(id), "%llu", (unsigned long long)payload.file_id);
            rt_services_provider_emit_event(RT_SERVICES_EVENT_WORKSHOP_SUBSCRIPTION_CHANGED,
                                            0,
                                            0,
                                            msg->callback_id == RT_STEAM_CB_UGC_FILE_SUBSCRIBED ? 1
                                                                                                : 0,
                                            id);
            return 1;
        }
        default:
            return 0;
    }
}
