//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_workshop.h
// Purpose: Public C ABI for Zanna.Services.Workshop, the provider-neutral
//          user-generated content service of a distribution platform (Steam
//          Workshop first): subscribed items with their install and download
//          state, queries returning Zanna.Services.WorkshopItem objects,
//          subscriptions, and publishing items, plus the WorkshopItem class
//          and the WorkshopQuery, WorkshopList, WorkshopVisibility, and
//          WorkshopUpdateStatus constants.
// Key invariants:
//   - Workshop members must run on the main thread; WorkshopItem getters and
//     constant getters may run on any thread.
//   - Empty item and update ids, unknown constants, pages below 1, and more
//     than 100 ids in a query trap whether or not a provider is started; a
//     provider traps on ids it cannot parse while it is active.
//   - Without a provider that supports the Workshop, members return false, 0,
//     or "" and requests complete as failed.
//   - Constant ordinals are stable public values documented in ADR 0366.
// Ownership/Lifetime:
//   - Returned strings, requests, and items are caller-owned; a WorkshopItem
//     copies its data and outlives the request it came from.
// Links: src/runtime/services/rt_services_workshop.c,
//        src/runtime/services/rt_services_provider.h,
//        docs/zannalib/services.md,
//        docs/adr/0366-platform-services-workshop.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_workshop.h
 * @brief Declares Zanna.Services.Workshop, WorkshopItem, and their constant classes.
 * @details Players share content they make for a game (leagues, parks,
 *          uniforms) through the platform's Workshop. A game lists and
 *          installs the items the player subscribed to, lets the player browse
 *          and subscribe from inside the game, and publishes what the player
 *          creates.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Stable constants
//===----------------------------------------------------------------------===//

/// @brief Items ranked by votes.
#define RT_SERVICES_WORKSHOP_QUERY_POPULAR INT64_C(1)
/// @brief Items ranked by publication date, newest first.
#define RT_SERVICES_WORKSHOP_QUERY_NEWEST INT64_C(2)
/// @brief Items ranked by recent votes.
#define RT_SERVICES_WORKSHOP_QUERY_TRENDING INT64_C(3)
/// @brief Items ranked by unique subscriptions.
#define RT_SERVICES_WORKSHOP_QUERY_MOST_SUBSCRIBED INT64_C(4)
/// @brief Items ranked by their last update, newest first.
#define RT_SERVICES_WORKSHOP_QUERY_RECENTLY_UPDATED INT64_C(5)
/// @brief Items ranked by how well they match the search text.
#define RT_SERVICES_WORKSHOP_QUERY_TEXT_SEARCH INT64_C(6)

/// @brief Items the player published.
#define RT_SERVICES_WORKSHOP_LIST_PUBLISHED INT64_C(1)
/// @brief Items the player subscribed to.
#define RT_SERVICES_WORKSHOP_LIST_SUBSCRIBED INT64_C(2)
/// @brief Items the player marked as favorites.
#define RT_SERVICES_WORKSHOP_LIST_FAVORITED INT64_C(3)
/// @brief Items the player voted up.
#define RT_SERVICES_WORKSHOP_LIST_VOTED_UP INT64_C(4)
/// @brief Items the player used or played.
#define RT_SERVICES_WORKSHOP_LIST_PLAYED INT64_C(5)

/// @brief Visible to everyone.
#define RT_SERVICES_WORKSHOP_VISIBILITY_PUBLIC INT64_C(0)
/// @brief Visible to the author's friends.
#define RT_SERVICES_WORKSHOP_VISIBILITY_FRIENDS_ONLY INT64_C(1)
/// @brief Visible to the author only.
#define RT_SERVICES_WORKSHOP_VISIBILITY_PRIVATE INT64_C(2)
/// @brief Visible to anyone with the link, but not listed.
#define RT_SERVICES_WORKSHOP_VISIBILITY_UNLISTED INT64_C(3)

/// @brief No update is in progress for the id (not submitted, or finished).
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE INT64_C(0)
/// @brief The platform is processing the item's settings.
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_PREPARING_CONFIG INT64_C(1)
/// @brief The platform is reading the content files.
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_PREPARING_CONTENT INT64_C(2)
/// @brief The platform is uploading the content.
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_UPLOADING_CONTENT INT64_C(3)
/// @brief The platform is uploading the preview image.
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_UPLOADING_PREVIEW INT64_C(4)
/// @brief The platform is committing the changes.
#define RT_SERVICES_WORKSHOP_UPDATE_STATUS_COMMITTING INT64_C(5)

/// @brief Capacity, in bytes including the terminator, of an item or update id.
#define RT_SERVICES_WORKSHOP_ID_CAPACITY 32
/// @brief Capacity, in bytes including the terminator, of an install folder path.
#define RT_SERVICES_WORKSHOP_FOLDER_CAPACITY 4096

//===----------------------------------------------------------------------===//
// Zanna.Services.Workshop: subscribed items
//===----------------------------------------------------------------------===//

/// @brief Count the items the player subscribed to.
/// @return Count, or 0.
int64_t rt_services_workshop_get_subscribed_count(void);

/// @brief Read the id of a subscribed item.
/// @param index Index in 0..SubscribedCount-1.
/// @return Caller-owned item id, or "" outside the range.
rt_string rt_services_workshop_subscribed_id_at(int64_t index);

/// @brief Report whether the player is subscribed to an item.
/// @param item_id Non-empty item id.
/// @return 1 when subscribed, otherwise 0.
int8_t rt_services_workshop_is_subscribed(rt_string item_id);

/// @brief Report whether an item is installed (possibly out of date).
/// @param item_id Non-empty item id.
/// @return 1 when installed, otherwise 0.
int8_t rt_services_workshop_is_installed(rt_string item_id);

/// @brief Report whether an item needs a download or an update.
/// @param item_id Non-empty item id.
/// @return 1 when it does, otherwise 0.
int8_t rt_services_workshop_needs_update(rt_string item_id);

/// @brief Report whether an item is downloading or waiting to download.
/// @param item_id Non-empty item id.
/// @return 1 when it is, otherwise 0.
int8_t rt_services_workshop_is_downloading(rt_string item_id);

/// @brief Read the folder holding an installed item's files.
/// @param item_id Non-empty item id.
/// @return Caller-owned absolute folder, or "" when not installed.
rt_string rt_services_workshop_install_folder(rt_string item_id);

/// @brief Read an installed item's size on disk.
/// @param item_id Non-empty item id.
/// @return Bytes, or 0 when not installed.
int64_t rt_services_workshop_install_size(rt_string item_id);

/// @brief Read when an installed item was installed or last updated.
/// @param item_id Non-empty item id.
/// @return Unix seconds, or 0 when not installed.
int64_t rt_services_workshop_install_time(rt_string item_id);

/// @brief Read how many bytes of an item's download have arrived.
/// @param item_id Non-empty item id.
/// @return Bytes, or 0.
int64_t rt_services_workshop_downloaded_bytes(rt_string item_id);

/// @brief Read the size of an item's download.
/// @param item_id Non-empty item id.
/// @return Bytes, or 0.
int64_t rt_services_workshop_download_total_bytes(rt_string item_id);

/// @brief Download or update an item; WorkshopItemDownloaded reports the result.
/// @param item_id Non-empty item id.
/// @param high_priority Nonzero to download it before other items.
/// @return 1 when the download was queued, otherwise 0.
int8_t rt_services_workshop_download(rt_string item_id, int8_t high_priority);

/// @brief Subscribe the player to an item.
/// @param item_id Non-empty item id.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopSubscribe (Text holds the id).
void *rt_services_workshop_subscribe(rt_string item_id);

/// @brief Unsubscribe the player from an item.
/// @param item_id Non-empty item id.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopUnsubscribe (Text holds the id).
void *rt_services_workshop_unsubscribe(rt_string item_id);

//===----------------------------------------------------------------------===//
// Zanna.Services.Workshop: queries
//===----------------------------------------------------------------------===//

/// @brief Query one page of the game's items.
/// @details The request completes with ItemCount items, Value holding the
///          total number of matching items, and Flag set when the platform
///          answered from its local cache.
/// @param order WorkshopQuery value; other values trap.
/// @param page Page from 1; smaller values trap.
/// @param required_tags Comma-separated tags every item must carry, or "".
/// @param search_text Text items must match in the title or description, or "".
/// @return Caller-owned Zanna.Services.Request of kind WorkshopQuery.
void *rt_services_workshop_query(int64_t order,
                                 int64_t page,
                                 rt_string required_tags,
                                 rt_string search_text);

/// @brief Query one page of one of the player's lists.
/// @param list WorkshopList value; other values trap.
/// @param page Page from 1; smaller values trap.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopQuery.
void *rt_services_workshop_query_user(int64_t list, int64_t page);

/// @brief Query the details of specific items.
/// @param item_ids Comma-separated item ids (spaces around ids are ignored); an
///        empty list, an empty entry, or more than 100 ids trap.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopQuery.
void *rt_services_workshop_query_items(rt_string item_ids);

//===----------------------------------------------------------------------===//
// Zanna.Services.Workshop: publishing
//===----------------------------------------------------------------------===//

/// @brief Create an empty item owned by the player.
/// @details The request completes with Text holding the new item id and Flag
///          set when the player must accept the platform's Workshop agreement
///          before the item becomes visible.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopCreate.
void *rt_services_workshop_create_item(void);

/// @brief Begin an update of an item the player owns.
/// @param item_id Non-empty item id.
/// @return Caller-owned update id, or "" when the update could not start.
rt_string rt_services_workshop_start_update(rt_string item_id);

/// @brief Set a pending update's title.
/// @param update_id Non-empty update id from StartUpdate.
/// @param title New title.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_title(rt_string update_id, rt_string title);

/// @brief Set a pending update's description.
/// @param update_id Non-empty update id from StartUpdate.
/// @param description New description.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_description(rt_string update_id, rt_string description);

/// @brief Set a pending update's developer metadata.
/// @param update_id Non-empty update id from StartUpdate.
/// @param metadata Game-defined text stored with the item.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_metadata(rt_string update_id, rt_string metadata);

/// @brief Replace a pending update's tags.
/// @param update_id Non-empty update id from StartUpdate.
/// @param tags Comma-separated tags; "" removes every tag.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_tags(rt_string update_id, rt_string tags);

/// @brief Set a pending update's visibility.
/// @param update_id Non-empty update id from StartUpdate.
/// @param visibility WorkshopVisibility value; other values trap.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_visibility(rt_string update_id, int64_t visibility);

/// @brief Set the folder whose files become the item's content.
/// @param update_id Non-empty update id from StartUpdate.
/// @param folder Existing folder (relative paths resolve against the working directory).
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_content(rt_string update_id, rt_string folder);

/// @brief Set the item's preview image.
/// @param update_id Non-empty update id from StartUpdate.
/// @param file Existing image file (relative paths resolve against the working directory).
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_workshop_set_preview(rt_string update_id, rt_string file);

/// @brief Submit a pending update.
/// @details The request completes with Text holding the item id and Flag set
///          when the player must accept the Workshop agreement.
/// @param update_id Non-empty update id from StartUpdate.
/// @param change_note Note shown in the item's change history, or "".
/// @return Caller-owned Zanna.Services.Request of kind WorkshopSubmit.
void *rt_services_workshop_submit_update(rt_string update_id, rt_string change_note);

/// @brief Read the stage of a submitted update.
/// @param update_id Non-empty update id.
/// @return WorkshopUpdateStatus value.
int64_t rt_services_workshop_update_status(rt_string update_id);

/// @brief Read the progress of the current stage of a submitted update.
/// @param update_id Non-empty update id.
/// @return Fraction in 0..1, or 0 when unknown.
double rt_services_workshop_update_progress(rt_string update_id);

/// @brief Delete an item the player owns.
/// @param item_id Non-empty item id.
/// @return Caller-owned Zanna.Services.Request of kind WorkshopDelete (Text holds the id).
void *rt_services_workshop_delete_item(rt_string item_id);

//===----------------------------------------------------------------------===//
// Zanna.Services.WorkshopItem
//===----------------------------------------------------------------------===//

/// @brief Read an item's id. @param item Borrowed WorkshopItem. @return Caller-owned id.
rt_string rt_services_workshop_item_get_id(void *item);
/// @brief Read an item's title. @param item Borrowed WorkshopItem. @return Caller-owned title.
rt_string rt_services_workshop_item_get_title(void *item);
/// @brief Read an item's description. @param item Borrowed WorkshopItem. @return Caller-owned text.
rt_string rt_services_workshop_item_get_description(void *item);
/// @brief Read the author's user id. @param item Borrowed WorkshopItem. @return Caller-owned id.
rt_string rt_services_workshop_item_get_owner_id(void *item);
/// @brief Read an item's comma-separated tags. @param item Borrowed WorkshopItem. @return Tags.
rt_string rt_services_workshop_item_get_tags(void *item);
/// @brief Read the preview image URL. @param item Borrowed WorkshopItem. @return URL, or "".
rt_string rt_services_workshop_item_get_preview_url(void *item);
/// @brief Read developer metadata. @param item Borrowed WorkshopItem. @return Metadata, or "".
rt_string rt_services_workshop_item_get_metadata(void *item);
/// @brief Read the creation time. @param item Borrowed WorkshopItem. @return Unix seconds.
int64_t rt_services_workshop_item_get_created(void *item);
/// @brief Read the last update time. @param item Borrowed WorkshopItem. @return Unix seconds.
int64_t rt_services_workshop_item_get_updated(void *item);
/// @brief Read the visibility. @param item Borrowed WorkshopItem. @return WorkshopVisibility.
int64_t rt_services_workshop_item_get_visibility(void *item);
/// @brief Read the up votes. @param item Borrowed WorkshopItem. @return Count.
int64_t rt_services_workshop_item_get_votes_up(void *item);
/// @brief Read the down votes. @param item Borrowed WorkshopItem. @return Count.
int64_t rt_services_workshop_item_get_votes_down(void *item);
/// @brief Read the content size. @param item Borrowed WorkshopItem. @return Bytes.
int64_t rt_services_workshop_item_get_size(void *item);
/// @brief Read the vote score. @param item Borrowed WorkshopItem. @return Score in 0..1.
double rt_services_workshop_item_get_score(void *item);

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.WorkshopQuery.Popular`. @return Stable ordinal 1.
int64_t rt_services_workshop_query_popular(void);
/// @brief Return `Zanna.Services.WorkshopQuery.Newest`. @return Stable ordinal 2.
int64_t rt_services_workshop_query_newest(void);
/// @brief Return `Zanna.Services.WorkshopQuery.Trending`. @return Stable ordinal 3.
int64_t rt_services_workshop_query_trending(void);
/// @brief Return `Zanna.Services.WorkshopQuery.MostSubscribed`. @return Stable ordinal 4.
int64_t rt_services_workshop_query_most_subscribed(void);
/// @brief Return `Zanna.Services.WorkshopQuery.RecentlyUpdated`. @return Stable ordinal 5.
int64_t rt_services_workshop_query_recently_updated(void);
/// @brief Return `Zanna.Services.WorkshopQuery.TextSearch`. @return Stable ordinal 6.
int64_t rt_services_workshop_query_text_search(void);

/// @brief Return `Zanna.Services.WorkshopList.Published`. @return Stable ordinal 1.
int64_t rt_services_workshop_list_published(void);
/// @brief Return `Zanna.Services.WorkshopList.Subscribed`. @return Stable ordinal 2.
int64_t rt_services_workshop_list_subscribed(void);
/// @brief Return `Zanna.Services.WorkshopList.Favorited`. @return Stable ordinal 3.
int64_t rt_services_workshop_list_favorited(void);
/// @brief Return `Zanna.Services.WorkshopList.VotedUp`. @return Stable ordinal 4.
int64_t rt_services_workshop_list_voted_up(void);
/// @brief Return `Zanna.Services.WorkshopList.Played`. @return Stable ordinal 5.
int64_t rt_services_workshop_list_played(void);

/// @brief Return `Zanna.Services.WorkshopVisibility.Public`. @return Stable ordinal 0.
int64_t rt_services_workshop_visibility_public(void);
/// @brief Return `Zanna.Services.WorkshopVisibility.FriendsOnly`. @return Stable ordinal 1.
int64_t rt_services_workshop_visibility_friends_only(void);
/// @brief Return `Zanna.Services.WorkshopVisibility.Private`. @return Stable ordinal 2.
int64_t rt_services_workshop_visibility_private(void);
/// @brief Return `Zanna.Services.WorkshopVisibility.Unlisted`. @return Stable ordinal 3.
int64_t rt_services_workshop_visibility_unlisted(void);

/// @brief Return `Zanna.Services.WorkshopUpdateStatus.None`. @return Stable ordinal 0.
int64_t rt_services_workshop_update_status_none(void);
/// @brief Return `Zanna.Services.WorkshopUpdateStatus.PreparingConfig`. @return Stable ordinal 1.
int64_t rt_services_workshop_update_status_preparing_config(void);
/// @brief Return `Zanna.Services.WorkshopUpdateStatus.PreparingContent`. @return Stable ordinal 2.
int64_t rt_services_workshop_update_status_preparing_content(void);
/// @brief Return `Zanna.Services.WorkshopUpdateStatus.UploadingContent`. @return Stable ordinal 3.
int64_t rt_services_workshop_update_status_uploading_content(void);
/// @brief Return `Zanna.Services.WorkshopUpdateStatus.UploadingPreview`. @return Stable ordinal 4.
int64_t rt_services_workshop_update_status_uploading_preview(void);
/// @brief Return `Zanna.Services.WorkshopUpdateStatus.Committing`. @return Stable ordinal 5.
int64_t rt_services_workshop_update_status_committing(void);

#ifdef __cplusplus
}
#endif
