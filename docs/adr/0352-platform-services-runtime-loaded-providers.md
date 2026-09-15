---
status: accepted
audience: contributors
last-verified: 2026-09-14
---

# ADR 0352: Platform Services with Runtime-Loaded Provider Redistributables (Steamworks First)

## Status

Accepted. Phase 1 (neutral services layer, Steam provider core, tests, documentation) is
implemented by this change; achievements, stats, leaderboards, cloud storage, rich presence,
overlay control, and packaging support are later phases that extend the same contracts.
Achievements, stats, leaderboards, rich presence, overlay control, text input, and cloud storage
are specified by [ADR 0353](0353-platform-services-player-features.md), which also extends the
provider interface described below. Packaging games with the redistributable (Steam depot
targets for `zanna package`) is specified by [ADR 0354](0354-store-depot-packaging.md).

## Context

Legacy Baseball is a commercial Zanna game bound for Steam, and the runtime has no way to reach a
distribution platform's services: identity, licensing, DLC, platform events, and (later)
achievements, stats, leaderboards, and cloud storage. Three constraints shape the solution.

1. **Zero dependencies.** Zanna builds from repository code only. The Steamworks SDK headers,
   import libraries, and binaries must never enter the build or the repository.
2. **Steamworks has one legal channel.** The Steam client protocol is private, and the Steamworks
   SDK Access Agreement (section 2.4) forbids replacing the SDK's functionality or communicating
   with Steamworks Services except through the API of the SDK redistributables
   (`steam_api64.dll`, `libsteam_api.dylib`, `libsteam_api.so`). Valve publishes a flat C API
   (`SteamAPI_*` exports) and a manual callback-dispatch API designed for non-C++ bindings.
3. **More stores will follow.** The layer must not be a Steam class; Steam is the first provider
   behind a neutral surface.

Precedent already covers runtime-resolved native interfaces without build dependencies: the
OpenGL backend resolves `libGL.so.1` with `dlsym`, the Wayland backend (ADR 0112, ADR 0139) loads
the client library through a header-free dispatch table, and the Windows accessibility bridge loads
`uiautomationcore.dll` at run time. The native linker already accepts `dlopen`/`dlsym`/`dlerror`
and `LoadLibraryW`/`GetProcAddress` imports. What is new is the kind of library: a platform-holder
redistributable that the *application developer* ships beside the executable, rather than an
operating-system component.

Existing runtime API rules also apply: routine failures are values, not `LastError` side channels
(ADR 0052); every reference-returning registry row declares ownership (ADR 0314); every public
class carries authored documentation (ADR 0101).

## Decision

### Policy: optional platform-holder redistributables

A runtime component may bind a platform holder's redistributable library when all of the following
hold, and the Steam provider is the first such binding:

- The binding is optional and loaded at run time by absolute path. Zanna-built executables never
  import it at link time and start normally without it.
- Nothing from the vendor enters the repository or the build: declarations cover only the subset
  used and are authored for interoperability from public documentation.
- Absence is a normal state. Every API returns a neutral value when the provider is unavailable.
- Tests run against a from-scratch fake library built only by the test tree.

### Namespace, component, and layout

- Runtime namespace `Zanna.Services`; runtime component archive `zanna_rt_services`
  (`RtComponent::Services`), C symbol prefix `rt_services_`, namespace prefix `Zanna.Services.`.
  Native links that use the component also pull `zanna_rt_io_fs` (executable directory) and
  `zanna_rt_oop` (runtime objects) through the dependency closure.
- Sources live in `src/runtime/services/` (neutral core, provider interface, dynamic-library
  adapters) and `src/runtime/services/steam/` (Steam provider). The frame-pump slot lives in Base
  (`src/runtime/core/rt_service_hooks.c`) so graphics code can call it without linking the
  services component.

### Provider model

A provider is a static `rt_services_provider` table (`src/runtime/services/rt_services_provider.h`)
with an identifier (`"steam"`), a display name, and callbacks: `start`, `pump`, `stop`,
`has_feature`, identity and licensing queries, and `begin_request`. Providers report back through
core functions: emit an event, add a diagnostic, look up a pending request's kind, and complete a
request. The core owns all shared state (status, event queue, diagnostics, pending requests).

Providers are compiled into the component's registry table. Exactly one provider is active per
process. Adding a store means adding one provider table, one registry entry, optional
`Zanna.Services.<Provider>` extension classes, and its tests; the neutral surface does not change.

### Phase 1 surface

All classes are static except `Request`. Stateful members must be called on the main thread and
trap otherwise with `Services: <Class>.<Member> must be called on the main thread`. Constant
classes may be read anywhere.

`Zanna.Services.Platform`

| Member | Signature | Ownership | Contract |
|---|---|---|---|
| `Init(provider, appId)` | `obj<Zanna.Result>(str,str)` | owned | `Ok(providerId)` or `Err(message)`; sets `Status` |
| `Update()` | `void()` | | Pumps the active provider; no-op when not started |
| `Shutdown()` | `void()` | | Stops the provider, cancels pending requests, clears queues; idempotent |
| `PollEvent()` | `i64()` | | Next event kind, or `EventKind.None` (0) when the queue is empty |
| `HasProvider(name)` | `i1(str)` | | Whether a provider id is compiled into this runtime |
| `HasFeature(feature)` | `i1(i64)` | | Whether the active provider supports a feature now |
| `IsDlcInstalled(dlcId)` | `i1(str)` | | Provider-defined DLC id; false when unavailable |
| `RequestPlayerCount()` | `obj<Zanna.Services.Request>()` | owned | Starts an async request; never null |
| `Diagnostics()` | `seq<str>()` | owned | Copies of up to 32 most recent non-fatal diagnostics |
| `IsAvailable` | `i1` | | True while a provider is started |
| `Status` | `i64` | | `Zanna.Services.Status` value |
| `Provider`, `AppId`, `UserId`, `UserName`, `Language` | `str` | owned | Empty string when unavailable |
| `IsLicensed`, `IsOnline` | `i1` | | False when unavailable |
| `EventResultCode`, `EventValue` | `i64` | | Fields of the last polled event |
| `EventText` | `str` | owned | Field of the last polled event |
| `EventFlag` | `i1` | | Field of the last polled event |
| `DroppedEvents` | `i64` | | Events discarded because the queue was full |

`Zanna.Services.Request` (instances come only from request methods): `Kind i64`, `IsDone i1`,
`Succeeded i1`, `ResultCode i64`, `Value i64`, `Error str` (owned). Requests never block;
callers poll `IsDone` across frames. `Zanna.Threads.Future` is deliberately not used: a blocking
`Get` on the main thread would deadlock because results arrive only while the main thread pumps.

`Zanna.Services.Steam` (extension class, neutral values unless Steam is the active provider except
where noted): `RestartAppIfNecessary(appId) i1(i64)` (works before `Init`), `IsActive i1`,
`SteamId i64`, `HardwareType i64`, `IsUnderProton i1`, `IsBigPicture i1`, `LibraryPath str` (owned).

Constant classes (stable ordinals):

- `Zanna.Services.Status`: `Ok 0`, `NotStarted 1`, `UnknownProvider 2`, `LibraryNotFound 3`,
  `LibraryIncompatible 4`, `UnsupportedPlatform 5`, `ClientNotRunning 6`, `VersionMismatch 7`,
  `InitFailed 8`.
- `Zanna.Services.EventKind`: `None 0`, `ServiceConnected 1`, `ServiceDisconnected 2`,
  `ConnectFailed 3`, `OverlayChanged 4`, `DlcInstalled 5`, `LaunchParametersChanged 6`,
  `ServiceShutdown 7`.
- `Zanna.Services.Feature`: `Identity 1`, `Licensing 2`, `Language 3`, `PlayerCount 4`.
- `Zanna.Services.RequestKind`: `PlayerCount 1`.
- `Zanna.Services.SteamHardware`: `Unknown -1`, `None 0`, `SteamDeck 1`, `SteamMachine 2`,
  `SteamFrame 3`.

### Lifecycle semantics

- `Status` starts at `NotStarted`. A successful `Init` makes it `Ok`; a failed `Init` leaves no
  active provider and sets the failure status; `Shutdown` returns it to `NotStarted`.
- `Init` with the already-active provider returns `Ok` without restarting. `Init` with a
  different provider while one is active returns
  `Err("Services: provider '<active>' is already active; call Platform.Shutdown() before starting '<requested>'")`
  and leaves `Status` unchanged.
- An unknown provider returns `Err("Services: unknown provider '<name>' (available: steam)")` with
  `Status.UnknownProvider`.
- Events use a 256-entry FIFO. When it is full the oldest event is dropped, `DroppedEvents`
  increments, and a diagnostic is recorded once per overflow episode.
- At most 64 requests may be pending. Beyond that a request completes immediately as failed with
  `Services: too many pending requests (limit 64)`. `Shutdown` completes pending requests as failed
  with `Services: request cancelled by Platform.Shutdown()` and `ResultCode` 0.
- While a provider is started, `Canvas.Poll` and `Canvas3D.Poll` (and therefore `World3D.Update`)
  call the Base frame-pump slot, so games with a normal render loop are pumped automatically.
  `Platform.Update()` remains available for loops without a canvas.
- There is no automatic provider shutdown at process exit. Library static destructors registered
  after the runtime's exit handler run first, and native Windows executables exit without running
  exit handlers. Games call `Platform.Shutdown()` on their quit path; the platform client already
  tolerates abrupt exit.

### Steam provider contract

**Library resolution** (exactly one candidate, never a search path, never unloaded):

1. `ZANNA_SERVICES_STEAM_LIBRARY`, when set and non-empty, is used as the exact path. It exists for
   development runs under `zanna run`, where the executable directory is the toolchain's, and for
   tests.
2. Otherwise `<executable directory>/steam_api64.dll` on Windows x64,
   `<executable directory>/libsteam_api.dylib` on macOS (the `Contents/MacOS` directory inside an
   application bundle), and `<executable directory>/libsteam_api.so` on Linux x64 and arm64.
3. Windows arm64 has no Steamworks redistributable and reports `UnsupportedPlatform`.

The C runtime cannot distinguish VM from native execution (`Environment.IsNative` is overridden
only at the frontend call boundary), so the working directory is never searched; the explicit
override replaces it.

**App identity.** Steam app ids are decimal integers in `1..4294967295`. When `SteamAppId` or
`SteamGameId` is absent from the environment, `Init` sets it to the requested app id *before*
loading the library, which replaces the development-only `steam_appid.txt` file.
`RestartAppIfNecessary` never sets them, because Steam uses their presence to detect a Steam launch.

**Supported redistributables:** Steamworks SDK 1.61 through 1.65. Core exports resolved as one
all-or-nothing table: `SteamAPI_InitFlat`, `SteamAPI_Shutdown`, `SteamAPI_RestartAppIfNecessary`,
`SteamAPI_IsSteamRunning`, `SteamAPI_GetHSteamPipe`, and `SteamAPI_ManualDispatch_Init`,
`_RunFrame`, `_GetNextCallback`, `_FreeLastCallback`, `_GetAPICallResult`. Interfaces are resolved
by exact accessor name; the method signatures the provider calls are identical across every listed
version:

| Interface | Accepted accessors | Methods used |
|---|---|---|
| ISteamUser | `SteamAPI_SteamUser_v023` | `BLoggedOn`, `GetSteamID` |
| ISteamFriends | `SteamAPI_SteamFriends_v018`, `_v017` | `GetPersonaName` |
| ISteamUtils | `SteamAPI_SteamUtils_v011` (1.65), `_v010` (1.61–1.64) | `GetAppID`, `IsSteamInBigPictureMode`; v011: `IsRunningOnSteamHardware`, `IsRunningUnderProton`; v010: `IsSteamRunningOnSteamDeck` |
| ISteamApps | `SteamAPI_SteamApps_v009`, `_v008` | `BIsSubscribed`, `BIsDlcInstalled`, `GetCurrentGameLanguage` |
| ISteamUserStats | `SteamAPI_SteamUserStats_v013` | `GetNumberOfCurrentPlayers` |

A missing or null interface disables only its features and records
`Steam: interface <accessor> unavailable; <features> disabled`.

**Initialization:** `SteamAPI_InitFlat` then `SteamAPI_ManualDispatch_Init` (which must follow a
successful init) then `SteamAPI_GetHSteamPipe` then the interface accessors.

**Pump:** `ManualDispatch_RunFrame`, then `GetNextCallback`/`FreeLastCallback` pairs, at most 1024
callbacks per pump. Decoded callbacks:

| Id | Callback | Payload bytes | Event |
|---|---|---|---|
| 101 | `SteamServersConnected_t` | any | `ServiceConnected` |
| 102 | `SteamServerConnectFailure_t` | 8 | `ConnectFailed` (code = EResult, flag = still retrying) |
| 103 | `SteamServersDisconnected_t` | 4 | `ServiceDisconnected` (code = EResult) |
| 331 | `GameOverlayActivated_t` | 12 | `OverlayChanged` (flag = active, value = user-initiated) |
| 703 | `SteamAPICallCompleted_t` | 16 | routes a call result |
| 704 | `SteamShutdown_t` | any | `ServiceShutdown` |
| 1005 | `DlcInstalled_t` | 4 | `DlcInstalled` (text and value = DLC app id) |
| 1014 | `NewUrlLaunchParameters_t` | any | `LaunchParametersChanged` |
| 1107 | `NumberOfCurrentPlayers_t` (call result) | 8 | completes `PlayerCount` (value = players) |

**Layouts:** callback structures use `#pragma pack(8)` on Windows and `#pragma pack(4)` on macOS
and Linux, matching the redistributables. The provider authors each decoded layout once under the
platform pack with compile-time size and offset assertions, including a packing sentinel
(`uint32, uint64, uint16, double` = 32 bytes under pack 8, 24 under pack 4). A payload whose size
differs from the declared size is not decoded and records
`Steam: callback <id> payload is <n> bytes; binding expects <m>`.

**Diagnostic and error text** (`Init` errors are also appended to `Diagnostics()`):

| Case | Status | Message |
|---|---|---|
| Library file missing | `LibraryNotFound` | `Steam: steam_api library not found: <path>` |
| Library fails to load | `LibraryIncompatible` | `Steam: could not load '<path>': <loader reason>` |
| Core export missing | `LibraryIncompatible` | `Steam: <path> is missing export '<symbol>' (Steamworks SDK 1.61-1.65 redistributable required)` |
| Unsupported host | `UnsupportedPlatform` | `Steam: no Steamworks redistributable exists for <os>-<arch>` |
| `SteamAPI_InitFlat` fails | `InitFailed`, `ClientNotRunning`, `VersionMismatch` | `Steam: SteamAPI_InitFlat failed (<FailedGeneric\|NoSteamClient\|VersionMismatch>): <Valve message>` |
| Malformed app id (trap) | — | `Services.Platform.Init: Steam app id '<id>' must be an integer in 1..4294967295` |
| Malformed DLC id while Steam is active (trap) | — | `Services.Platform.IsDlcInstalled: Steam DLC id '<id>' must be an integer in 1..4294967295` |
| `RestartAppIfNecessary` id out of range (trap) | — | `Services.Steam.RestartAppIfNecessary: app id <n> must be in 1..4294967295` |
| Init after a GPU presenter exists | unchanged | `Steam: initialized after a GPU-presented window was created; the desktop overlay may not attach` |

### Graphics interaction

The Steam desktop overlay hooks GPU presentation and must see the graphics device created after
`SteamAPI_Init`. Canvas3D notes the first GPU-presented window in a Base-level flag, and the Steam
provider records the diagnostic above when it starts afterwards. The 2D Canvas and GUI present
through CPU blits and never receive the desktop overlay; this is documented rather than changed.

## Consequences

- Games gain identity, licensing, DLC, language, platform events, and player counts on Steam, and
  the same binary runs unchanged without Steam (itch.io, development, CI).
- The repository stays free of Valve files; developers download the redistributable under Valve's
  agreement and ship it beside their executable. Packaging support is a later phase.
- Each Steamworks SDK release must be checked against the accessor table before the supported range
  is extended; a newer redistributable that drops an accessor degrades to disabled features, not
  crashes.
- The runtime gains a new component archive and 8 public runtime classes; the source-health
  contract-file baseline grows accordingly.
- Distributing a GPL-licensed game that loads the proprietary redistributable raises a license
  question that the planned runtime-exception legal review must settle; commercially licensed
  games are unaffected.

## Alternatives Considered

- **Compile and link against the Steamworks SDK.** Rejected: violates the zero-dependency rule,
  places vendor headers in the build, and makes executables fail to start without the library.
- **Reimplement the Steam client protocol.** Rejected: undocumented, changes with client updates,
  and prohibited by the SDK agreement.
- **Steam Web API only.** Rejected as the integration path: publisher keys cannot ship in clients
  and the overlay and in-game services are unreachable; it remains available to Zanna-built
  servers through `Zanna.Network`.
- **A single `Zanna.Steam` class.** Rejected: every future store would need a parallel surface.
- **A `LastError` string property.** Rejected per ADR 0052; `Init` returns a Result and non-fatal
  problems go to the bounded `Diagnostics()` list.
- **Futures for asynchronous results.** Rejected because blocking waits deadlock the pump.
- **Searching the working directory for the library.** Rejected: it enables library planting and
  cannot be limited to VM runs; the explicit override covers development.
- **Automatic shutdown from the runtime's exit handler.** Rejected for the destructor-ordering and
  Windows exit reasons above.

## Amendment (2026-09-14): verification against the real SDK, init status, launch parameters

**Verification.** The binding was checked against a genuine Steamworks SDK 1.61 and against the SDK
1.65 flat header. Every export name the provider resolves exists in the 1.61 macOS
redistributable, except the 1.65 names that have 1.61 fallbacks (`SteamAPI_SteamUtils_v011`,
`IsRunningOnSteamHardware`, `IsRunningUnderProton`, `SteamAPI_SteamApps_v009`,
`SteamAPI_SteamFriends_v018`). Every bound flat signature is identical in both headers. The
callback ids, the pack-4 structure sizes and offsets, and the enum values used by the provider
compiled against the 1.61 headers with static assertions. With a signed-in Steam client on macOS
arm64 and app 480, `examples/apps/steam-check` passed every check it runs:

- Library: the 1.61 redistributable, and the 1.65-generation `libsteam_api.dylib` bundled with
  the Steam client.
- Runs: VM runs, and a `zanna package --target steam-macos` bundle.
- Checks: identity, player counts, achievements, stats, all leaderboard request kinds with
  entries, presence, cloud files, launch parameters, and `Shutdown` followed by a second `Init`.

The client posts `UserAchievementStored_t` before `UserStatsStored_t`, and the fake library now
does the same. The desktop overlay, the keyboards, and Windows and Linux clients remain
unverified.

**Init status.** A machine without the Steam client makes `SteamAPI_InitFlat` return
`FailedGeneric` ("Could not determine Steam client install directory."), which the table above
mapped to `InitFailed`. When `InitFlat` returns `FailedGeneric` and `SteamAPI_IsSteamRunning()`
is false, the status is now `ClientNotRunning`; the message still carries Valve's text. A running
client with no signed-in user also reports `FailedGeneric` ("ConnectToGlobalUser failed.") while
`IsSteamRunning` is true, and keeps `InitFailed`. When `InitFlat` fails, the redistributable
writes `[S_API]` lines to the console itself; they cannot be suppressed.

**Launch parameters.** `EventKind.LaunchParametersChanged` had no way to read the new values.
`Zanna.Services.Platform` gains:

| Member | Signature | Ownership | Contract |
|---|---|---|---|
| `LaunchCommandLine` | `str` | owned | Command line of a platform launch URL (Steam: `steam://run/<appid>//<command line>/`), or `""` |
| `LaunchParameter(key)` | `str(str)` | owned | Named launch parameter (Steam: `steam://run/<appid>//?key=value`), or `""`; an empty key traps with `Services.Platform.LaunchParameter: key must not be empty` |

`Zanna.Services.Feature.LaunchParameters` is `12`. The provider table gains the query callbacks
`launch_command_line` and `launch_parameter`. The Steam provider binds
`SteamAPI_ISteamApps_GetLaunchCommandLine` and `SteamAPI_ISteamApps_GetLaunchQueryParam`
(identical in SDK 1.61 through 1.65) as their own group. A redistributable without them keeps
licensing, DLC, and language queries and records
`Steam: export <symbol> unavailable; launch parameters disabled`. The command line is read into a
4096-byte buffer; text that fills it records
`Steam: the launch command line filled the 4096-byte buffer and may be truncated`.

**Provider contract.** The core registers a request after `begin_request` returns, so a provider
must never complete the request it is starting from inside that callback. It returns 0 to fail
synchronously and completes later requests from `pump`.
