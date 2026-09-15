---
status: accepted
audience: contributors
last-verified: 2026-09-14
---

# ADR 0353: Platform Services Player Features (Achievements, Stats, Leaderboards, Presence, Overlay, Text Input, Cloud)

## Status

Accepted. Implements phase 2 of the platform services program started by
[ADR 0352](0352-platform-services-runtime-loaded-providers.md), whose policy, provider model,
library resolution, and lifecycle rules still apply unchanged except where this record extends
them. Phase 3, packaging, is [ADR 0354](0354-store-depot-packaging.md).

## Context

ADR 0352 gave games identity, licensing, DLC, platform events, and a non-blocking request object,
with Steam as the first provider behind the neutral `Zanna.Services` surface. A store release also
needs the player-facing services: achievements with unlock notifications, stats, leaderboards,
rich presence, overlay control, an on-screen keyboard for controller-only devices (Steam Deck
compatibility review requires one for text entry), and per-user cloud files.

The owner requirement that the layer serve other stores later rules out mirroring Steam's
interfaces. Every major store has achievements, stats, leaderboards, presence, an overlay, and
cloud saves, but they disagree on details: Steam addresses leaderboards by handle, other stores by
name; Steam batches stat commits, others commit each change; some keyboards type into the game's
field, others return text. Each of these needs a neutral shape that a second provider can
implement without a new surface.

The Steamworks flat functions used here were checked against the SDK 1.61, 1.62, 1.63, 1.64, and
1.65 declarations: every signature is identical across the supported range, and
`SteamAPI_SteamRemoteStorage_v016` is the remote storage accessor in all five.

## Decision

### Neutral surface

Seven static classes and six constant classes join `Zanna.Services`, and three existing classes
gain members. Stateful members must run on the main thread (ADR 0352 trap). Reference-returning
rows are `owned`.

`Zanna.Services.Achievements`

| Member | Signature | Contract |
|---|---|---|
| `Unlock(id)` | `i1(str)` | Unlocks locally; `Stats.Store` commits and shows the notification |
| `Clear(id)` | `i1(str)` | Locks again (testing) |
| `IsUnlocked(id)` | `i1(str)` | |
| `UnlockTime(id)` | `i64(str)` | Unix seconds, 0 when locked or unknown |
| `IndicateProgress(id, current, maximum)` | `i1(str,i64,i64)` | Progress notification only |
| `Count` | `i64` | Achievements defined |
| `IdAt(index)` | `str(i64)` | `""` outside `0..Count-1` |
| `DisplayName(id)`, `Description(id)` | `str(str)` | Localized text, `""` when unavailable |
| `IsHidden(id)` | `i1(str)` | |

`Zanna.Services.Stats`: `GetInt i64(str)`, `SetInt i1(str,i64)`, `GetFloat f64(str)`,
`SetFloat i1(str,f64)`, `UpdateAverageRate i1(str,f64,f64)` (name, session count, session
seconds), `Store i1()` (commits stats and achievements; completion is `EventKind.StatsStored`),
`ResetAll i1(i1)` (testing; the flag also locks achievements).

`Zanna.Services.Leaderboards` (every member returns a `Zanna.Services.Request`, addressed by name):

| Member | Signature | Completed request |
|---|---|---|
| `Find(name)` | `obj<Zanna.Services.Request>(str)` | `Value` = board entry count, `Text` = name |
| `FindOrCreate(name, sort, display)` | `obj<Zanna.Services.Request>(str,i64,i64)` | as `Find` |
| `Upload(name, score, keepBest)` | `obj<Zanna.Services.Request>(str,i64,i1)` | `Value` = new global rank, `Flag` = stored score changed, `Text` = name |
| `Download(name, scope, start, end)` | `obj<Zanna.Services.Request>(str,i64,i64,i64)` | entries, `Value` = board entry count, `Text` = name |

`Zanna.Services.Presence`: `Set i1(str,str)` (an empty value removes the key), `Clear void()`.

`Zanna.Services.Overlay`: `IsEnabled i1`, `Open i1(i64)` (an `OverlayPage`),
`OpenWebPage i1(str,i1)` (URL, modal), `OpenStore i1(str,i1)` (provider product id, add to cart),
`SetNotificationPosition i1(i64)`, `SetNotificationInset i1(i64,i64)`. Members return true when the
request reached the platform.

`Zanna.Services.OnScreenKeyboard`: `ShowFloating i1(i64,i64,i64,i64,i64)` (mode, field x, y, width,
height in window pixels; keys arrive as ordinary keyboard input), `DismissFloating i1()`,
`RequestText obj<Zanna.Services.Request>(str,str,i64,i64)` (prompt, initial text, maximum length in
bytes, mode; the completed request holds the submitted text in `Text` and its byte length in
`Value`; cancelling fails it). The class is not called `TextInput`: runtime class leaf names are
unique across namespaces (`test_runtime_class_qualified_surface`), and `Zanna.GUI.TextInput`
already exists. The feature, request kind, event, and mode constants keep the `TextInput` names
because they describe text entry rather than name a class.

`Zanna.Services.Cloud`: `IsEnabled i1` (account and app), `QuotaTotal i64`, `QuotaAvailable i64`,
`Write i1(str,obj<Zanna.Collections.Bytes>)`, `Read obj<Zanna.Result>(str)` (`Ok(Bytes)` or
`Err(message)`), `Exists i1(str)`, `Delete i1(str)`, `Size i64(str)`, `Timestamp i64(str)`,
`Files seq<str>()`, `BeginBatch i1()`, `EndBatch i1()`. Operations are synchronous against the
platform's local copy.

Extended classes:

- `Zanna.Services.Platform.EventTotal i64`: the total a progress event's `EventValue` counts
  toward; 0 for other events.
- `Zanna.Services.Request`: `Flag i1`, `Text str`, `EntryCount i64`, and the methods
  `EntryRank i64(i64)`, `EntryScore i64(i64)`, `EntryUserId str(i64)`, `EntryUserName str(i64)`.
  An entry index outside `0..EntryCount-1` traps with
  `Services.Request.<Member>: index <i> is outside 0..<n>`, or
  `Services.Request.<Member>: index <i> is out of range; the request holds no entries`. A request
  holds at most 100 entries. `EntryUserName` asks the provider that produced the request for the
  current name while that provider is active (platforms learn names after the entries arrive) and
  otherwise returns the name captured at completion.

Constants (stable ordinals; existing values unchanged):

- `EventKind`: `StatsStored 8` (flag = stored, code = provider result), `AchievementStored 9`
  (text = id, flag = unlocked, value = progress, total = maximum), `TextInputDismissed 10`.
- `Feature`: `Achievements 5`, `Stats 6`, `Leaderboards 7`, `Presence 8`, `Overlay 9`,
  `TextInput 10`, `Cloud 11`.
- `RequestKind`: `LeaderboardFind 2`, `LeaderboardUpload 3`, `LeaderboardDownload 4`,
  `TextInput 5`.
- `LeaderboardScope`: `Global 0`, `AroundUser 1`, `Friends 2`.
- `LeaderboardSort`: `Ascending 1`, `Descending 2`.
- `LeaderboardDisplay`: `Numeric 1`, `Seconds 2`, `Milliseconds 3`.
- `OverlayPage`: `Friends 1`, `Community 2`, `Players 3`, `Settings 4`, `OfficialGroup 5`,
  `Stats 6`, `Achievements 7`.
- `NotificationPosition`: `TopLeft 0`, `TopRight 1`, `BottomLeft 2`, `BottomRight 3`.
- `TextInputMode`: `SingleLine 0`, `MultiLine 1`, `Email 2`, `Numeric 3`, `Password 4`.

### Argument rules

ADR 0352's rule that an absent provider never causes a trap still holds. Arguments are divided by
who can reject them:

- **Malformed for every provider: trap, even without a provider.** Empty identifiers
  (`Services.<Class>.<Member>: <what> must not be empty`, where `<what>` is `achievement id`,
  `stat name`, `leaderboard name`, `key`, `url`, `product id`, or `file name`); constants outside
  their class (`sort must be a LeaderboardSort value (got <n>)`,
  `display must be a LeaderboardDisplay value (got <n>)`,
  `scope must be a LeaderboardScope value (got <n>)`, `page must be an OverlayPage value (got <n>)`,
  `position must be a NotificationPosition value (got <n>)`,
  `mode must be a TextInputMode value (got <n>)`); download ranges no provider accepts
  (`Global ranks must satisfy 1 <= start <= end (got <s>..<e>)`,
  `start must not exceed end (got <s>..<e>)`,
  `a download spans at most 100 entries (got <s>..<e>)`; `Friends` ignores the range);
  `maxLength must be in 1..4096 (got <n>)`; and `Cloud.Write` data that is not Bytes
  (`data must be Zanna.Collections.Bytes`).
- **Values computed at run time: return false with a diagnostic.**
  `Services: Stats.SetFloat('<name>') rejected a non-finite value`;
  `Services: Stats.UpdateAverageRate('<name>') needs a finite count and a positive, finite session length`;
  `Services: Achievements.IndicateProgress('<id>') needs 0 <= current <= maximum and maximum > 0 (got <c> of <m>)`;
  `Services: OnScreenKeyboard.ShowFloating needs a non-negative width and height (got <w>x<h>)`.
- **Provider formats and limits:** the provider decides, as ADR 0352 does for app and DLC ids.
  A provider-defined id format traps only while that provider is active; limits return false or
  fail the request with a diagnostic.

### Provider model extension

`rt_services_provider` gains one pointer per feature to a static operation table
(`rt_services_achievement_ops`, `_stat_ops`, `_leaderboard_ops`, `_presence_ops`, `_overlay_ops`,
`_text_input_ops`, `_cloud_ops`); a NULL table or operation reports neutral values. The core
validates the provider-independent rules above before calling a provider.

`begin_request` receives an `rt_services_request_args` record (kind plus named argument fields)
instead of a bare kind. The provider handle it returns is the provider's own token and must stay
stable for the life of the request, so one request can span several platform calls. Providers
complete requests through `rt_services_provider_finish_request` with an
`rt_services_request_result` (status, code, value, flag, text, error, and up to 100 leaderboard
entries, all copied); `rt_services_provider_complete_request` remains as the short form. Progress
events use `rt_services_provider_emit_progress_event`.

`Platform.Diagnostics` skips a message identical to the newest retained one, so a call that fails
every frame cannot flush older diagnostics.

### Steam provider contract

Interfaces and exports (identical in SDK 1.61 through 1.65):

| Feature | Interface | Methods |
|---|---|---|
| Achievements | ISteamUserStats (v013) | `SetAchievement`, `ClearAchievement`, `GetAchievementAndUnlockTime`, `IndicateAchievementProgress`, `GetNumAchievements`, `GetAchievementName`, `GetAchievementDisplayAttribute` (`name`, `desc`, `hidden`) |
| Stats | ISteamUserStats (v013) | `GetStatInt32`, `SetStatInt32`, `GetStatFloat`, `SetStatFloat`, `UpdateAvgRateStat`, `StoreStats`, `ResetAllStats` |
| Leaderboards | ISteamUserStats (v013) | `FindLeaderboard`, `FindOrCreateLeaderboard`, `GetLeaderboardName`, `GetLeaderboardEntryCount`, `DownloadLeaderboardEntries`, `GetDownloadedLeaderboardEntry`, `UploadLeaderboardScore` |
| Entry names | ISteamFriends (v018/v017) | `GetFriendPersonaName`, `RequestUserInformation` |
| Presence | ISteamFriends | `SetRichPresence`, `ClearRichPresence` |
| Overlay pages | ISteamFriends | `ActivateGameOverlay`, `ActivateGameOverlayToWebPage`, `ActivateGameOverlayToStore` |
| Overlay status | ISteamUtils (v011/v010) | `IsOverlayEnabled`, `SetOverlayNotificationPosition`, `SetOverlayNotificationInset` |
| Text input | ISteamUtils | `ShowFloatingGamepadTextInput`, `DismissFloatingGamepadTextInput`, `ShowGamepadTextInput`, `GetEnteredGamepadTextLength`, `GetEnteredGamepadTextInput` |
| Cloud | ISteamRemoteStorage (`SteamAPI_SteamRemoteStorage_v016`) | `FileWrite`, `FileRead`, `FileExists`, `FileDelete`, `GetFileSize`, `GetFileTimestamp`, `GetFileCount`, `GetFileNameAndSize`, `GetQuota`, `IsCloudEnabledForAccount`, `IsCloudEnabledForApp`, `BeginFileWriteBatch`, `EndFileWriteBatch` |

Each row is one all-or-nothing group. A missing export disables its group and records
`Steam: export <symbol> unavailable; <features> disabled` for the first missing export (a missing
accessor records `Steam: interface <accessor> unavailable; <features> disabled`), and
`Platform.HasFeature` reports the group unavailable. `Overlay` requires both overlay groups.

Mappings: `OverlayPage` maps to the dialogs `friends`, `community`, `players`, `settings`,
`officialgamegroup`, `stats`, `achievements`; `OpenWebPage` modal maps to
`k_EActivateGameOverlayToWebPageMode_Modal`; `OpenStore` takes a decimal app id and maps
add-to-cart to `k_EOverlayToStoreFlag_AddToCartAndShow`; `NotificationPosition` ordinals equal
`ENotificationPosition`; `LeaderboardScope` maps to `ELeaderboardDataRequest` Global,
GlobalAroundUser, Friends; `keepBest` maps to `KeepBest`/`ForceUpdate`; floating keyboard modes
map to `EFloatingGamepadTextInputMode` (`Password` uses SingleLine); `RequestText` maps
`Password` to `k_EGamepadTextInputModePassword` and `MultiLine` to
`k_EGamepadTextInputLineModeMultipleLines`.

Leaderboards are addressed by name. The provider caches up to 32 name-to-`SteamLeaderboard_t`
mappings per session (the oldest is replaced). `Upload` and `Download` for an uncached name issue
`FindLeaderboard` first and continue under the same request, so they may take two pumps.
Downloaded entries are read immediately, all of them, because Steam frees the data once every
entry was read; the first 100 are kept, and a larger download records
`Steam: leaderboard '<name>' download returned <n> entries; kept the first 100`. User ids are
decimal SteamID64 strings. A name Steam reports as `[unknown]` is captured as `""` and requested
with `RequestUserInformation(id, true)`.

Decoded callbacks added to ADR 0352's table:

| Id | Callback | Payload bytes (pack 8 / pack 4) | Result |
|---|---|---|---|
| 714 | `GamepadTextInputDismissed_t` | 12 / 12 | completes the pending TextInput request |
| 738 | `FloatingGamepadTextInputDismissed_t` | any | `TextInputDismissed` |
| 1102 | `UserStatsStored_t` | 16 / 12 | `StatsStored` |
| 1103 | `UserAchievementStored_t` | 152 / 148 | `AchievementStored` (unlocked when progress and maximum are both 0) |
| 1104 | `LeaderboardFindResult_t` (call result) | 16 / 12 | advances a leaderboard request |
| 1105 | `LeaderboardScoresDownloaded_t` (call result) | 24 / 20 | completes Download |
| 1106 | `LeaderboardScoreUploaded_t` (call result) | 32 / 28 | completes Upload |

`GetDownloadedLeaderboardEntry` writes `LeaderboardEntry_t`: 32 bytes under pack 8, 28 under
pack 4 (the one-byte-aligned `CSteamID` is its first member). All layouts carry compile-time size
and offset assertions. At most one `RequestText` is pending at a time.

Steam diagnostics and request errors:

| Case | Behavior | Message |
|---|---|---|
| `SetAchievement`/`ClearAchievement` rejected | false | `Steam: SetAchievement('<id>') failed; check that the achievement is defined for this app` (and the `ClearAchievement` form) |
| Achievement read rejected | false/0 | `Steam: GetAchievementAndUnlockTime('<id>') failed; check that the achievement is defined for this app` |
| Progress above 32 bits | false | `Steam: achievement progress for '<id>' must fit in 32 bits (got <c> of <m>)` |
| Progress rejected | false | `Steam: IndicateAchievementProgress('<id>', <c>, <m>) failed; the achievement must be defined and locked, and progress must be above 0 and below the maximum` |
| Int stat outside int32 | false | `Steam: stat '<name>' value <v> is outside the int32 range` |
| Float stat outside float32 | false | `Steam: stat '<name>' value <v> is outside the float range` |
| Stat write rejected | false | `Steam: SetStatInt32('<name>', <v>) failed; check the stat's type, client write access, and limits` (and `SetStatFloat`) |
| Stat read rejected | 0 | `Steam: GetStatInt32('<name>') failed; check that the stat is defined as an INT stat` (and `GetStatFloat`: `as a FLOAT or AVGRATE stat`) |
| Store reverted stats | event code 8 | `Steam: StoreStats rejected one or more stats (EResult 8); Steam reverted them to the stored values` |
| Leaderboard missing | request fails | `Steam: leaderboard '<name>' was not found` |
| Leaderboard name too long | request fails | `Steam: leaderboard name '<name>' is longer than 127 bytes` |
| Score outside int32 | request fails | `Steam: leaderboard score <v> is outside the int32 range` |
| Upload rejected | request fails | `Steam: score upload to leaderboard '<name>' failed` |
| Feature group unbound | request fails | `Steam: leaderboards are unavailable (see Platform.Diagnostics)`, `Steam: text input is unavailable (see Platform.Diagnostics)` |
| Presence rejected | false | `Steam: SetRichPresence('<key>') failed; keys must be shorter than 64 bytes, values shorter than 256 bytes, and at most 30 keys may be set` |
| Malformed store id (trap, Steam active) | — | `Services.Overlay.OpenStore: Steam app id '<id>' must be an integer in 1..4294967295` |
| Floating keyboard refused | false | `Steam: ShowFloatingGamepadTextInput returned false; the floating keyboard needs Steam Deck or Big Picture mode` |
| Text input refused | request fails | `Steam: the gamepad text input could not be shown; it needs Steam Deck or Big Picture mode` |
| Second text input | request fails | `Steam: a text input request is already pending` |
| Text input cancelled | request fails | `Steam: text input was cancelled` |
| Cloud file too large | false | `Steam: cloud file '<name>' is <n> bytes; Steam Cloud accepts at most 104857600 bytes per file` |
| Cloud write rejected | false | `Steam: FileWrite('<name>', <n> bytes) failed; check the file name, the Steam Cloud quota, and the app's file count limit` |
| Cloud read of a missing file | Err | `Steam: cloud file '<name>' does not exist` |

## Consequences

- Games gain the store features a Steam release needs through classes that name no store; a second
  provider fills the same operation tables.
- The request object grows result text, a flag, and leaderboard entries, so later request kinds
  (for example async cloud or user-generated content queries) reuse it without new classes.
- `StoreStats` is rate-limited by Steam on the order of minutes; the documentation directs commits
  to natural break points.
- Leaderboard entry details (per-entry integer arrays), user-generated content attachments,
  global achievement percentages, and asynchronous cloud calls are not exposed; each can be added
  as a request kind without changing this surface.
- The runtime gains 13 public classes and 4 contract source pairs; the fake `steam_api` gains the
  new exports and a core-only profile that proves a redistributable without them degrades to
  neutral values.

## Alternatives Considered

- **Expose Steam handles (`SteamLeaderboard_t`) to games.** Rejected: other stores address boards
  by name, and a handle-based surface would leak into every future provider.
- **A `Zanna.Services.Steam` home for these features.** Rejected by the owner requirement that the
  surface serve other stores.
- **Per-entry `LeaderboardEntry` objects.** Rejected: index accessors on the request avoid one heap
  object per entry and keep the class count down.
- **Trap on every rejected value.** Rejected: stat values and progress are computed while the game
  runs, and a store-side limit must not end a shipped game.
- **Futures or callbacks for requests.** Rejected for the reasons in ADR 0352.
- **Asynchronous cloud calls only.** Rejected for this phase: Steam's synchronous calls touch only
  the local cache and fit save and load paths; asynchronous kinds can be added later.

## Amendment (2026-09-14): RequestText maxLength counts characters

`OnScreenKeyboard.RequestText(prompt, initialText, maxLength, mode)` passes `maxLength` to
`ShowGamepadTextInput` as `unCharMax`, which Steam treats as a character limit. The contract above
said bytes. `maxLength` is the most characters the player may enter (still `1..4096`). The
completed request's `Value` stays the submitted text's length in bytes, which exceeds the character
count for non-ASCII text. The flat signatures used by this record were rechecked against the SDK
1.61 and 1.65 headers and are unchanged.
