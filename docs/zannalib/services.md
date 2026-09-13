---
status: active
audience: public
last-verified: 2026-09-12
---

# Platform Services

> Distribution-platform services behind one provider-neutral API: user identity, licensing, DLC,
> platform events, and asynchronous requests. Steam is the first provider.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Zanna.Services.Platform](#zannaservicesplatform)
- [Zanna.Services.Request](#zannaservicesrequest)
- [Zanna.Services.Steam](#zannaservicessteam)
- [Constants](#constants)
- [Shipping with the Steam Provider](#shipping-with-the-steam-provider)
- [Testing Without Steam](#testing-without-steam)
- [Adding a Provider](#adding-a-provider)

For exact signatures, see the generated [Services reference](../generated/runtime/services.md).
The design is recorded in
[ADR 0352](../adr/0352-platform-services-runtime-loaded-providers.md).

---

## Overview

`Zanna.Services` connects a game to the store it is distributed through. The API is split in two:

- **Neutral classes** work the same for every provider: `Platform` (lifecycle, identity, licensing,
  events, requests), `Request`, and the constant classes `Status`, `EventKind`, `Feature`, and
  `RequestKind`.
- **Provider extension classes** expose what only one store offers. `Zanna.Services.Steam` and its
  `SteamHardware` constants are the first.

Providers are compiled into the runtime and selected by id when the game starts. Only `"steam"`
exists today; other stores are added behind the same `Platform` surface.

Key rules:

- **Absence is normal.** Without a started provider (no Steam client, no redistributable library,
  or a build shipped elsewhere) every query returns `""`, `0`, or `false` and nothing traps. One
  build runs on Steam, on other stores, and during development.
- **No build dependency.** The Steam provider loads Valve's `steam_api` redistributable at run
  time from beside the executable. Nothing from the Steamworks SDK is needed to build Zanna or
  your game. You ship the redistributable with the game.
- **Main thread only.** Stateful members trap with
  `Services: <Class>.<Member> must be called on the main thread` when called from another thread.
  Constant classes can be read from any thread.
- **Automatic pumping.** While a provider is started, every `Canvas.Poll` and `Canvas3D.Poll` (and
  therefore `World3D.Update`) delivers pending platform callbacks. Loops without a canvas call
  `Platform.Update()` once per frame.
- **Errors are values.** `Platform.Init` returns a `Zanna.Result`, `Platform.Status` holds a
  `Zanna.Services.Status` code, and non-fatal problems are kept in `Platform.Diagnostics()`.
- **Shut down explicitly.** The runtime does not stop the provider at process exit. Call
  `Platform.Shutdown()` on the game's quit path.

---

## Quick Start

```zia
module Main;

bind Zanna.Terminal;
bind Zanna.Services as Services;

func start() {
    // Release builds only: relaunch through the Steam client when started outside it.
    if Services.Steam.RestartAppIfNecessary(480) {
        return;
    }

    // Start before creating any window so the Steam overlay can attach.
    var started = Services.Platform.Init("steam", "480");
    if started.IsErr {
        Say("Playing without Steam: " + started.UnwrapErrStr());
    } else {
        Say("Welcome, " + Services.Platform.UserName);
    }

    // Inside the frame loop, after Canvas.Poll or Canvas3D.Poll:
    var kind = Services.Platform.PollEvent();
    while kind != Services.EventKind.None {
        if kind == Services.EventKind.OverlayChanged && Services.Platform.EventFlag {
            Say("Overlay opened; pause the game");
        }
        kind = Services.Platform.PollEvent();
    }

    Services.Platform.Shutdown();
}
```

```basic
DIM started AS OBJECT
started = Zanna.Services.Platform.Init("steam", "480")

IF Zanna.Services.Platform.IsAvailable THEN
    PRINT "Welcome, "; Zanna.Services.Platform.UserName
ELSE
    PRINT "Playing without Steam (status "; Zanna.Services.Platform.Status; ")"
END IF

Zanna.Services.Platform.Shutdown()
```

---

## Zanna.Services.Platform

Provider-neutral lifecycle, identity, licensing, events, and requests.

**Type:** Static utility class

### Methods

| Method                  | Signature                  | Description                                                                 |
|-------------------------|----------------------------|-----------------------------------------------------------------------------|
| `Init(provider, appId)` | `Result(String, String)`   | Starts a provider; `Ok` holds the provider id, `Err` the failure message     |
| `Update()`              | `Void()`                   | Pumps the active provider once; no effect when none is started               |
| `Shutdown()`            | `Void()`                   | Stops the provider, cancels pending requests, clears events; idempotent      |
| `PollEvent()`           | `Integer()`                | Dequeues the next event and returns its `EventKind`, or `EventKind.None`     |
| `HasProvider(name)`     | `Boolean(String)`          | Whether a provider id (case-insensitive) is compiled into this runtime       |
| `HasFeature(feature)`   | `Boolean(Integer)`         | Whether the active provider can serve a `Feature` right now                  |
| `IsDlcInstalled(dlcId)` | `Boolean(String)`          | Whether a DLC is owned and installed (id format is provider-defined)         |
| `RequestPlayerCount()`  | `Request()`                | Starts a non-blocking request for the number of players currently in game    |
| `Diagnostics()`         | `Seq()`                    | Up to 32 most recent non-fatal diagnostic messages, oldest first             |

### Properties

| Property          | Type                  | Description                                                              |
|-------------------|-----------------------|--------------------------------------------------------------------------|
| `IsAvailable`     | `Boolean` (read-only) | `TRUE` while a provider is started                                       |
| `Status`          | `Integer` (read-only) | A `Zanna.Services.Status` value                                          |
| `Provider`        | `String` (read-only)  | Active provider id (`"steam"`), or `""`                                  |
| `AppId`           | `String` (read-only)  | Application id reported by the provider, or `""`                         |
| `UserId`          | `String` (read-only)  | Signed-in user's id (decimal SteamID64 on Steam), or `""`                |
| `UserName`        | `String` (read-only)  | Signed-in user's display name, or `""`                                   |
| `Language`        | `String` (read-only)  | User's game language code (Steam: `"english"`, `"german"`, ...), or `""` |
| `IsLicensed`      | `Boolean` (read-only) | Whether the user holds a license for the running game                    |
| `IsOnline`        | `Boolean` (read-only) | Whether the platform client is connected to its online service           |
| `EventResultCode` | `Integer` (read-only) | Provider result code of the last polled event (Steam: `EResult`), or `0` |
| `EventText`       | `String` (read-only)  | Text payload of the last polled event, or `""`                           |
| `EventValue`      | `Integer` (read-only) | Integer payload of the last polled event, or `0`                         |
| `EventFlag`       | `Boolean` (read-only) | Boolean payload of the last polled event                                 |
| `DroppedEvents`   | `Integer` (read-only) | Events discarded because the 256-event queue was full                    |

### Behavior Notes

- **Status transitions.** `Status` starts as `NotStarted`. A successful `Init` makes it `Ok`. A
  failed `Init` leaves no provider started and records why (`LibraryNotFound`,
  `ClientNotRunning`, and so on). `Shutdown` returns it to `NotStarted`.
- **Repeated `Init`.** Starting the already-active provider again returns `Ok` without restarting
  it. Starting a different provider while one is active returns
  `Err("Services: provider 'steam' is already active; call Platform.Shutdown() before starting '<name>'")`
  and leaves `Status` unchanged. An unknown id returns
  `Err("Services: unknown provider '<name>' (available: steam)")` with `Status.UnknownProvider`.
- **Malformed ids trap.** Each provider validates its own id format. Steam app and DLC ids are
  decimal integers in `1..4294967295`; `Init("steam", "abc")` traps with
  `Services.Platform.Init: Steam app id 'abc' must be an integer in 1..4294967295`.
- **Events.** Platform callbacks become events during a pump. `PollEvent` copies the next event
  into the `Event*` properties; an empty queue clears them. The queue holds 256 events; when it is
  full the oldest is discarded, `DroppedEvents` counts it, and one diagnostic is recorded per
  overflow. See [EventKind](#zannaserviceseventkind) for each event's payload.
- **Features.** `HasFeature` can be `FALSE` while a provider is started, for example when an older
  redistributable lacks an interface. `Diagnostics()` then contains a message such as
  `Steam: interface SteamAPI_SteamUserStats_v013 unavailable; player counts disabled`.
- **Diagnostics** collect `Init` failures, disabled features, rejected callback payloads, and event
  overflow. They survive `Shutdown`, so a failed start can still be inspected afterwards.
- **Requests** never block. See [Zanna.Services.Request](#zannaservicesrequest).

### Zia Example

```zia
module PlayerCount;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Services as Services;

var pending: Any = null;

func startCount() {
    if Services.Platform.HasFeature(Services.Feature.PlayerCount) {
        pending = Services.Platform.RequestPlayerCount();
    }
}

// Call once per frame; the Canvas poll has already pumped the provider.
func checkCount() {
    if pending == null {
        return;
    }
    var request = pending as Services.Request;
    if request.IsDone {
        if request.Succeeded {
            Say(Fmt.Int(request.Value) + " players online");
        } else {
            Say("Player count unavailable: " + request.Error);
        }
        pending = null;
    }
}

func start() {
    var started = Services.Platform.Init("steam", "480");
    if started.IsErr {
        for message in Services.Platform.Diagnostics() {
            Say(message);
        }
        return;
    }
    startCount();
    Services.Platform.Update();
    checkCount();
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.Request

A non-blocking platform request.

**Type:** Instance class (returned by request methods such as `Platform.RequestPlayerCount`)

### Properties

| Property     | Type                  | Description                                                           |
|--------------|-----------------------|-----------------------------------------------------------------------|
| `Kind`       | `Integer` (read-only) | A `Zanna.Services.RequestKind` value                                  |
| `IsDone`     | `Boolean` (read-only) | `TRUE` once the request completed, successfully or not                |
| `Succeeded`  | `Boolean` (read-only) | `TRUE` when the request completed successfully                        |
| `ResultCode` | `Integer` (read-only) | Provider result code (Steam: `1` on success), or `0` when none exists |
| `Value`      | `Integer` (read-only) | Kind-specific result; the player count for `PlayerCount`              |
| `Error`      | `String` (read-only)  | Failure message, or `""`                                              |

### Behavior Notes

- A request completes during a later provider pump. Check `IsDone` on later frames instead of
  waiting; results only arrive while the main thread keeps pumping.
- A request that cannot start is returned already completed as failed, with messages such as
  `Services: no platform services provider is started` or
  `Services: too many pending requests (limit 64)`.
- `Platform.Shutdown` completes outstanding requests as failed with
  `Services: request cancelled by Platform.Shutdown()` and `ResultCode` `0`.

---

## Zanna.Services.Steam

Steam-only capabilities. Every query returns a neutral value unless Steam is the active provider;
`RestartAppIfNecessary` also works before `Platform.Init`.

**Type:** Static utility class

### Methods

| Method                           | Signature          | Description                                                                        |
|----------------------------------|--------------------|------------------------------------------------------------------------------------|
| `RestartAppIfNecessary(appId)`   | `Boolean(Integer)` | `TRUE` when Steam is relaunching the game through the client; exit immediately     |

### Properties

| Property        | Type                  | Description                                                                 |
|-----------------|-----------------------|-----------------------------------------------------------------------------|
| `IsActive`      | `Boolean` (read-only) | `TRUE` while Steam is the active provider                                   |
| `SteamId`       | `Integer` (read-only) | Signed-in user's SteamID64, or `0`                                          |
| `HardwareType`  | `Integer` (read-only) | A `Zanna.Services.SteamHardware` value; `Unknown` while Steam is not active |
| `IsUnderProton` | `Boolean` (read-only) | `TRUE` when running under Proton (SDK 1.65 redistributables only)           |
| `IsBigPicture`  | `Boolean` (read-only) | `TRUE` when Steam runs in Big Picture mode                                  |
| `LibraryPath`   | `String` (read-only)  | Path of the loaded `steam_api` redistributable, or `""`                     |

### Behavior Notes

- `RestartAppIfNecessary(appId)` returns `FALSE` when the game was launched by Steam, when a
  development `steam_appid.txt` file is present, or when the redistributable cannot be loaded (the
  reason goes to `Platform.Diagnostics()`). Ids outside `1..4294967295` trap. Use it in release
  builds, before `Platform.Init`.
- Redistributables older than Steamworks SDK 1.65 report only `SteamHardware.SteamDeck` or
  `SteamHardware.None`, and `IsUnderProton` is always `FALSE`. Use hardware values for defaults and
  analytics, not to gate features.

---

## Constants

All constant classes are static and readable from any thread. Values are stable.

### Zanna.Services.Status

| Name                  | Value | Meaning                                                             |
|-----------------------|-------|---------------------------------------------------------------------|
| `Ok`                  | 0     | A provider is started                                               |
| `NotStarted`          | 1     | Before `Init`, and after `Shutdown`                                 |
| `UnknownProvider`     | 2     | `Init` named a provider this runtime does not include               |
| `LibraryNotFound`     | 3     | The provider's redistributable library file does not exist         |
| `LibraryIncompatible` | 4     | The library failed to load or lacks a required export               |
| `UnsupportedPlatform` | 5     | The provider ships no redistributable for this OS and architecture  |
| `ClientNotRunning`    | 6     | The platform client (for example Steam) is not running              |
| `VersionMismatch`     | 7     | The platform client is older than the redistributable requires      |
| `InitFailed`          | 8     | Initialization failed for another reason                            |

### Zanna.Services.EventKind

| Name                      | Value | Payload                                                                          |
|---------------------------|-------|----------------------------------------------------------------------------------|
| `None`                    | 0     | The queue is empty                                                               |
| `ServiceConnected`        | 1     | —                                                                                |
| `ServiceDisconnected`     | 2     | `EventResultCode`: provider result                                               |
| `ConnectFailed`           | 3     | `EventResultCode`: provider result; `EventFlag`: still retrying                  |
| `OverlayChanged`          | 4     | `EventFlag`: overlay open; `EventValue`: `1` when the user opened or closed it   |
| `DlcInstalled`            | 5     | `EventText` and `EventValue`: the DLC id                                         |
| `LaunchParametersChanged` | 6     | —                                                                                |
| `ServiceShutdown`         | 7     | — (save and call `Platform.Shutdown()`)                                          |

### Zanna.Services.Feature

| Name          | Value | Members it covers                        |
|---------------|-------|------------------------------------------|
| `Identity`    | 1     | `UserId`, `UserName`, `IsOnline`         |
| `Licensing`   | 2     | `IsLicensed`, `IsDlcInstalled`           |
| `Language`    | 3     | `Language`                               |
| `PlayerCount` | 4     | `RequestPlayerCount`                     |

### Zanna.Services.RequestKind

| Name          | Value | Request                                  |
|---------------|-------|------------------------------------------|
| `PlayerCount` | 1     | `Platform.RequestPlayerCount()`          |

### Zanna.Services.SteamHardware

| Name           | Value | Meaning                                  |
|----------------|-------|------------------------------------------|
| `Unknown`      | -1    | Steam is not the active provider         |
| `None`         | 0     | Not Steam hardware                       |
| `SteamDeck`    | 1     | Steam Deck                               |
| `SteamMachine` | 2     | Steam Machine                            |
| `SteamFrame`   | 3     | Steam Frame                              |

---

## Shipping with the Steam Provider

### The redistributable

Download the Steamworks SDK from the Steamworks partner site and copy the library for each target
from its `redistributable_bin` folder next to your executable. Zanna loads it from exactly one
place and never searches system paths.

| Target        | SDK file                            | Location in your build                              |
|---------------|-------------------------------------|-----------------------------------------------------|
| Windows x64   | `win64/steam_api64.dll`             | Next to the `.exe`                                  |
| macOS         | `osx/libsteam_api.dylib` (universal) | `YourGame.app/Contents/MacOS/`                     |
| Linux x64     | `linux64/libsteam_api.so`           | Next to the executable                              |
| Linux arm64   | `linuxarm64/libsteam_api.so`        | Next to the executable (SDK 1.63 or newer)          |
| Windows arm64 | none                                | Unsupported; ship the x64 build                     |

Supported redistributables are Steamworks SDK **1.61 through 1.65**. A redistributable missing a
core export fails `Init` with `Status.LibraryIncompatible`; one missing a single interface starts
with that interface's features disabled (see `Platform.Diagnostics()`).

The SDK is licensed to you by Valve under the Steamworks SDK Access Agreement, which covers
shipping `redistributable_bin` files with your game. Zanna does not include or download them.

### App ids and development runs

- `Init("steam", appId)` sets the `SteamAppId` and `SteamGameId` environment variables when they are
  absent, so a `steam_appid.txt` file is not needed during development. Never ship
  `steam_appid.txt`. Values Steam sets when it launches the game are never overwritten.
- The Steam client must be running and signed in to an account that owns the app. Valve's
  Spacewar sample app id `480` is available to every account for experiments.
- Under `zanna run`, the executable directory is the toolchain's, so point the provider at a
  library explicitly:

  ```sh
  ZANNA_SERVICES_STEAM_LIBRARY=/path/to/sdk/redistributable_bin/osx/libsteam_api.dylib zanna run main.zia
  ```

  ```powershell
  $env:ZANNA_SERVICES_STEAM_LIBRARY = "C:\steamworks\redistributable_bin\win64\steam_api64.dll"
  zanna run main.zia
  ```

### Overlay and startup order

- Call `Platform.Init` before creating a `Canvas`, `Canvas3D`, or `World3D`. If a GPU-presented
  window already exists, `Diagnostics()` records
  `Steam: initialized after a GPU-presented window was created; the desktop overlay may not attach`.
- The desktop Steam overlay draws on GPU-presented frames only: `Canvas3D` and `World3D` windows
  receive it, but the 2D `Canvas` and GUI applications present through the CPU and do not.
- Pause single-player games while `EventKind.OverlayChanged` reports the overlay open.
- On macOS, builds signed with the hardened runtime (the default for Developer ID signing) need the
  entitlements `com.apple.security.cs.disable-library-validation` and
  `com.apple.security.cs.allow-dyld-environment-variables` to load `libsteam_api.dylib` and the
  overlay. Provide them with the `macos-entitlements` project directive. Steam games cannot use the
  App Sandbox entitlement.

### Shutting down

Call `Platform.Shutdown()` when the player quits. Native Windows executables exit without running
exit handlers, and platform libraries may already be tearing down during process exit, so the
runtime never shuts the provider down on its own.

---

## Testing Without Steam

- Games need no special handling: without a started provider every query is neutral, so the same
  code runs in development, CI, and non-Steam builds.
- To exercise the unavailable path deliberately, point `ZANNA_SERVICES_STEAM_LIBRARY` at a file that
  does not exist; `Init` then fails with `Status.LibraryNotFound`.
- Zanna's own tests run the Steam provider against from-scratch fake `steam_api` libraries built by
  the test tree (`src/tests/runtime/RTServicesFakeSteamApi.c`), on the VM and in native binaries.

---

## Adding a Provider

Providers are C tables compiled into the `zanna_rt_services` runtime component. To add a store:

1. Implement an `rt_services_provider` table (`src/runtime/services/rt_services_provider.h`):
   `start`, `pump`, `stop`, and whichever query callbacks the store supports.
2. List it in the provider registry in `src/runtime/services/rt_services.c`.
3. Optionally add a `Zanna.Services.<Provider>` extension class for store-only features.
4. Add tests against a fake library and document the provider on this page.

The neutral `Platform` surface, status codes, event kinds, and request objects stay unchanged. The
contract, including the policy for runtime-loaded redistributables, is
[ADR 0352](../adr/0352-platform-services-runtime-loaded-providers.md).
