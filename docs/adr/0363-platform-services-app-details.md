---
status: accepted
audience: contributors
last-verified: 2026-09-14
---

# ADR 0363: Platform Services App Details (DLC List, Build Id, Branch)

## Status

Accepted. Extends `Zanna.Services.Platform` from
[ADR 0352](0352-platform-services-runtime-loaded-providers.md). The policy, provider model, and
neutral-value rules there apply unchanged.

## Context

`Platform.IsDlcInstalled(dlcId)` answers one question about a DLC the game already knows. A
commercial game also needs to list what it sells: a store screen shows every expansion with its
name and whether it can be bought yet. Support and bug reports need to know which build a player
runs and whether it came from a beta branch.

Steam answers these through `ISteamApps` with `GetDLCCount`, `BGetDLCDataByIndex`,
`GetAppBuildId`, and `GetCurrentBetaName`. Their flat signatures are identical in SDK 1.61 and
1.65. (`GetBetaInfo` changed signature between those releases and is not used.)

## Decision

`Zanna.Services.Platform` gains, main-thread only and neutral without a provider:

| Member | Signature | Ownership | Contract |
|---|---|---|---|
| `DlcCount` | `i64` | | DLC the application defines, owned or not; 0 when unavailable |
| `DlcIdAt(index)` | `str(i64)` | owned | Provider-defined DLC id, or `""` outside `0..DlcCount-1` |
| `DlcNameAt(index)` | `str(i64)` | owned | Display name, or `""` |
| `DlcAvailableAt(index)` | `i1(i64)` | | Whether the DLC can be bought now |
| `BuildId` | `i64` | | Installed build id; 0 when the build did not come from the platform |
| `BranchName` | `str` | owned | Branch of the installed build (Steam beta name), or `""` on the default branch |

Out-of-range indexes return neutral values instead of trapping, as `Achievements.IdAt` does.
`Feature.AppDetails` is `14`.

The provider table gains the query callbacks `dlc_count`, `dlc_at`, `build_id`, and
`branch_name`. The Steam provider binds the four `ISteamApps` exports as their own group. A
redistributable without them keeps licensing, DLC, language, and launch parameter queries, and
records `Steam: export <symbol> unavailable; DLC list, build id, and branch queries disabled`.

Steam mapping details:

- DLC ids are decimal app ids.
- Names and branch names are read into 256-byte buffers.
- `GetCurrentBetaName` returning false means the default branch.
- Steam documents that the DLC count may stop at 64.

## Consequences

- A game can build a DLC store screen and report its build and branch without naming Steam.
- With a signed-in Steam client on macOS (app 480), every member reached Steam. Spacewar defines
  no DLC, and a build Steam did not launch reports build id 0 and no branch.

## Alternatives Considered

- **One `DlcInfo` object per DLC.** Rejected. Index accessors avoid heap objects and match
  `Achievements.IdAt`.
- **`GetBetaInfo` for branch details.** Rejected. Its signature changed between SDK 1.61 and
  1.65 (a last-updated field was added), and the branch name covers the support need.
