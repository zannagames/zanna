---
status: accepted
audience: contributors
last-verified: 2026-09-15
---

# ADR 0366: Platform Services Workshop (Steam Workshop First)

## Status

Accepted. Adds `Zanna.Services.Workshop` and `Zanna.Services.WorkshopItem` to the platform
services layer of [ADR 0352](0352-platform-services-runtime-loaded-providers.md). Requests, events,
and neutral values follow the rules there.

## Context

Games that let players make content ship it through the store's Workshop. For Legacy Baseball
that content is custom leagues, parks, and uniforms.

- **In the game.** The game must find the items the player subscribed to and load their files,
  let the player browse and subscribe, and publish what the player builds.
- **On Steam.** `ISteamUGC` provides all of this through query handles, update handles, call
  results, and callbacks.

The flat interface is `SteamUGC_v020` in SDK 1.61 and `SteamUGC_v021` from SDK 1.62. All the
methods this ADR binds keep their signatures across both versions except two:
`GetNumSubscribedItems` and `GetSubscribedItems` gained a trailing `bool bIncludeLocallyDisabled`.
Both redistributables export those functions under the same names, so the binding must pick the
declaration that matches the interface it opened. Calling the v021 export through the v020
declaration passes an undefined `bool`.

## Decision

### Neutral surface

`Zanna.Services.Workshop` is a static class. Members are main-thread only and neutral without a
provider. Ids and tag lists are strings, and lists are comma-separated.

| Member | Signature | Contract |
|---|---|---|
| `SubscribedCount` | `i64` | Items the player subscribed to |
| `SubscribedIdAt(index)` | `str(i64)`, owned | Subscribed item id, or `""` |
| `IsSubscribed` / `IsInstalled` / `NeedsUpdate` / `IsDownloading(itemId)` | `i1(str)` | Item state |
| `InstallFolder(itemId)` | `str(str)`, owned | Folder of an installed item, or `""` |
| `InstallSize` / `InstallTime(itemId)` | `i64(str)` | Bytes on disk and install time (Unix seconds) |
| `DownloadedBytes` / `DownloadTotalBytes(itemId)` | `i64(str)` | Download progress |
| `Download(itemId, highPriority)` | `i1(str,i1)` | Download or update an item |
| `Subscribe` / `Unsubscribe(itemId)` | `obj<Request>(str)`, owned | Change a subscription |
| `Query(order, page, requiredTags, searchText)` | `obj<Request>(i64,i64,str,str)`, owned | One page of the app's items |
| `QueryUser(list, page)` | `obj<Request>(i64,i64)`, owned | One page of one of the player's lists |
| `QueryItems(itemIds)` | `obj<Request>(str)`, owned | Details of specific items |
| `CreateItem()` | `obj<Request>()`, owned | New empty item owned by the player |
| `StartUpdate(itemId)` | `str(str)`, owned | Update id, or `""` |
| `SetTitle` / `SetDescription` / `SetMetadata` / `SetTags(updateId, text)` | `i1(str,str)` | Update fields |
| `SetVisibility(updateId, visibility)` | `i1(str,i64)` | `WorkshopVisibility` |
| `SetContent(updateId, folder)` / `SetPreview(updateId, file)` | `i1(str,str)` | Upload paths; relative paths resolve against the working directory and must exist |
| `SubmitUpdate(updateId, changeNote)` | `obj<Request>(str,str)`, owned | Send the update |
| `UpdateStatus(updateId)` | `i64(str)` | `WorkshopUpdateStatus` |
| `UpdateProgress(updateId)` | `f64(str)` | Current stage progress in 0..1 |
| `DeleteItem(itemId)` | `obj<Request>(str)`, owned | Delete the player's item |

**Query results.** A query request completes with `WorkshopItem` objects:

- `Request` gains `ItemCount` (`i64`) and `ItemAt(index)` (`obj<Zanna.Services.WorkshopItem>`,
  owned), and holds at most 100 items. An index outside `0..ItemCount-1` traps: `index <i> is
  outside 0..<n>`, or `the request holds no items`.
- A `WorkshopItem` copies its data when the request completes and outlives the request. Its
  getters read no provider state, so they are not main-thread bound.
- Items are `obj` instances with class id `-0x5E0102`. Their read-only properties are `Id`,
  `Title`, `Description`, `OwnerId`, `Tags`, `PreviewUrl`, and `Metadata` (`str`); `Created`,
  `Updated`, `Visibility`, `VotesUp`, `VotesDown`, and `Size` (`i64`); and `Score` (`f64`).

**Request kinds and results.**

| Kind | Value | `Value` | `Flag` | `Text` |
|---|---|---|---|---|
| `WorkshopQuery` | 9 | Matching items on every page | Answered from the local cache | — |
| `WorkshopSubscribe` | 10 | — | — | Item id |
| `WorkshopUnsubscribe` | 11 | — | — | Item id |
| `WorkshopCreate` | 12 | — | Player must accept the Workshop agreement | New item id |
| `WorkshopSubmit` | 13 | — | Player must accept the Workshop agreement | Item id |
| `WorkshopDelete` | 14 | — | — | Item id |

**Events.** While a provider with the Workshop runs, the event queue reports:

- `WorkshopItemInstalled` (15): `EventText` is the item id.
- `WorkshopItemDownloaded` (16): `EventText` is the item id, `EventFlag` is success, and
  `EventResultCode` is the provider result.
- `WorkshopSubscriptionChanged` (17): `EventText` is the item id, and `EventFlag` is true for a
  subscription.

**Constants.**

| Class | Values |
|---|---|
| `Feature` | `Workshop` is 18 |
| `WorkshopQuery` | `Popular` 1, `Newest` 2, `Trending` 3, `MostSubscribed` 4, `RecentlyUpdated` 5, `TextSearch` 6 |
| `WorkshopList` | `Published` 1, `Subscribed` 2, `Favorited` 3, `VotedUp` 4, `Played` 5 |
| `WorkshopVisibility` | `Public` 0, `FriendsOnly` 1, `Private` 2, `Unlisted` 3 |
| `WorkshopUpdateStatus` | `None` 0, `PreparingConfig` 1, `PreparingContent` 2, `UploadingContent` 3, `UploadingPreview` 4, `Committing` 5 |

**Traps and diagnostics.**

- These trap whether or not a provider is started, as `Services.Workshop.<Member>: <detail>`:
  - empty item ids, update ids, and id lists;
  - an empty entry in an id list;
  - more than 100 ids;
  - unknown `WorkshopQuery`, `WorkshopList`, or `WorkshopVisibility` values;
  - pages below 1;
  - empty content or preview paths.
- A missing content folder or preview file records `Services: Workshop.<Member> found no
  folder|file at '<path>'` and returns false.

### Provider contract

`rt_services_workshop_ops` holds these operations:

- `check_item_id` and `check_update_id`, which trap on malformed ids.
- `subscribed_count` and `subscribed_id_at`.
- `item_state`, returning flags with Steam's `EItemState` bit values: subscribed 1, installed 4,
  needs update 8, downloading 16, download pending 32.
- `install_info`, `download_info`, and `download`.
- `start_update`, `set_update_text` (the title, description, metadata, tags, content, and
  preview fields), `set_update_visibility`, and `update_progress`.

Requests go through `begin_request`, whose arguments gain `item_id`, `items`, `tags`, `search`,
`query`, `list`, and `page`. `rt_services_request_result` gains `items` and `item_count`: borrowed
`rt_services_workshop_item` records that the core copies into `WorkshopItem` objects. The provider
table gains `.workshop`.

### Steam binding

- **Binding.** The accessor is `SteamUGC_v021`, falling back to `v020`. The group needs the
  accessor, 32 exports, and `ISteamUtils::GetAppID`. The subscribed-item functions are bound through the declaration of
  the opened version, and v021 calls pass `bIncludeLocallyDisabled = false`. A missing piece
  records `Steam: interface ... unavailable; Workshop disabled`.
- **Queries.**
  - `Query` uses `CreateQueryAllUGCRequestPage`. `Popular`, `Newest`, `Trending`,
    `MostSubscribed`, `RecentlyUpdated`, and `TextSearch` map to `EUGCQuery` 0, 1, 3, 12, 19, and
    11. The matching type is `k_EUGCMatchingUGCType_Items_ReadyToUse`, and creator and consumer
    are the running app.
  - `QueryUser` uses `CreateQueryUserUGCRequest` with the account id (the low 32 bits of the
    SteamID). `Published`, `Subscribed`, `Favorited`, `VotedUp`, and `Played` map to
    `EUserUGCList` 0, 6, 5, 2, and 7. Subscribed lists sort by subscription date and the others by
    creation date.
  - `QueryItems` uses `CreateQueryUGCDetailsRequest`, and more than 50 ids fail the request with
    `Steam: Workshop.QueryItems takes at most 50 item ids (got N)`.
  - Every query adds the required tags, sets the search text, and asks for long descriptions and
    metadata before `SendQueryUGCRequest`.
- **Reading and releasing results.** On `SteamUGCQueryCompleted_t` the binding reads each result
  with `GetQueryUGCResult`, `GetQueryUGCPreviewURL`, and `GetQueryUGCMetadata` into heap copies,
  completes the request, and frees the copies. `Size` is `m_ulTotalFilesSize`, or the legacy
  `m_nFileSize` when that is 0. The query handle is released on every path: a failed send,
  completion, and provider stop.
- **Other requests.**
  - `SubscribeItem`, `UnsubscribeItem`, and `DeleteItem` complete from
    `RemoteStorageSubscribePublishedFileResult_t` (1313),
    `RemoteStorageUnsubscribePublishedFileResult_t` (1315), and `DeleteItemResult_t` (3417).
  - `CreateItem` uses `k_EWorkshopFileTypeCommunity` and completes from `CreateItemResult_t`
    (3403). `SubmitItemUpdate` completes from `SubmitItemUpdateResult_t` (3404).
  - A result other than `k_EResultOK` fails the request with the EResult, for example `Steam:
    SubscribeItem('<id>') failed (EResult 9)`.
- **Updates.** `SetTags` splits the list, trims spaces, and passes a `SteamParamStringArray_t`
  without admin tags. A setter Steam refuses records the method, the update id, and the text length.
  `StartItemUpdate` returning `k_UGCUpdateHandleInvalid` records `Steam: StartItemUpdate('<id>')
  failed`.
- **Callbacks.** `ItemInstalled_t` (3405), `DownloadItemResult_t` (3406),
  `RemoteStoragePublishedFileSubscribed_t` (1321), and `RemoteStoragePublishedFileUnsubscribed_t`
  (1322) become the three events. Reports for other apps are ignored.
- **Ids.** Item ids are decimal `PublishedFileId_t` values in 1..2^64−1, and update ids are
  decimal `UGCUpdateHandle_t` values in 0..2^64−2. Anything else traps with, for example,
  `Services.Workshop.IsInstalled: Steam Workshop item id 'abc' must be an integer in
  1..18446744073709551615`.
- **Layouts.** Static assertions pin every structure:
  - `SteamUGCDetails_t`: 9772 bytes under pack 4 and 9784 under pack 8; the owner is at 8156 or
    8160 and the total size at 9764 or 9776.
  - `SteamUGCQueryCompleted_t`: 280 bytes under both packings.
  - `CreateItemResult_t`: 16 or 24 bytes.
  - `SubmitItemUpdateResult_t`: 16 bytes.
  - `ItemInstalled_t`: 28 or 32 bytes.
  - `DownloadItemResult_t`: 16 or 24 bytes.
  - The file-result and file-subscription structures: 12 or 16 bytes.
  - `SteamParamStringArray_t`: 12 or 16 bytes.

  The pack-4 values match both the SDK 1.61 and SDK 1.65 headers.

## Consequences

- A game can load, browse, subscribe to, and publish Workshop content without naming Steam, and
  the same code is neutral in other builds.
- Voting, favorites, dependencies, playtime tracking, key-value tags, additional previews, content
  descriptors, EULA status, and cursor paging are left out. They can be appended later.
- Tests use a new `test_rt_services_workshop` binary against the fake redistributable. The fake
  models:
  - three items, one installed, one waiting for a download, and one owned by the player;
  - all three query kinds with tag and text filters and paging, and handle release;
  - subscriptions with their events, and downloads with the installed and downloaded events;
  - creation, updates with every field, and deletion;
  - the v020 signatures in the SDK 1.61 profile;
  - the core-only profile.

  The Zia and BASIC fixtures cover the class on the VM and in native binaries.
- **Live check (2026-09-15, macOS arm64, Spacewar 480, SDK 1.61 and the 1.65-generation library):**
  - The popular and newest pages returned 50 of 4,317 items each, with titles, owners, votes,
    scores, sizes up to 321 MB, dates, metadata, and preview URLs.
  - Repeated queries set `Flag` from Steam's cache.
  - Item-detail and published-list queries completed.
  - Subscribing, downloading, and publishing were not exercised, because they would change the
    account, download other players' content, or publish new content.

## Alternatives Considered

- **Item details on `Request` (`ItemTitle(i)` and similar).** Rejected. That adds 14 kind-specific
  members to every request. An item object groups them and survives the request.
- **A global details cache keyed by item id.** Rejected. It would need an eviction policy and could
  show stale data. Objects owned by the game have a clear lifetime.
- **Cursor paging.** Rejected for now. Page numbers match how games present Workshop browsing, and
  cursor queries can be added as another query form.
- **One call that sets every update field.** Rejected. An update id with setters mirrors how
  platforms stage updates and leaves unchanged fields untouched.
