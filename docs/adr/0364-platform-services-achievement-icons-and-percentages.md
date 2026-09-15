---
status: accepted
audience: contributors
last-verified: 2026-09-15
---

# ADR 0364: Platform Services Achievement Icons and Global Unlock Percentages

## Status

Accepted. Extends `Zanna.Services.Achievements` from
[ADR 0353](0353-platform-services-player-features.md). The request and event model is from
[ADR 0352](0352-platform-services-runtime-loaded-providers.md). The policy, provider model, and
neutral-value rules there apply unchanged.

## Context

An in-game achievement screen shows each achievement's icon. It often marks rare achievements
with the share of players who unlocked them. `Zanna.Services.Achievements` could name
achievements and read their state, but a game had no platform icon and no global statistic to
draw.

Steam serves icons through `ISteamUserStats::GetAchievementIcon`, which returns an image handle
for the achievement's current state. `ISteamUtils::GetImageSize` and `GetImageRGBA` read that
image. When the icon is not loaded yet, the call returns 0, starts a load, and later posts
`UserAchievementIconFetched_t` (callback 1109).

Global percentages arrive in one download. `RequestGlobalAchievementPercentages` completes with
the `GlobalAchievementPercentagesReady_t` call result (1110). After that,
`GetAchievementAchievedPercent` answers per achievement.

The flat signatures of all five exports are identical in SDK 1.61 and 1.65.

## Decision

`Zanna.Services.Achievements` gains these members. All are main-thread only and neutral without a
provider. An empty id traps, as for the existing members.

| Member | Signature | Ownership | Contract |
|---|---|---|---|
| `IconWidth(id)` | `i64(str)` | | Width of the icon for the current state; 0 while it loads or when unavailable |
| `IconHeight(id)` | `i64(str)` | | Height of that icon; 0 likewise |
| `IconRgba(id)` | `obj<Zanna.Collections.Bytes>(str)` | owned | `width * height * 4` RGBA bytes, rows top to bottom; empty `Bytes` likewise |
| `RequestGlobalPercentages()` | `obj<Zanna.Services.Request>()` | owned | Downloads every achievement's global unlock share |
| `GlobalPercent(id)` | `f64(str)` | | Share in 0..100 after a successful request; 0 otherwise |

New constants:

- `EventKind.AchievementIconReady` is `11`. `EventText` holds the achievement id and `EventFlag`
  the unlocked variant. `EventValue` is `1` when the achievement has an icon for that state and
  `0` when it has none.
- `Feature.AchievementIcons` is `15` and `Feature.AchievementPercentages` is `16`.
- `RequestKind.AchievementPercentages` is `8`. It carries no `Value`, `Flag`, `Text`, or details.

Provider contract: `rt_services_achievement_ops` gains two operations.

- `icon(id, &width, &height, out_rgba)` returns 1 only when the icon is loaded. A `NULL`
  `out_rgba` reads only the size. Otherwise it receives caller-owned `Bytes`.
- `global_percent(id, &percent)`.

Providers serve the new request kind through `begin_request`.

Steam binding:

- **Icon group.** `GetAchievementIcon`, `GetAchievementAndUnlockTime` (to know the current state),
  `GetImageSize`, `GetImageRGBA`, and an open `ISteamUtils`. A missing export records
  `Steam: export <symbol> unavailable; achievement icons disabled`.
- **Percentage group.** `RequestGlobalAchievementPercentages` and
  `GetAchievementAchievedPercent`. A missing export records
  `Steam: export <symbol> unavailable; global achievement percentages disabled`.
- **Unset icons.** Steam answers every `GetAchievementIcon` call for an achievement without an icon
  with another `UserAchievementIconFetched_t` that carries handle 0. An app that reads the icon
  again when the ready event arrives would therefore loop without end. The binding remembers the
  unset variants reported in a session, in a heap list freed at `Shutdown`. It passes the first
  report on and does not call `GetAchievementIcon` for those variants again.
- **Unknown ids.** An undefined id is caught before any icon call and records
  `Steam: GetAchievementAndUnlockTime('<id>') failed; check that the achievement is defined for this app`.
- **Oversized images.** Images larger than 4096 by 4096 pixels are refused before allocation with
  `Steam: achievement '<id>' icon is <w>x<h> pixels; the binding reads at most 4096x4096`.
- **Reading before a download.** `GlobalPercent` before a successful request records
  `Steam: global achievement percentages are not loaded; call Achievements.RequestGlobalPercentages first`.
  After one, an unknown id records
  `Steam: GetAchievementAchievedPercent('<id>') failed; check that the achievement is defined for this app`.
- **Failed downloads.** A result other than `k_EResultOK` fails the request with
  `Steam: global achievement percentages are unavailable (EResult <n>)` and the result in
  `ResultCode`. Valve documents `k_EResultFail` (2) for both a failed call and an app without
  global percentages.
- **Layouts.** `UserAchievementIconFetched_t` is 144 bytes under both packings: name at 8,
  `m_bAchieved` at 136, `m_nIconHandle` at 140. `GlobalAchievementPercentagesReady_t` is 16 bytes
  under pack 8 and 12 under pack 4, with `m_eResult` at 8. Static assertions pin both, and the
  pack-4 values match the SDK 1.61 headers.

## Consequences

- A game can draw achievement icons, as `Pixels.FromBytes` or textures, and mark rare
  achievements without naming Steam. The same code is neutral in non-Steam builds.
- Icons cost one frame on first use. Games should redraw on `AchievementIconReady` rather than
  poll every frame, although polling is safe.
- Tests use the fake redistributable. It models:
  - icons that load on the next frame;
  - an achievement with no icon, answering every request with a new report, as Steam does;
  - global percentages, including a failed download;
  - the core-only profile without either group.

  The Zia and BASIC fixtures cover both groups on the VM and in native binaries.
- **Live check (2026-09-15, macOS arm64, Spacewar 480, both SDK 1.61 and the 1.65-generation
  library):**
  - Icons are 64 by 64 RGBA, and saved as PNG they show the Steamworks artwork.
  - Uncached icons arrived after one `AchievementIconReady`.
  - Spacewar's `NEW_ACHIEVEMENT_0_4` has no icon. Before the unset-icon rule it produced 26,275
    ready events in eight seconds; with the rule it produced one.
  - Spacewar has no global percentages. The request fails with EResult 2, and Steam's public Web
    API `GetGlobalAchievementPercentagesForApp` also returns none for app 480, so only the fakes
    exercise a successful download.

## Alternatives Considered

- **Return `Zanna.Graphics.Pixels`.** Rejected. `zanna_rt_services` does not depend on the
  graphics runtime, and `Bytes` also serves GUI and texture uploads.
- **Expose image handles.** Rejected. Handles are Steam's model, and an app would need a second,
  Steam-shaped API to read them.
- **Block until an icon loads.** Rejected. Every `Zanna.Services` call is non-blocking; the ready
  event plus a second read keeps frames smooth.
- **Return all percentages in the request.** Rejected. `GlobalPercent(id)` reads like
  `IsUnlocked(id)` and needs no per-request storage for hundreds of achievements.
- **Leave unset icons to the app.** Rejected. The natural read-on-event pattern would flood the
  256-event queue and drop other events.
