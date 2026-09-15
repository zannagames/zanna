//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_workshop.c
// Purpose: Implements the provider-neutral Zanna.Services.Workshop class on
//          top of the active provider's Workshop operations and request
//          kinds, the immutable Zanna.Services.WorkshopItem objects that
//          queries return, and the Workshop constant classes.
// Key invariants:
//   - Every Workshop member checks the main thread first, then the argument
//     rules that hold for every provider (traps), then the provider's id
//     format (traps while that provider is active), and only then delegates.
//   - Paths are made absolute here, and missing content folders or preview
//     files return false with a diagnostic before the provider sees them.
//   - A WorkshopItem copies every field when it is created and never changes.
// Ownership/Lifetime:
//   - WorkshopItem objects own their strings; the finalizer releases them.
//   - Normalized id lists are temporary heap buffers freed before returning.
// Links: src/runtime/services/rt_services_workshop.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0366-platform-services-workshop.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_workshop.c
 * @brief Implements Zanna.Services.Workshop and Zanna.Services.WorkshopItem.
 */

#include "rt_services_workshop.h"

#include "rt_dir.h"
#include "rt_file_ext.h"
#include "rt_object.h"
#include "rt_path.h"
#include "rt_services.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Zanna.Services.WorkshopItem
//===----------------------------------------------------------------------===//

/// @brief Payload of a Zanna.Services.WorkshopItem object.
typedef struct rt_services_workshop_item_impl {
    rt_string id;          ///< Owned item id.
    rt_string title;       ///< Owned title.
    rt_string description; ///< Owned description.
    rt_string owner_id;    ///< Owned author id.
    rt_string tags;        ///< Owned comma-separated tags.
    rt_string preview_url; ///< Owned preview URL.
    rt_string metadata;    ///< Owned developer metadata.
    int64_t created;       ///< Creation time in Unix seconds.
    int64_t updated;       ///< Last update time in Unix seconds.
    int64_t visibility;    ///< WorkshopVisibility value.
    int64_t votes_up;      ///< Up votes.
    int64_t votes_down;    ///< Down votes.
    int64_t size;          ///< Content size in bytes.
    double score;          ///< Vote score in 0..1.
} rt_services_workshop_item_impl;

/// @brief Release one owned string field.
/// @param field Field to clear.
static void workshop_item_release_text(rt_string *field) {
    if (*field) {
        rt_string_unref(*field);
        *field = NULL;
    }
}

/// @brief Finalizer releasing a WorkshopItem's strings.
/// @param obj Zero-reference payload.
static void workshop_item_finalize(void *obj) {
    rt_services_workshop_item_impl *item = (rt_services_workshop_item_impl *)obj;
    if (!item)
        return;
    workshop_item_release_text(&item->id);
    workshop_item_release_text(&item->title);
    workshop_item_release_text(&item->description);
    workshop_item_release_text(&item->owner_id);
    workshop_item_release_text(&item->tags);
    workshop_item_release_text(&item->preview_url);
    workshop_item_release_text(&item->metadata);
}

/// @brief Create a WorkshopItem from a provider record.
/// @param item Borrowed record.
/// @return New object with one reference, or NULL.
void *rt_services_internal_workshop_item_new(const rt_services_workshop_item *item) {
    rt_services_workshop_item_impl *impl = (rt_services_workshop_item_impl *)rt_obj_new_i64(
        RT_SERVICES_WORKSHOP_ITEM_CLASS_ID, (int64_t)sizeof(rt_services_workshop_item_impl));
    if (!impl)
        return NULL;
    memset(impl, 0, sizeof(*impl));
    rt_obj_set_finalizer(impl, workshop_item_finalize);
    impl->id = rt_services_internal_owned_text(item->id);
    impl->title = rt_services_internal_owned_text(item->title);
    impl->description = rt_services_internal_owned_text(item->description);
    impl->owner_id = rt_services_internal_owned_text(item->owner_id);
    impl->tags = rt_services_internal_owned_text(item->tags);
    impl->preview_url = rt_services_internal_owned_text(item->preview_url);
    impl->metadata = rt_services_internal_owned_text(item->metadata);
    impl->created = item->created;
    impl->updated = item->updated;
    impl->visibility = item->visibility;
    impl->votes_up = item->votes_up;
    impl->votes_down = item->votes_down;
    impl->size = item->size;
    impl->score = item->score;
    return impl;
}

/// @brief Validate a WorkshopItem handle.
/// @param item Candidate handle.
/// @param member Class-qualified member name for the trap.
/// @return Payload, or NULL for NULL or after a trap for a foreign handle.
static rt_services_workshop_item_impl *workshop_item_checked(void *item, const char *member) {
    if (!item)
        return NULL;
    if (!rt_obj_is_instance(
            item, RT_SERVICES_WORKSHOP_ITEM_CLASS_ID, sizeof(rt_services_workshop_item_impl))) {
        char message[192];
        snprintf(
            message, sizeof(message), "Services: %s: expected Zanna.Services.WorkshopItem", member);
        rt_trap(message);
        return NULL;
    }
    return (rt_services_workshop_item_impl *)item;
}

/// @brief Return a new reference to a string field.
/// @param item Candidate handle.
/// @param member Class-qualified member name.
/// @param offset Offset of the rt_string field.
/// @return Caller-owned string, or the empty string.
static rt_string workshop_item_text(void *item, const char *member, size_t offset) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, member);
    if (!impl)
        return rt_str_empty();
    rt_string value = *(rt_string *)((char *)impl + offset);
    return value ? rt_string_ref(value) : rt_str_empty();
}

/// @brief Read WorkshopItem.Id. @param item Borrowed item. @return Caller-owned id.
rt_string rt_services_workshop_item_get_id(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.Id", offsetof(rt_services_workshop_item_impl, id));
}

/// @brief Read WorkshopItem.Title. @param item Borrowed item. @return Caller-owned title.
rt_string rt_services_workshop_item_get_title(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.Title", offsetof(rt_services_workshop_item_impl, title));
}

/// @brief Read WorkshopItem.Description. @param item Borrowed item. @return Caller-owned text.
rt_string rt_services_workshop_item_get_description(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.Description", offsetof(rt_services_workshop_item_impl, description));
}

/// @brief Read WorkshopItem.OwnerId. @param item Borrowed item. @return Caller-owned id.
rt_string rt_services_workshop_item_get_owner_id(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.OwnerId", offsetof(rt_services_workshop_item_impl, owner_id));
}

/// @brief Read WorkshopItem.Tags. @param item Borrowed item. @return Caller-owned tags.
rt_string rt_services_workshop_item_get_tags(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.Tags", offsetof(rt_services_workshop_item_impl, tags));
}

/// @brief Read WorkshopItem.PreviewUrl. @param item Borrowed item. @return Caller-owned URL.
rt_string rt_services_workshop_item_get_preview_url(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.PreviewUrl", offsetof(rt_services_workshop_item_impl, preview_url));
}

/// @brief Read WorkshopItem.Metadata. @param item Borrowed item. @return Caller-owned metadata.
rt_string rt_services_workshop_item_get_metadata(void *item) {
    return workshop_item_text(
        item, "WorkshopItem.Metadata", offsetof(rt_services_workshop_item_impl, metadata));
}

/// @brief Read WorkshopItem.Created. @param item Borrowed item. @return Unix seconds.
int64_t rt_services_workshop_item_get_created(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.Created");
    return impl ? impl->created : 0;
}

/// @brief Read WorkshopItem.Updated. @param item Borrowed item. @return Unix seconds.
int64_t rt_services_workshop_item_get_updated(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.Updated");
    return impl ? impl->updated : 0;
}

/// @brief Read WorkshopItem.Visibility. @param item Borrowed item. @return WorkshopVisibility.
int64_t rt_services_workshop_item_get_visibility(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.Visibility");
    return impl ? impl->visibility : 0;
}

/// @brief Read WorkshopItem.VotesUp. @param item Borrowed item. @return Count.
int64_t rt_services_workshop_item_get_votes_up(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.VotesUp");
    return impl ? impl->votes_up : 0;
}

/// @brief Read WorkshopItem.VotesDown. @param item Borrowed item. @return Count.
int64_t rt_services_workshop_item_get_votes_down(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.VotesDown");
    return impl ? impl->votes_down : 0;
}

/// @brief Read WorkshopItem.Size. @param item Borrowed item. @return Bytes.
int64_t rt_services_workshop_item_get_size(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.Size");
    return impl ? impl->size : 0;
}

/// @brief Read WorkshopItem.Score. @param item Borrowed item. @return Score in 0..1.
double rt_services_workshop_item_get_score(void *item) {
    rt_services_workshop_item_impl *impl = workshop_item_checked(item, "WorkshopItem.Score");
    return impl ? impl->score : 0.0;
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's Workshop operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_workshop_ops *workshop_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->workshop : NULL;
}

/// @brief Validate an item id argument.
/// @param member Class-qualified member name.
/// @param item_id Item id argument.
/// @return Borrowed id bytes, or NULL after a trap.
static const char *workshop_item_id(const char *member, rt_string item_id) {
    const char *id = rt_services_internal_require_name(item_id, member, "item id");
    if (!id)
        return NULL;
    const rt_services_workshop_ops *ops = workshop_ops();
    if (ops && ops->check_item_id && !ops->check_item_id(member, id))
        return NULL;
    return id;
}

/// @brief Validate an update id argument.
/// @param member Class-qualified member name.
/// @param update_id Update id argument.
/// @return Borrowed id bytes, or NULL after a trap.
static const char *workshop_update_id(const char *member, rt_string update_id) {
    const char *id = rt_services_internal_require_name(update_id, member, "update id");
    if (!id)
        return NULL;
    const rt_services_workshop_ops *ops = workshop_ops();
    if (ops && ops->check_update_id && !ops->check_update_id(member, id))
        return NULL;
    return id;
}

/// @brief Read an item's state flags.
/// @param member Class-qualified member name.
/// @param item_id Item id argument.
/// @return RT_SERVICES_WORKSHOP_STATE_* flags, or 0.
static int64_t workshop_state(const char *member, rt_string item_id) {
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = workshop_item_id(member, item_id);
    if (!id)
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    return (ops && ops->item_state) ? ops->item_state(id) : 0;
}

/// @brief Read an installed item's folder, size, and time.
/// @param member Class-qualified member name.
/// @param item_id Item id argument.
/// @param folder Destination of RT_SERVICES_WORKSHOP_FOLDER_CAPACITY bytes.
/// @param size Receives the size.
/// @param time Receives the install time.
/// @return 1 when installed, otherwise 0.
static int workshop_install(
    const char *member, rt_string item_id, char *folder, int64_t *size, int64_t *time) {
    folder[0] = '\0';
    *size = 0;
    *time = 0;
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = workshop_item_id(member, item_id);
    if (!id)
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    return ops && ops->install_info &&
           ops->install_info(id, folder, RT_SERVICES_WORKSHOP_FOLDER_CAPACITY, size, time);
}

/// @brief Read an item's download progress.
/// @param member Class-qualified member name.
/// @param item_id Item id argument.
/// @param downloaded Receives the bytes downloaded.
/// @param total Receives the bytes to download.
static void workshop_download_info(const char *member,
                                   rt_string item_id,
                                   int64_t *downloaded,
                                   int64_t *total) {
    *downloaded = 0;
    *total = 0;
    if (!rt_services_provider_require_main_thread(member))
        return;
    const char *id = workshop_item_id(member, item_id);
    if (!id)
        return;
    const rt_services_workshop_ops *ops = workshop_ops();
    if (!ops || !ops->download_info || !ops->download_info(id, downloaded, total)) {
        *downloaded = 0;
        *total = 0;
    }
}

/// @brief Start a request that names one item or update.
/// @param member Class-qualified member name.
/// @param kind RT_SERVICES_REQUEST_WORKSHOP_* kind.
/// @param id Validated item or update id.
/// @param text Borrowed text argument ("" when unused).
/// @return Caller-owned request.
static void *workshop_request(const char *member, int64_t kind, const char *id, const char *text) {
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = kind;
    args.name = "";
    args.text = text;
    args.item_id = id;
    args.items = "";
    args.tags = "";
    args.search = "";
    return rt_services_internal_begin_request(&args, member);
}

/// @brief Trap unless a page number is 1 or more.
/// @param member Class-qualified member name.
/// @param page Candidate page.
/// @return 1 when valid, 0 after the trap.
static int workshop_require_page(const char *member, int64_t page) {
    if (page >= 1)
        return 1;
    rt_services_internal_trap_argument(
        member, "page must be 1 or more (got %lld)", (long long)page);
    return 0;
}

/// @brief Set a text field of a pending update.
/// @param member Class-qualified member name.
/// @param update_id Update id argument.
/// @param field Field to set.
/// @param text Text argument.
/// @return 1 when accepted, otherwise 0.
static int8_t workshop_set_text(const char *member,
                                rt_string update_id,
                                rt_services_workshop_field field,
                                rt_string text) {
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = workshop_update_id(member, update_id);
    if (!id)
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    return (ops && ops->set_update_text &&
            ops->set_update_text(id, field, rt_services_internal_cstr(text)))
               ? 1
               : 0;
}

/// @brief Set a path field of a pending update after checking the path exists.
/// @param member Class-qualified member name.
/// @param update_id Update id argument.
/// @param field RT_SERVICES_WORKSHOP_FIELD_CONTENT or _PREVIEW.
/// @param path Path argument.
/// @return 1 when accepted, otherwise 0.
static int8_t workshop_set_path(const char *member,
                                rt_string update_id,
                                rt_services_workshop_field field,
                                rt_string path) {
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = workshop_update_id(member, update_id);
    if (!id)
        return 0;
    const int folder = field == RT_SERVICES_WORKSHOP_FIELD_CONTENT;
    if (!rt_services_internal_require_name(path, member, folder ? "folder" : "file"))
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    if (!ops || !ops->set_update_text)
        return 0;
    rt_string absolute = rt_path_abs(path);
    if (!absolute)
        return 0;
    int8_t accepted = 0;
    const int exists = folder ? rt_dir_exists(absolute) != 0 : rt_io_file_exists(absolute) != 0;
    if (!exists) {
        rt_services_provider_add_diagnostic("Services: %s found no %s at '%s'",
                                            member,
                                            folder ? "folder" : "file",
                                            rt_string_cstr(absolute));
    } else {
        accepted = ops->set_update_text(id, field, rt_string_cstr(absolute)) ? 1 : 0;
    }
    rt_string_unref(absolute);
    return accepted;
}

//===----------------------------------------------------------------------===//
// Subscribed items
//===----------------------------------------------------------------------===//

/// @brief Count subscribed items.
/// @return Count, or 0.
int64_t rt_services_workshop_get_subscribed_count(void) {
    if (!rt_services_provider_require_main_thread("Workshop.SubscribedCount"))
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    const int64_t count = (ops && ops->subscribed_count) ? ops->subscribed_count() : 0;
    return count < 0 ? 0 : count;
}

/// @brief Read the id of a subscribed item.
/// @param index Index.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_workshop_subscribed_id_at(int64_t index) {
    if (!rt_services_provider_require_main_thread("Workshop.SubscribedIdAt"))
        return rt_str_empty();
    const rt_services_workshop_ops *ops = workshop_ops();
    if (!ops || !ops->subscribed_id_at || index < 0)
        return rt_str_empty();
    char id[RT_SERVICES_WORKSHOP_ID_CAPACITY];
    id[0] = '\0';
    return ops->subscribed_id_at(index, id, sizeof(id)) ? rt_services_internal_owned_text(id)
                                                        : rt_str_empty();
}

/// @brief Report whether the player is subscribed to an item.
/// @param item_id Item id.
/// @return 1 when subscribed, otherwise 0.
int8_t rt_services_workshop_is_subscribed(rt_string item_id) {
    return (workshop_state("Workshop.IsSubscribed", item_id) &
            RT_SERVICES_WORKSHOP_STATE_SUBSCRIBED)
               ? 1
               : 0;
}

/// @brief Report whether an item is installed.
/// @param item_id Item id.
/// @return 1 when installed, otherwise 0.
int8_t rt_services_workshop_is_installed(rt_string item_id) {
    return (workshop_state("Workshop.IsInstalled", item_id) & RT_SERVICES_WORKSHOP_STATE_INSTALLED)
               ? 1
               : 0;
}

/// @brief Report whether an item needs a download or update.
/// @param item_id Item id.
/// @return 1 when it does, otherwise 0.
int8_t rt_services_workshop_needs_update(rt_string item_id) {
    return (workshop_state("Workshop.NeedsUpdate", item_id) &
            RT_SERVICES_WORKSHOP_STATE_NEEDS_UPDATE)
               ? 1
               : 0;
}

/// @brief Report whether an item is downloading.
/// @param item_id Item id.
/// @return 1 when downloading or pending, otherwise 0.
int8_t rt_services_workshop_is_downloading(rt_string item_id) {
    return (workshop_state("Workshop.IsDownloading", item_id) &
            (RT_SERVICES_WORKSHOP_STATE_DOWNLOADING | RT_SERVICES_WORKSHOP_STATE_DOWNLOAD_PENDING))
               ? 1
               : 0;
}

/// @brief Read an installed item's folder.
/// @param item_id Item id.
/// @return Caller-owned folder, or the empty string.
rt_string rt_services_workshop_install_folder(rt_string item_id) {
    char folder[RT_SERVICES_WORKSHOP_FOLDER_CAPACITY];
    int64_t size = 0;
    int64_t time = 0;
    return workshop_install("Workshop.InstallFolder", item_id, folder, &size, &time)
               ? rt_services_internal_owned_text(folder)
               : rt_str_empty();
}

/// @brief Read an installed item's size.
/// @param item_id Item id.
/// @return Bytes, or 0.
int64_t rt_services_workshop_install_size(rt_string item_id) {
    char folder[RT_SERVICES_WORKSHOP_FOLDER_CAPACITY];
    int64_t size = 0;
    int64_t time = 0;
    return workshop_install("Workshop.InstallSize", item_id, folder, &size, &time) ? size : 0;
}

/// @brief Read an installed item's install time.
/// @param item_id Item id.
/// @return Unix seconds, or 0.
int64_t rt_services_workshop_install_time(rt_string item_id) {
    char folder[RT_SERVICES_WORKSHOP_FOLDER_CAPACITY];
    int64_t size = 0;
    int64_t time = 0;
    return workshop_install("Workshop.InstallTime", item_id, folder, &size, &time) ? time : 0;
}

/// @brief Read the bytes of an item's download that have arrived.
/// @param item_id Item id.
/// @return Bytes, or 0.
int64_t rt_services_workshop_downloaded_bytes(rt_string item_id) {
    int64_t downloaded = 0;
    int64_t total = 0;
    workshop_download_info("Workshop.DownloadedBytes", item_id, &downloaded, &total);
    return downloaded;
}

/// @brief Read the size of an item's download.
/// @param item_id Item id.
/// @return Bytes, or 0.
int64_t rt_services_workshop_download_total_bytes(rt_string item_id) {
    int64_t downloaded = 0;
    int64_t total = 0;
    workshop_download_info("Workshop.DownloadTotalBytes", item_id, &downloaded, &total);
    return total;
}

/// @brief Download or update an item.
/// @param item_id Item id.
/// @param high_priority Nonzero for high priority.
/// @return 1 when queued, otherwise 0.
int8_t rt_services_workshop_download(rt_string item_id, int8_t high_priority) {
    static const char member[] = "Workshop.Download";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = workshop_item_id(member, item_id);
    if (!id)
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    return (ops && ops->download && ops->download(id, high_priority ? 1 : 0)) ? 1 : 0;
}

/// @brief Subscribe the player to an item.
/// @param item_id Item id.
/// @return Caller-owned request.
void *rt_services_workshop_subscribe(rt_string item_id) {
    static const char member[] = "Workshop.Subscribe";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *id = workshop_item_id(member, item_id);
    return id ? workshop_request(member, RT_SERVICES_REQUEST_WORKSHOP_SUBSCRIBE, id, "") : NULL;
}

/// @brief Unsubscribe the player from an item.
/// @param item_id Item id.
/// @return Caller-owned request.
void *rt_services_workshop_unsubscribe(rt_string item_id) {
    static const char member[] = "Workshop.Unsubscribe";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *id = workshop_item_id(member, item_id);
    return id ? workshop_request(member, RT_SERVICES_REQUEST_WORKSHOP_UNSUBSCRIBE, id, "") : NULL;
}

//===----------------------------------------------------------------------===//
// Queries
//===----------------------------------------------------------------------===//

/// @brief Query one page of the game's items.
/// @param order WorkshopQuery value.
/// @param page Page from 1.
/// @param required_tags Comma-separated tags.
/// @param search_text Search text.
/// @return Caller-owned request.
void *rt_services_workshop_query(int64_t order,
                                 int64_t page,
                                 rt_string required_tags,
                                 rt_string search_text) {
    static const char member[] = "Workshop.Query";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    if (order < RT_SERVICES_WORKSHOP_QUERY_POPULAR ||
        order > RT_SERVICES_WORKSHOP_QUERY_TEXT_SEARCH) {
        rt_services_internal_trap_argument(
            member, "order must be a WorkshopQuery value (got %lld)", (long long)order);
        return NULL;
    }
    if (!workshop_require_page(member, page))
        return NULL;
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_WORKSHOP_QUERY;
    args.name = "";
    args.text = "";
    args.item_id = "";
    args.items = "";
    args.tags = rt_services_internal_cstr(required_tags);
    args.search = rt_services_internal_cstr(search_text);
    args.query = order;
    args.page = page;
    return rt_services_internal_begin_request(&args, member);
}

/// @brief Query one page of one of the player's lists.
/// @param list WorkshopList value.
/// @param page Page from 1.
/// @return Caller-owned request.
void *rt_services_workshop_query_user(int64_t list, int64_t page) {
    static const char member[] = "Workshop.QueryUser";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    if (list < RT_SERVICES_WORKSHOP_LIST_PUBLISHED || list > RT_SERVICES_WORKSHOP_LIST_PLAYED) {
        rt_services_internal_trap_argument(
            member, "list must be a WorkshopList value (got %lld)", (long long)list);
        return NULL;
    }
    if (!workshop_require_page(member, page))
        return NULL;
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_WORKSHOP_QUERY;
    args.name = "";
    args.text = "";
    args.item_id = "";
    args.items = "";
    args.tags = "";
    args.search = "";
    args.list = list;
    args.page = page;
    return rt_services_internal_begin_request(&args, member);
}

/// @brief Query the details of specific items.
/// @param item_ids Comma-separated ids.
/// @return Caller-owned request, or NULL after a trap.
void *rt_services_workshop_query_items(rt_string item_ids) {
    static const char member[] = "Workshop.QueryItems";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *text = rt_services_internal_require_name(item_ids, member, "item id list");
    if (!text)
        return NULL;
    const size_t length = strlen(text);
    char *normalized = (char *)malloc(length + 1);
    char *entry = (char *)malloc(length + 1);
    if (!normalized || !entry) {
        free(normalized);
        free(entry);
        rt_trap("Services.Workshop.QueryItems: out of memory");
        return NULL;
    }
    normalized[0] = '\0';
    size_t used = 0;
    int64_t count = 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    const char *cursor = text;
    for (;;) {
        const char *comma = strchr(cursor, ',');
        const char *end = comma ? comma : cursor + strlen(cursor);
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        const char *last = end;
        while (last > cursor && (last[-1] == ' ' || last[-1] == '\t'))
            --last;
        const size_t entry_length = (size_t)(last - cursor);
        if (entry_length == 0) {
            free(normalized);
            free(entry);
            rt_services_internal_trap_argument(
                member, "item id %lld in the list is empty", (long long)(count + 1));
            return NULL;
        }
        memcpy(entry, cursor, entry_length);
        entry[entry_length] = '\0';
        if (++count > RT_SERVICES_REQUEST_ITEM_CAPACITY) {
            free(normalized);
            free(entry);
            rt_services_internal_trap_argument(
                member, "the list holds more than %d item ids", RT_SERVICES_REQUEST_ITEM_CAPACITY);
            return NULL;
        }
        if (ops && ops->check_item_id && !ops->check_item_id(member, entry)) {
            free(normalized);
            free(entry);
            return NULL;
        }
        if (used > 0)
            normalized[used++] = ',';
        memcpy(normalized + used, entry, entry_length);
        used += entry_length;
        normalized[used] = '\0';
        if (!comma)
            break;
        cursor = comma + 1;
    }
    free(entry);
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_WORKSHOP_QUERY;
    args.name = "";
    args.text = "";
    args.item_id = "";
    args.items = normalized;
    args.tags = "";
    args.search = "";
    void *request = rt_services_internal_begin_request(&args, member);
    free(normalized);
    return request;
}

//===----------------------------------------------------------------------===//
// Publishing
//===----------------------------------------------------------------------===//

/// @brief Create an empty item.
/// @return Caller-owned request.
void *rt_services_workshop_create_item(void) {
    static const char member[] = "Workshop.CreateItem";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    return workshop_request(member, RT_SERVICES_REQUEST_WORKSHOP_CREATE, "", "");
}

/// @brief Begin an update of an item.
/// @param item_id Item id.
/// @return Caller-owned update id, or the empty string.
rt_string rt_services_workshop_start_update(rt_string item_id) {
    static const char member[] = "Workshop.StartUpdate";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    const char *id = workshop_item_id(member, item_id);
    if (!id)
        return rt_str_empty();
    const rt_services_workshop_ops *ops = workshop_ops();
    char update[RT_SERVICES_WORKSHOP_ID_CAPACITY];
    update[0] = '\0';
    if (!ops || !ops->start_update || !ops->start_update(id, update, sizeof(update)) || !update[0])
        return rt_str_empty();
    return rt_services_internal_owned_text(update);
}

/// @brief Set a pending update's title.
/// @param update_id Update id.
/// @param title Title.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_title(rt_string update_id, rt_string title) {
    return workshop_set_text(
        "Workshop.SetTitle", update_id, RT_SERVICES_WORKSHOP_FIELD_TITLE, title);
}

/// @brief Set a pending update's description.
/// @param update_id Update id.
/// @param description Description.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_description(rt_string update_id, rt_string description) {
    return workshop_set_text(
        "Workshop.SetDescription", update_id, RT_SERVICES_WORKSHOP_FIELD_DESCRIPTION, description);
}

/// @brief Set a pending update's metadata.
/// @param update_id Update id.
/// @param metadata Metadata.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_metadata(rt_string update_id, rt_string metadata) {
    return workshop_set_text(
        "Workshop.SetMetadata", update_id, RT_SERVICES_WORKSHOP_FIELD_METADATA, metadata);
}

/// @brief Replace a pending update's tags.
/// @param update_id Update id.
/// @param tags Comma-separated tags.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_tags(rt_string update_id, rt_string tags) {
    return workshop_set_text("Workshop.SetTags", update_id, RT_SERVICES_WORKSHOP_FIELD_TAGS, tags);
}

/// @brief Set a pending update's visibility.
/// @param update_id Update id.
/// @param visibility WorkshopVisibility value.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_visibility(rt_string update_id, int64_t visibility) {
    static const char member[] = "Workshop.SetVisibility";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    if (visibility < RT_SERVICES_WORKSHOP_VISIBILITY_PUBLIC ||
        visibility > RT_SERVICES_WORKSHOP_VISIBILITY_UNLISTED) {
        rt_services_internal_trap_argument(
            member,
            "visibility must be a WorkshopVisibility value (got %lld)",
            (long long)visibility);
        return 0;
    }
    const char *id = workshop_update_id(member, update_id);
    if (!id)
        return 0;
    const rt_services_workshop_ops *ops = workshop_ops();
    return (ops && ops->set_update_visibility && ops->set_update_visibility(id, visibility)) ? 1
                                                                                             : 0;
}

/// @brief Set the content folder of a pending update.
/// @param update_id Update id.
/// @param folder Folder.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_content(rt_string update_id, rt_string folder) {
    return workshop_set_path(
        "Workshop.SetContent", update_id, RT_SERVICES_WORKSHOP_FIELD_CONTENT, folder);
}

/// @brief Set the preview image of a pending update.
/// @param update_id Update id.
/// @param file Image file.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_preview(rt_string update_id, rt_string file) {
    return workshop_set_path(
        "Workshop.SetPreview", update_id, RT_SERVICES_WORKSHOP_FIELD_PREVIEW, file);
}

/// @brief Submit a pending update.
/// @param update_id Update id.
/// @param change_note Change note.
/// @return Caller-owned request.
void *rt_services_workshop_submit_update(rt_string update_id, rt_string change_note) {
    static const char member[] = "Workshop.SubmitUpdate";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *id = workshop_update_id(member, update_id);
    return id ? workshop_request(member,
                                 RT_SERVICES_REQUEST_WORKSHOP_SUBMIT,
                                 id,
                                 rt_services_internal_cstr(change_note))
              : NULL;
}

/// @brief Read an update's progress through the provider.
/// @param member Class-qualified member name.
/// @param update_id Update id argument.
/// @param status Receives the status.
/// @param processed Receives the bytes processed.
/// @param total Receives the bytes to process.
static void workshop_progress(
    const char *member, rt_string update_id, int64_t *status, int64_t *processed, int64_t *total) {
    *status = RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE;
    *processed = 0;
    *total = 0;
    if (!rt_services_provider_require_main_thread(member))
        return;
    const char *id = workshop_update_id(member, update_id);
    if (!id)
        return;
    const rt_services_workshop_ops *ops = workshop_ops();
    if (!ops || !ops->update_progress || !ops->update_progress(id, status, processed, total)) {
        *status = RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE;
        *processed = 0;
        *total = 0;
    }
}

/// @brief Read an update's status.
/// @param update_id Update id.
/// @return WorkshopUpdateStatus value.
int64_t rt_services_workshop_update_status(rt_string update_id) {
    int64_t status = 0;
    int64_t processed = 0;
    int64_t total = 0;
    workshop_progress("Workshop.UpdateStatus", update_id, &status, &processed, &total);
    return (status >= RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE &&
            status <= RT_SERVICES_WORKSHOP_UPDATE_STATUS_COMMITTING)
               ? status
               : RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE;
}

/// @brief Read an update's progress.
/// @param update_id Update id.
/// @return Fraction in 0..1.
double rt_services_workshop_update_progress(rt_string update_id) {
    int64_t status = 0;
    int64_t processed = 0;
    int64_t total = 0;
    workshop_progress("Workshop.UpdateProgress", update_id, &status, &processed, &total);
    if (total <= 0 || processed <= 0)
        return 0.0;
    return processed >= total ? 1.0 : (double)processed / (double)total;
}

/// @brief Delete an item.
/// @param item_id Item id.
/// @return Caller-owned request.
void *rt_services_workshop_delete_item(rt_string item_id) {
    static const char member[] = "Workshop.DeleteItem";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *id = workshop_item_id(member, item_id);
    return id ? workshop_request(member, RT_SERVICES_REQUEST_WORKSHOP_DELETE, id, "") : NULL;
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return WorkshopQuery.Popular. @return 1.
int64_t rt_services_workshop_query_popular(void) {
    return RT_SERVICES_WORKSHOP_QUERY_POPULAR;
}

/// @brief Return WorkshopQuery.Newest. @return 2.
int64_t rt_services_workshop_query_newest(void) {
    return RT_SERVICES_WORKSHOP_QUERY_NEWEST;
}

/// @brief Return WorkshopQuery.Trending. @return 3.
int64_t rt_services_workshop_query_trending(void) {
    return RT_SERVICES_WORKSHOP_QUERY_TRENDING;
}

/// @brief Return WorkshopQuery.MostSubscribed. @return 4.
int64_t rt_services_workshop_query_most_subscribed(void) {
    return RT_SERVICES_WORKSHOP_QUERY_MOST_SUBSCRIBED;
}

/// @brief Return WorkshopQuery.RecentlyUpdated. @return 5.
int64_t rt_services_workshop_query_recently_updated(void) {
    return RT_SERVICES_WORKSHOP_QUERY_RECENTLY_UPDATED;
}

/// @brief Return WorkshopQuery.TextSearch. @return 6.
int64_t rt_services_workshop_query_text_search(void) {
    return RT_SERVICES_WORKSHOP_QUERY_TEXT_SEARCH;
}

/// @brief Return WorkshopList.Published. @return 1.
int64_t rt_services_workshop_list_published(void) {
    return RT_SERVICES_WORKSHOP_LIST_PUBLISHED;
}

/// @brief Return WorkshopList.Subscribed. @return 2.
int64_t rt_services_workshop_list_subscribed(void) {
    return RT_SERVICES_WORKSHOP_LIST_SUBSCRIBED;
}

/// @brief Return WorkshopList.Favorited. @return 3.
int64_t rt_services_workshop_list_favorited(void) {
    return RT_SERVICES_WORKSHOP_LIST_FAVORITED;
}

/// @brief Return WorkshopList.VotedUp. @return 4.
int64_t rt_services_workshop_list_voted_up(void) {
    return RT_SERVICES_WORKSHOP_LIST_VOTED_UP;
}

/// @brief Return WorkshopList.Played. @return 5.
int64_t rt_services_workshop_list_played(void) {
    return RT_SERVICES_WORKSHOP_LIST_PLAYED;
}

/// @brief Return WorkshopVisibility.Public. @return 0.
int64_t rt_services_workshop_visibility_public(void) {
    return RT_SERVICES_WORKSHOP_VISIBILITY_PUBLIC;
}

/// @brief Return WorkshopVisibility.FriendsOnly. @return 1.
int64_t rt_services_workshop_visibility_friends_only(void) {
    return RT_SERVICES_WORKSHOP_VISIBILITY_FRIENDS_ONLY;
}

/// @brief Return WorkshopVisibility.Private. @return 2.
int64_t rt_services_workshop_visibility_private(void) {
    return RT_SERVICES_WORKSHOP_VISIBILITY_PRIVATE;
}

/// @brief Return WorkshopVisibility.Unlisted. @return 3.
int64_t rt_services_workshop_visibility_unlisted(void) {
    return RT_SERVICES_WORKSHOP_VISIBILITY_UNLISTED;
}

/// @brief Return WorkshopUpdateStatus.None. @return 0.
int64_t rt_services_workshop_update_status_none(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE;
}

/// @brief Return WorkshopUpdateStatus.PreparingConfig. @return 1.
int64_t rt_services_workshop_update_status_preparing_config(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_PREPARING_CONFIG;
}

/// @brief Return WorkshopUpdateStatus.PreparingContent. @return 2.
int64_t rt_services_workshop_update_status_preparing_content(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_PREPARING_CONTENT;
}

/// @brief Return WorkshopUpdateStatus.UploadingContent. @return 3.
int64_t rt_services_workshop_update_status_uploading_content(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_UPLOADING_CONTENT;
}

/// @brief Return WorkshopUpdateStatus.UploadingPreview. @return 4.
int64_t rt_services_workshop_update_status_uploading_preview(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_UPLOADING_PREVIEW;
}

/// @brief Return WorkshopUpdateStatus.Committing. @return 5.
int64_t rt_services_workshop_update_status_committing(void) {
    return RT_SERVICES_WORKSHOP_UPDATE_STATUS_COMMITTING;
}
