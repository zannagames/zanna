---
status: active
audience: public
last-verified: 2026-09-15
---

# Platform Services

> Distribution-platform services behind one provider-neutral API: user identity, licensing, DLC,
> platform events, achievements (with icons and global unlock percentages), stats, leaderboards,
> rich presence, overlay control, on-screen keyboards, cloud files, recording timeline markers,
> action-based controller input, and Workshop content. Steam is the first provider.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Zanna.Services.Platform](#zannaservicesplatform)
- [Zanna.Services.Request](#zannaservicesrequest)
- [Zanna.Services.Achievements](#zannaservicesachievements)
- [Zanna.Services.Stats](#zannaservicesstats)
- [Zanna.Services.Leaderboards](#zannaservicesleaderboards)
- [Zanna.Services.Presence](#zannaservicespresence)
- [Zanna.Services.Overlay](#zannaservicesoverlay)
- [Zanna.Services.OnScreenKeyboard](#zannaservicesonscreenkeyboard)
- [Zanna.Services.Cloud](#zannaservicescloud)
- [Zanna.Services.Timeline](#zannaservicestimeline)
- [Zanna.Services.ActionInput](#zannaservicesactioninput)
- [Zanna.Services.Workshop](#zannaservicesworkshop)
- [Zanna.Services.WorkshopItem](#zannaservicesworkshopitem)
- [Zanna.Services.Steam](#zannaservicessteam)
- [Constants](#constants)
- [Shipping with the Steam Provider](#shipping-with-the-steam-provider)
- [Testing Without Steam](#testing-without-steam)
- [Adding a Provider](#adding-a-provider)

For exact signatures, see the generated [Services reference](../generated/runtime/services.md).
The design is recorded in
[ADR 0352](../adr/0352-platform-services-runtime-loaded-providers.md) (platform, requests, Steam
provider), [ADR 0353](../adr/0353-platform-services-player-features.md) (player features),
[ADR 0354](../adr/0354-store-depot-packaging.md) (store depot packaging),
[ADR 0362](../adr/0362-platform-services-timeline.md) (recording timeline),
[ADR 0363](../adr/0363-platform-services-app-details.md) (DLC list, build id, branch),
[ADR 0364](../adr/0364-platform-services-achievement-icons-and-percentages.md) (achievement icons
and global unlock percentages),
[ADR 0365](../adr/0365-platform-services-action-input.md) (action input), and
[ADR 0366](../adr/0366-platform-services-workshop.md) (Workshop).

---

## Overview

`Zanna.Services` connects a game to the store it is distributed through. The API is split in two:

- **Neutral classes** work the same for every provider: `Platform` (lifecycle, identity, licensing,
  events, requests), `Request`, the player features `Achievements`, `Stats`, `Leaderboards`,
  `Presence`, `Overlay`, `OnScreenKeyboard`, `Cloud`, `Timeline`, `ActionInput`, and
  `Workshop` (with its `WorkshopItem` objects), and the constant classes `Status`, `EventKind`,
  `Feature`, `RequestKind`, `LeaderboardScope`, `LeaderboardSort`, `LeaderboardDisplay`,
  `OverlayPage`, `NotificationPosition`, `TextInputMode`, `TimelineMode`, `TimelineClip`,
  `ControllerType`, `GlyphSize`, `WorkshopQuery`, `WorkshopList`, `WorkshopVisibility`, and
  `WorkshopUpdateStatus`.
- **Provider extension classes** expose what only one store offers. `Zanna.Services.Steam` and its
  `SteamHardware` constants are the first.

Providers are compiled into the runtime and selected by id when the game starts. Only `"steam"`
exists today; other stores are added behind the same neutral classes.

Key rules:

- **Absence is normal.** Without a started provider (no Steam client, no redistributable library,
  or a build shipped elsewhere) every query returns `""`, `0`, or `false`, requests complete as
  failed, and nothing traps. One build runs on Steam, on other stores, and during development.
- **No build dependency.** The Steam provider loads Valve's `steam_api` redistributable at run
  time from beside the executable. Nothing from the Steamworks SDK is needed to build Zanna or
  your game. You ship the redistributable with the game.
- **Main thread only.** Stateful members trap with
  `Services: <Class>.<Member> must be called on the main thread` when called from another thread.
  Constant classes can be read from any thread.
- **Automatic pumping.** While a provider is started, every `Canvas.Poll` and `Canvas3D.Poll` (and
  therefore `World3D.Update`) delivers pending platform callbacks. Loops without a canvas call
  `Platform.Update()` once per frame.
- **Errors are values.** `Platform.Init` and `Cloud.Read` return a `Zanna.Result`, members that
  change platform state return `false` when rejected, requests carry an `Error`, and non-fatal
  problems are kept in `Platform.Diagnostics()`.
- **Programming errors trap.** An empty identifier, a value outside its constant class, or an
  impossible leaderboard range traps whether or not a provider is started, so mistakes surface
  during development. Values computed while the game runs (stat values, progress) never trap.
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

IF Zanna.Services.Achievements.Unlock("ACH_WIN_ONE_GAME") THEN
    Zanna.Services.Stats.Store()
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
| `LaunchParameter(key)`  | `String(String)`           | A parameter the platform's launch URL passed, or `""`                        |
| `DlcIdAt(index)`        | `String(Integer)`          | Id of the DLC at `index`, or `""`                                            |
| `DlcNameAt(index)`      | `String(Integer)`          | Display name of the DLC at `index`, or `""`                                  |
| `DlcAvailableAt(index)` | `Boolean(Integer)`         | Whether the DLC at `index` can be bought now                                 |

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
| `LaunchCommandLine` | `String` (read-only) | Command line the platform's launch URL passed, or `""`                  |
| `DlcCount`        | `Integer` (read-only) | DLC the game defines, owned or not, or `0`                               |
| `BuildId`         | `Integer` (read-only) | Installed build id; `0` when the build did not come from the platform    |
| `BranchName`      | `String` (read-only)  | Branch of the installed build (Steam beta name), or `""` on the default branch |
| `EventResultCode` | `Integer` (read-only) | Provider result code of the last polled event (Steam: `EResult`), or `0` |
| `EventText`       | `String` (read-only)  | Text payload of the last polled event, or `""`                           |
| `EventValue`      | `Integer` (read-only) | Integer payload of the last polled event, or `0`                         |
| `EventTotal`      | `Integer` (read-only) | Total that a progress event's `EventValue` counts toward, or `0`         |
| `EventFlag`       | `Boolean` (read-only) | Boolean payload of the last polled event                                 |
| `DroppedEvents`   | `Integer` (read-only) | Events discarded because the 256-event queue was full                    |

### Behavior Notes

- **Status transitions.** `Status` starts as `NotStarted`. A successful `Init` makes it `Ok`. A
  failed `Init` leaves no provider started and records why (`LibraryNotFound`,
  `ClientNotRunning`, and so on). `Shutdown` returns it to `NotStarted`.
- **Steam not running.** `ClientNotRunning` means the Steam client is not running, including when
  it is not installed. A running client that nobody is signed in to reports `InitFailed` with
  Steam's reason in the `Err` message (for example `ConnectToGlobalUser failed.`). When `Init`
  fails, Valve's library also prints `[S_API]` lines to the console; they come from the
  redistributable and cannot be turned off.
- **Launch parameters.** When a player launches or rejoins the game through a platform URL, such as
  joining a friend through rich presence, the platform passes a command line and named
  parameters (Steam: `steam://run/<appid>//<command line>/` and
  `steam://run/<appid>//?team=boston&season=1972`). `LaunchCommandLine` and
  `LaunchParameter(key)` read them. This is not the operating system command line (use
  `Zanna.System.Environment` for that). If the game is already running, `EventKind.LaunchParametersChanged`
  fires; read both again. An empty key traps. On Steam, names starting with `@` are reserved and
  read as `""`, and the command line is read into a 4096-byte buffer.
- **DLC list and build.** `DlcCount` with `DlcIdAt`, `DlcNameAt`, and `DlcAvailableAt` lists every
  DLC the game defines (Steam reports at most 64). Pair it with `IsDlcInstalled(DlcIdAt(i))`
  for a store screen. Indexes outside the range return `""` and `FALSE`. `BuildId` and
  `BranchName` identify the installed build for support reports; a build Steam did not install
  reports `0` and `""`.
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
  or stripped redistributable lacks an interface or an export. `Diagnostics()` then contains a
  message such as
  `Steam: interface SteamAPI_SteamRemoteStorage_v016 unavailable; cloud storage disabled` or
  `Steam: export SteamAPI_ISteamUserStats_SetAchievement unavailable; achievements disabled`.
- **Diagnostics** collect `Init` failures, disabled features, rejected calls, rejected callback
  payloads, and event overflow. A message identical to the newest one is not repeated, so a call
  that fails every frame does not push older messages out. Diagnostics survive `Shutdown`, so a
  failed start can still be inspected afterwards.
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

**Type:** Instance class (returned by request methods such as `Platform.RequestPlayerCount`,
`Leaderboards.Download`, and `OnScreenKeyboard.RequestText`)

### Properties

| Property     | Type                  | Description                                                           |
|--------------|-----------------------|-----------------------------------------------------------------------|
| `Kind`       | `Integer` (read-only) | A `Zanna.Services.RequestKind` value                                  |
| `IsDone`     | `Boolean` (read-only) | `TRUE` once the request completed, successfully or not                |
| `Succeeded`  | `Boolean` (read-only) | `TRUE` when the request completed successfully                        |
| `ResultCode` | `Integer` (read-only) | Provider result code (Steam: `1` on success), or `0` when none exists |
| `Value`      | `Integer` (read-only) | Kind-specific integer result (see below)                              |
| `Flag`       | `Boolean` (read-only) | Kind-specific boolean result (see below)                              |
| `Text`       | `String` (read-only)  | Kind-specific text result (see below), or `""`                        |
| `Error`      | `String` (read-only)  | Failure message, or `""`                                              |
| `EntryCount` | `Integer` (read-only) | Leaderboard entries held (downloads only, at most 100), or `0`        |
| `DetailCount` | `Integer` (read-only) | Kind-specific integer details held (at most 8), or `0`               |
| `ItemCount`  | `Integer` (read-only) | Workshop items held by a completed Workshop query (at most 100), or `0` |

### Methods

| Method               | Signature          | Description                                                        |
|----------------------|--------------------|--------------------------------------------------------------------|
| `EntryRank(index)`   | `Integer(Integer)` | Global rank (from 1) of the entry at `index`                       |
| `EntryScore(index)`  | `Integer(Integer)` | Score of the entry at `index`                                      |
| `EntryUserId(index)` | `String(Integer)`  | Provider user id of the entry (decimal SteamID64 on Steam)         |
| `EntryUserName(index)` | `String(Integer)` | Display name of the entry's user, or `""` while it is unknown     |
| `Detail(index)`      | `Integer(Integer)` | Kind-specific integer detail at `index` (see below)               |
| `ItemAt(index)`      | `WorkshopItem(Integer)` | Workshop item at `index` of a completed Workshop query        |

### Results by kind

| `Kind`                | `Value`                      | `Flag`                     | `Text`              |
|-----------------------|------------------------------|----------------------------|---------------------|
| `PlayerCount`         | Players currently in game    | —                          | —                   |
| `LeaderboardFind`     | Entries on the board         | —                          | Leaderboard name    |
| `LeaderboardUpload`   | The player's new global rank | The stored score changed   | Leaderboard name    |
| `LeaderboardDownload` | Entries on the board         | —                          | Leaderboard name    |
| `TextInput`           | Length of the text in bytes  | —                          | The submitted text  |
| `TimelineEventRecording` | —                         | A recording covers the event | The event id      |
| `TimelinePhaseRecording` | Recorded milliseconds     | Anything was recorded      | The phase id        |
| `AchievementPercentages` | —                         | —                          | —                   |
| `WorkshopQuery`       | Matching items on every page | Answered from the local cache | —                |
| `WorkshopSubscribe`, `WorkshopUnsubscribe`, `WorkshopDelete` | — | —            | The item id         |
| `WorkshopCreate`, `WorkshopSubmit` | —               | The player must accept the Workshop agreement | The item id |

`TimelinePhaseRecording` also holds four details: `Detail(0)` recorded milliseconds, `Detail(1)`
longest clip milliseconds, `Detail(2)` clip count, and `Detail(3)` screenshot count. Other kinds
hold none.

### Behavior Notes

- A request completes during a later provider pump. Check `IsDone` on later frames instead of
  waiting; results only arrive while the main thread keeps pumping. Some requests span several
  platform calls (a leaderboard upload may look the board up first) and take an extra frame.
- A request that cannot start is returned already completed as failed, with messages such as
  `Services: no platform services provider is started` or
  `Services: too many pending requests (limit 64)`.
- `Platform.Shutdown` completes outstanding requests as failed with
  `Services: request cancelled by Platform.Shutdown()` and `ResultCode` `0`.
- Entry indexes outside `0..EntryCount-1` trap, for example
  `Services.Request.EntryRank: index 3 is outside 0..2`. Loop with `while i < request.EntryCount`.
  Detail indexes outside `0..DetailCount-1` trap the same way
  (`Services.Request.Detail: index 4 is outside 0..3`).
- `EntryUserName` asks the provider for the current name while it is still active, because
  platforms often learn the names of strangers a moment after the scores arrive: an entry that
  reads `""` right after the download usually has its name a few frames later. After `Shutdown`
  the name known at completion is returned.

---

## Zanna.Services.Achievements

Unlocks and inspects the game's achievements, addressed by the API id defined with the platform
(for Steam, in the Steamworks app admin).

**Type:** Static utility class

### Methods

| Method                                  | Signature                          | Description                                                        |
|-----------------------------------------|------------------------------------|--------------------------------------------------------------------|
| `Unlock(id)`                            | `Boolean(String)`                  | Unlocks locally; `Stats.Store()` commits it and shows the notification |
| `Clear(id)`                             | `Boolean(String)`                  | Locks the achievement again (testing)                              |
| `IsUnlocked(id)`                        | `Boolean(String)`                  | Whether the achievement is unlocked                                |
| `UnlockTime(id)`                        | `Integer(String)`                  | Unlock time in seconds since the Unix epoch, or `0`                |
| `IndicateProgress(id, current, maximum)`| `Boolean(String, Integer, Integer)`| Shows a progress notification without unlocking                    |
| `IdAt(index)`                           | `String(Integer)`                  | API id of the achievement at `index`, or `""` outside `0..Count-1`  |
| `DisplayName(id)`                       | `String(String)`                   | Localized name, or `""`                                            |
| `Description(id)`                       | `String(String)`                   | Localized description, or `""`                                     |
| `IsHidden(id)`                          | `Boolean(String)`                  | Whether the achievement stays hidden until unlocked                |
| `IconWidth(id)`                         | `Integer(String)`                  | Width of the icon for the current state, or `0` while it loads     |
| `IconHeight(id)`                        | `Integer(String)`                  | Height of the icon for the current state, or `0` while it loads    |
| `IconRgba(id)`                          | `Bytes(String)`                    | `IconWidth * IconHeight * 4` RGBA bytes, or empty `Bytes` while it loads |
| `RequestGlobalPercentages()`            | `Request()`                        | Downloads the share of players who unlocked each achievement       |
| `GlobalPercent(id)`                     | `Float(String)`                    | Share of players who unlocked the achievement (0 to 100), or `0`   |

### Properties

| Property | Type                  | Description                                  |
|----------|-----------------------|----------------------------------------------|
| `Count`  | `Integer` (read-only) | Number of achievements the app defines, or 0 |

### Behavior Notes

- `Unlock` only changes local state. Call `Stats.Store()` soon after an unlock (at the end of the
  game, for example) to commit it; the unlock notification appears then, and
  `EventKind.AchievementStored` with `EventFlag` `TRUE` confirms each stored unlock. Steam
  delivers the `AchievementStored` events of a commit before its `StatsStored` event, so handle
  events by kind rather than by position.
- `IndicateProgress` shows "10 / 40"-style notifications. It does not store progress (keep that
  in a stat) and does not unlock. `current` and `maximum` must satisfy
  `0 <= current <= maximum` with `maximum > 0`; Steam additionally rejects `0` and
  `current == maximum` and requires the achievement to still be locked.
- A rejected call returns `FALSE` and records a diagnostic such as
  `Steam: SetAchievement('ACH_WIN') failed; check that the achievement is defined for this app`.
- An empty id traps: `Services.Achievements.Unlock: achievement id must not be empty`.
- **Icons.** `IconWidth`, `IconHeight`, and `IconRgba` read the icon for the achievement's current
  state, so unlocking switches to the other icon. Platforms load icons on demand: the first read
  of an icon that is not loaded yet returns `0` (or empty `Bytes`) and starts the load, and
  `EventKind.AchievementIconReady` names the achievement when it finishes. Read the icon again
  then. `EventValue` is `0` when the achievement has no icon for that state; that report comes
  once per session, and later reads return `0` without asking again. Steam icons are 64 by 64
  pixels, rows top to bottom, ready for `Pixels.FromBytes(width, height, bytes)`. The Steam
  client keeps loaded icons, so later runs usually read them at once. Icons larger than 4096 by
  4096 pixels are refused with a diagnostic.
- **Global percentages.** `RequestGlobalPercentages` downloads every achievement's global unlock
  share at once; after it succeeds, `GlobalPercent(id)` returns the share (for example `3.25`
  for a rare achievement) until `Shutdown`. Before that, `GlobalPercent` returns `0` and records
  `Steam: global achievement percentages are not loaded; call Achievements.RequestGlobalPercentages first`.
  On Steam, a failed request with `ResultCode` `2` also means Steam has no percentages for the
  app yet; Spacewar (480) has none.

### Zia Example

```zia
module Achievements;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Services as Services;

var gamesWon: Integer = 0;

func onGameWon() {
    gamesWon = gamesWon + 1;
    Services.Stats.SetInt("GamesWon", gamesWon);
    if gamesWon == 1 {
        Services.Achievements.Unlock("ACH_WIN_ONE_GAME");
    } else if gamesWon < 10 {
        Services.Achievements.IndicateProgress("ACH_WIN_TEN_GAMES", gamesWon, 10);
    } else {
        Services.Achievements.Unlock("ACH_WIN_TEN_GAMES");
    }
    // Commit once per game, not per play: Steam rate-limits stores.
    Services.Stats.Store();
}

// Call once per frame after Canvas.Poll or Canvas3D.Poll.
func handleEvents() {
    var kind = Services.Platform.PollEvent();
    while kind != Services.EventKind.None {
        if kind == Services.EventKind.StatsStored && !Services.Platform.EventFlag {
            Say("Stats were not stored (result " + Fmt.Int(Services.Platform.EventResultCode) + ")");
        } else if kind == Services.EventKind.AchievementStored && Services.Platform.EventFlag {
            Say("Unlocked: " + Services.Achievements.DisplayName(Services.Platform.EventText));
        }
        kind = Services.Platform.PollEvent();
    }
}

func start() {
    var started = Services.Platform.Init("steam", "480");
    if started.IsOk {
        gamesWon = Services.Stats.GetInt("GamesWon");
        onGameWon();
        Services.Platform.Update();
        handleEvents();
    }
    Services.Platform.Shutdown();
}
```

### Zia Example: an achievement list

```zia
module AchievementScreen;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Time.Clock as Clock;
bind Zanna.Graphics.Pixels as Pixels;
bind Zanna.Services as Services;

// Saves the icon for the achievement's current state; returns false while it is still loading.
func saveIcon(id: String) -> Boolean {
    var width = Services.Achievements.IconWidth(id);
    if width == 0 {
        return false;
    }
    var icon = Pixels.FromBytes(width, Services.Achievements.IconHeight(id), Services.Achievements.IconRgba(id));
    Pixels.SavePng(icon, id + ".png");
    return true;
}

func start() {
    if Services.Platform.Init("steam", "480").IsErr {
        return;
    }
    var rates = Services.Achievements.RequestGlobalPercentages();
    var i = 0;
    while i < Services.Achievements.Count {
        saveIcon(Services.Achievements.IdAt(i));
        i = i + 1;
    }

    // A game pumps from its frame loop instead; this sketch pumps for three seconds.
    var until = Clock.NowMs() + 3000;
    while Clock.NowMs() < until {
        Services.Platform.Update();
        var kind = Services.Platform.PollEvent();
        while kind != Services.EventKind.None {
            if kind == Services.EventKind.AchievementIconReady && Services.Platform.EventValue == 1 {
                saveIcon(Services.Platform.EventText);
            }
            kind = Services.Platform.PollEvent();
        }
        Clock.Sleep(16);
    }

    i = 0;
    while i < Services.Achievements.Count {
        var id = Services.Achievements.IdAt(i);
        var line = Services.Achievements.DisplayName(id);
        if rates.Succeeded {
            line = line + ": " + Fmt.NumFixed(Services.Achievements.GlobalPercent(id), 1) + "% of players";
        }
        Say(line);
        i = i + 1;
    }
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.Stats

Reads, updates, and commits the player's stats, addressed by the API name defined with the
platform.

**Type:** Static utility class

### Methods

| Method                                   | Signature                        | Description                                                  |
|------------------------------------------|----------------------------------|--------------------------------------------------------------|
| `GetInt(name)`                           | `Integer(String)`                | Integer stat value, or `0` when unavailable                  |
| `SetInt(name, value)`                    | `Boolean(String, Integer)`       | Sets an integer stat locally                                 |
| `GetFloat(name)`                         | `Float(String)`                  | Floating-point stat value, or `0.0` when unavailable         |
| `SetFloat(name, value)`                  | `Boolean(String, Float)`         | Sets a floating-point stat locally                           |
| `UpdateAverageRate(name, count, seconds)`| `Boolean(String, Float, Float)`  | Adds one session to an average-rate stat                     |
| `Store()`                                | `Boolean()`                      | Commits changed stats and achievements to the platform       |
| `ResetAll(includeAchievements)`          | `Boolean(Boolean)`               | Resets every stat, and optionally every achievement (testing)|

### Behavior Notes

- **Commit with `Store`.** Changes stay local until `Store` sends them together with pending
  achievement unlocks. `Store` returning `TRUE` means the commit started; the result arrives as
  `EventKind.StatsStored` (`EventFlag` `TRUE` on success, `EventResultCode` the provider result).
  Steam also commits unsent changes when the game exits normally.
- **One commit, several events.** Steam often reports one `Store` twice: once when the client
  commits locally and again when its server confirms. A change to the same stat or achievement
  made between the two can be overwritten by the confirmation. For example, `Achievements.Clear`
  right after storing an unlock may be undone. Let a commit settle for a few seconds before
  changing the same values again.
- **Rate limits.** Steam limits how often `Store` may be called (on the order of minutes). Commit
  at natural break points such as the end of a game or season, never every frame.
- **Rejected values.** Steam integer stats are 32-bit and floating-point stats are single
  precision. `SetInt("Hits", 3000000000)` returns `FALSE` with
  `Steam: stat 'Hits' value 3000000000 is outside the int32 range`; non-finite floats return
  `FALSE` with `Services: Stats.SetFloat('<name>') rejected a non-finite value`. A stat of the wrong
  type, one the client may not write, or one outside its configured limits returns `FALSE` with a
  diagnostic naming the stat. An increment-only stat rejects a lower value the same way, so
  "set it back" never undoes an increase; only `ResetAll` lowers it again.
- **Reverted stats.** When the server rejects part of a commit, `StatsStored` carries result code
  `8`, Steam restores the stored values, and `Diagnostics()` records
  `Steam: StoreStats rejected one or more stats (EResult 8); Steam reverted them to the stored values`.
- **Average rates.** `UpdateAverageRate("FeetPerSecond", 5280.0, 120.0)` adds 5280 feet over a
  120-second session; the platform keeps the rolling average. The session length must be positive.

---

## Zanna.Services.Leaderboards

Finds leaderboards, uploads scores, and downloads ranked entries. Every member returns a
[Request](#zannaservicesrequest); boards are addressed by name.

**Type:** Static utility class

### Methods

| Method                                  | Signature                                   | Description                                                   |
|-----------------------------------------|---------------------------------------------|---------------------------------------------------------------|
| `Find(name)`                            | `Request(String)`                           | Checks that a board exists; `Value` holds its entry count      |
| `FindOrCreate(name, sort, display)`     | `Request(String, Integer, Integer)`         | Like `Find`, creating a missing board                          |
| `Upload(name, score, keepBest)`         | `Request(String, Integer, Boolean)`         | Stores the player's score; `Value` is the new rank, `Flag` whether it changed |
| `Download(name, scope, start, end)`     | `Request(String, Integer, Integer, Integer)`| Fetches up to 100 entries                                      |

### Behavior Notes

- **Scopes.** `LeaderboardScope.Global` downloads ranks `start..end` counted from 1 (for example
  `1, 10` for the top ten). `AroundUser` treats `start` and `end` as offsets from the player's own
  rank (`-4, 5` fetches the player with four entries above and five below). `Friends` fetches the
  player and their friends and ignores the range.
- **Range rules trap.** Global ranges must satisfy `1 <= start <= end`, every range must satisfy
  `start <= end`, and a range may span at most 100 entries:
  `Services.Leaderboards.Download: a download spans at most 100 entries (got 1..500)`. A
  `Friends` download with more than 100 entries keeps the first 100 and records a diagnostic.
- **Keep best.** `Upload(name, score, true)` keeps the player's better score, where "better"
  follows the board's sort order; `false` always replaces it. `Flag` tells you whether the stored
  score changed, so a lower score with `keepBest` still succeeds with `Flag` `FALSE`.
- **Creating boards.** Prefer defining boards with the platform (Steamworks app admin) and using
  `Find`. On Steam, a board created by `FindOrCreate` does not appear in the Steam community until
  its community name is set in the app admin. `FindOrCreate` suits boards created at run time,
  such as one per season.
- **Failures** complete the request with an `Error`, for example
  `Steam: leaderboard 'SEASON_1972' was not found`,
  `Steam: leaderboard score 3000000000 is outside the int32 range` (Steam scores are 32-bit), or
  `Steam: leaderboard name '<name>' is longer than 127 bytes`.
- **Names and handles.** The provider resolves a board name once per session; the first upload or
  download to a board may take one frame longer than later ones.

### Zia Example

```zia
module HomeRunBoard;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Services as Services;

var upload: Any = null;
var board: Any = null;

func submitSeason(homeRuns: Integer) {
    upload = Services.Leaderboards.Upload("SEASON_HOME_RUNS", homeRuns, true);
}

// Call once per frame after the canvas poll.
func updateBoard() {
    if upload != null {
        var sent = upload as Services.Request;
        if sent.IsDone {
            if sent.Succeeded && sent.Flag {
                Say("New personal best, global rank " + Fmt.Int(sent.Value));
            }
            upload = null;
            // Download after the upload so the board includes the new score.
            board = Services.Leaderboards.Download("SEASON_HOME_RUNS", Services.LeaderboardScope.AroundUser, -4, 5);
        }
    }
    if board != null {
        var entries = board as Services.Request;
        if entries.IsDone {
            if entries.Succeeded {
                var i = 0;
                while i < entries.EntryCount {
                    Say(Fmt.Int(entries.EntryRank(i)) + ". " + entries.EntryUserName(i) + " " + Fmt.Int(entries.EntryScore(i)));
                    i = i + 1;
                }
            } else {
                Say("Leaderboard unavailable: " + entries.Error);
            }
            board = null;
        }
    }
}

func start() {
    if Services.Platform.Init("steam", "480").IsOk {
        submitSeason(38);
        var frame = 0;
        while frame < 4 {
            Services.Platform.Update();
            updateBoard();
            frame = frame + 1;
        }
    }
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.Presence

Publishes what the player is doing to their friends.

**Type:** Static utility class

### Methods

| Method            | Signature                 | Description                                                     |
|-------------------|---------------------------|-----------------------------------------------------------------|
| `Set(key, value)` | `Boolean(String, String)` | Publishes one key; an empty `value` removes the key             |
| `Clear()`         | `Void()`                  | Removes every key                                               |

### Behavior Notes

- Keys are provider-defined. On Steam, `status` is plain text shown in some friends views, and
  `steam_display` names a localization token (`#StatusManaging`) configured for the app in
  Steamworks, whose substitutions come from further keys.
- Steam keys must be shorter than 64 bytes and values shorter than 256 bytes, with at most 30
  keys set; anything else returns `FALSE` with a diagnostic. An empty key traps.
- Call `Clear()` when the player leaves the activity the presence describes, for example on
  returning to the main menu.

---

## Zanna.Services.Overlay

Opens the platform's in-game overlay and positions its notifications.

**Type:** Static utility class

### Methods

| Method                                  | Signature                  | Description                                                    |
|-----------------------------------------|----------------------------|----------------------------------------------------------------|
| `Open(page)`                            | `Boolean(Integer)`         | Shows an `OverlayPage`                                         |
| `OpenWebPage(url, modal)`               | `Boolean(String, Boolean)` | Opens the overlay browser; `modal` hides other overlay windows |
| `OpenStore(productId, addToCart)`       | `Boolean(String, Boolean)` | Opens a store page, optionally adding the product to the cart  |
| `SetNotificationPosition(position)`     | `Boolean(Integer)`         | Chooses the `NotificationPosition` corner for notifications    |
| `SetNotificationInset(horizontal, vertical)` | `Boolean(Integer, Integer)` | Offsets notifications from that corner, in pixels        |

### Properties

| Property    | Type                  | Description                                            |
|-------------|-----------------------|--------------------------------------------------------|
| `IsEnabled` | `Boolean` (read-only) | Whether the overlay is attached and usable right now   |

### Behavior Notes

- The overlay can take a few seconds to attach after startup, so `IsEnabled` may start `FALSE`.
  When it stays `FALSE` (the user disabled it, or the game presents through the CPU), show your own
  screen instead of calling `Open`.
- Members return `TRUE` when the request reached the platform; the overlay reports opening and
  closing through `EventKind.OverlayChanged`.
- On Steam, `OpenStore` takes a decimal app id (a DLC's app id, for example); a malformed id traps
  with `Services.Overlay.OpenStore: Steam app id 'abc' must be an integer in 1..4294967295`.
  `OpenWebPage` needs an absolute URL including `https://`.
- The desktop overlay only draws over GPU-presented windows (`Canvas3D` and `World3D`); see
  [Overlay and startup order](#overlay-and-startup-order).

---

## Zanna.Services.OnScreenKeyboard

On-screen keyboards for devices without a physical keyboard, such as Steam Deck.

**Type:** Static utility class

### Methods

| Method                                            | Signature                                     | Description                                              |
|---------------------------------------------------|-----------------------------------------------|----------------------------------------------------------|
| `ShowFloating(mode, x, y, width, height)`         | `Boolean(Integer, Integer, Integer, Integer, Integer)` | Opens a floating keyboard beside the game's own text field |
| `DismissFloating()`                               | `Boolean()`                                   | Closes the floating keyboard                             |
| `RequestText(prompt, initialText, maxLength, mode)` | `Request(String, String, Integer, Integer)` | Opens full-screen text entry that returns the text       |

### Behavior Notes

- **Floating keyboard.** `ShowFloating` positions the keyboard so it does not cover the text field
  at `x, y, width, height` (window pixels). Its keys arrive as ordinary keyboard input, so the
  game's existing text field keeps working. `EventKind.TextInputDismissed` reports when the player
  closes it. Show it when a text field gains focus on a controller-only device.
- **Full-screen entry.** `RequestText` returns a `Request` of kind `TextInput` that completes when
  the player submits (`Succeeded`, `Text`) or cancels (fails with `Steam: text input was cancelled`).
  Only one can be pending at a time. `maxLength` is the most characters the player may enter; it
  must be in `1..4096` and traps otherwise. `Value` holds the submitted text's length in bytes,
  which exceeds the character count for non-ASCII text.
- **Availability.** On Steam both keyboards need Steam Deck or Big Picture mode: elsewhere
  `ShowFloating` returns `FALSE` with a diagnostic and `RequestText` fails. Players with a physical
  keyboard type normally, so treat `FALSE` as "no on-screen keyboard needed".
- **Modes.** `TextInputMode.Password` masks full-screen entry; the floating keyboard types into the
  game's field, which does its own masking, and uses the `SingleLine` layout.

### Zia Example

```zia
module ShellServices;

bind Zanna.Text.Fmt as Fmt;
bind Zanna.Services as Services;

var nameRequest: Any = null;

func onInningStart(team: String, inning: Integer) {
    Services.Presence.Set("status", "Managing " + team + ", inning " + Fmt.Int(inning));
}

func placeNotifications() {
    // Keep unlock notifications away from a scorebug in the top-left corner.
    Services.Overlay.SetNotificationPosition(Services.NotificationPosition.BottomRight);
    Services.Overlay.SetNotificationInset(24, 24);
}

func openAchievements() -> Boolean {
    if !Services.Overlay.IsEnabled {
        return false;
    }
    return Services.Overlay.Open(Services.OverlayPage.Achievements);
}

func focusNameField(x: Integer, y: Integer, width: Integer, height: Integer) {
    // False on a desktop without a controller shell, where the physical keyboard is used.
    Services.OnScreenKeyboard.ShowFloating(Services.TextInputMode.SingleLine, x, y, width, height);
}

func askForTeamName(current: String) {
    nameRequest = Services.OnScreenKeyboard.RequestText("Team name", current, 24, Services.TextInputMode.SingleLine);
}

// Returns the submitted name once, or "" while waiting or after a cancel.
func takeTeamName() -> String {
    if nameRequest == null {
        return "";
    }
    var request = nameRequest as Services.Request;
    if !request.IsDone {
        return "";
    }
    nameRequest = null;
    if request.Succeeded {
        return request.Text;
    }
    return "";
}
```

---

## Zanna.Services.Cloud

Per-user files that the platform synchronizes between the player's devices.

**Type:** Static utility class

### Methods

| Method              | Signature                | Description                                                        |
|---------------------|--------------------------|--------------------------------------------------------------------|
| `Write(name, data)` | `Boolean(String, Bytes)` | Creates or replaces a file                                         |
| `Read(name)`        | `Result(String)`         | `Ok` holds the file's `Bytes`, `Err` the failure message           |
| `Exists(name)`      | `Boolean(String)`        | Whether the file exists                                            |
| `Delete(name)`      | `Boolean(String)`        | Deletes the file locally and from the platform                     |
| `Size(name)`        | `Integer(String)`        | File size in bytes, or `0`                                         |
| `Timestamp(name)`   | `Integer(String)`        | Last write time in seconds since the Unix epoch, or `0`            |
| `Files()`           | `Seq()`                  | Names of the app's cloud files                                     |
| `BeginBatch()`      | `Boolean()`              | Starts grouping writes and deletes into one logical change         |
| `EndBatch()`        | `Boolean()`              | Ends the group                                                     |

### Properties

| Property         | Type                  | Description                                                 |
|------------------|-----------------------|-------------------------------------------------------------|
| `IsEnabled`      | `Boolean` (read-only) | Whether both the player's account and the app enable cloud storage |
| `QuotaTotal`     | `Integer` (read-only) | The app's cloud quota for this player, in bytes             |
| `QuotaAvailable` | `Integer` (read-only) | Bytes still available                                       |

### Behavior Notes

- **Local first.** Operations work on the platform's local copy and return immediately; the
  platform uploads changes on its own schedule (Steam syncs when the game exits).
- **Batches.** Wrap the writes and deletes of one save in `BeginBatch` and `EndBatch`, so the
  platform never synchronizes a save file without its index. Nested batches are rejected.
- **Limits.** Steam Cloud files are at most 100 MiB, and the byte and file-count quotas are set in
  Steamworks. A rejected write returns `FALSE` with a diagnostic naming the file.
- **Auto-Cloud alternative.** Steam can also synchronize files the game already writes under
  `Zanna.IO.Path.DataDir` without any code (Auto-Cloud, configured in Steamworks). Use `Cloud`
  when the game wants to decide what is synchronized, or needs cloud files on other stores.
- An empty file name traps, and so does `Write` with data that is not `Bytes`.

### Zia Example

```zia
module CloudSave;

bind Zanna.Terminal;
bind Zanna.Services as Services;
bind Zanna.Collections.Bytes as Bytes;

func saveLeague(json: String) -> Boolean {
    if !Services.Cloud.IsEnabled {
        return false;
    }
    Services.Cloud.BeginBatch();
    var saved = Services.Cloud.Write("league.json", Bytes.FromStr(json));
    Services.Cloud.EndBatch();
    return saved;
}

func loadLeague() -> String {
    var read = Services.Cloud.Read("league.json");
    if read.IsErr {
        Say("No cloud save: " + read.UnwrapErrStr());
        return "";
    }
    var data = read.Unwrap() as Bytes;
    return data.ToStr();
}

func start() {
    if Services.Platform.Init("steam", "480").IsOk {
        saveLeague("{\"year\":1972}");
        Say(loadLeague());
    }
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.Timeline

Marks the platform's background gameplay recording (Steam game recording) so players can find
and clip moments later.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|---|---|---|
| `SetGameMode(mode)` | `Boolean(Integer)` | Colors the timeline bar with a `TimelineMode` |
| `SetTooltip(text, offsetSeconds)` | `Boolean(String, Float)` | Describes the current state, such as the score |
| `ClearTooltip(offsetSeconds)` | `Boolean(Float)` | Removes the state description |
| `AddEvent(title, description, icon, priority, offsetSeconds, clip)` | `String(String, String, String, Integer, Float, Integer)` | Marks a moment; returns its event id, or `""` |
| `AddRangeEvent(title, description, icon, priority, offsetSeconds, durationSeconds, clip)` | `String(String, String, String, Integer, Float, Float, Integer)` | Marks a span that is already over; returns its event id, or `""` |
| `StartRangeEvent(title, description, icon, priority, offsetSeconds, clip)` | `String(String, String, String, Integer, Float, Integer)` | Starts a span; returns its event id, or `""` |
| `UpdateRangeEvent(eventId, title, description, icon, priority, clip)` | `Boolean(String, String, String, String, Integer, Integer)` | Changes an open span (`priority` `-1` keeps the current one) |
| `EndRangeEvent(eventId, offsetSeconds)` | `Boolean(String, Float)` | Closes an open span |
| `RemoveEvent(eventId)` | `Boolean(String)` | Deletes an event this process added |
| `RequestEventRecording(eventId)` | `Request(String)` | Asks whether the recording still covers an event |
| `StartPhase()` | `Boolean()` | Starts a phase (a game, a chapter), ending the current one |
| `EndPhase()` | `Boolean()` | Ends the current phase |
| `SetPhaseId(phaseId)` | `Boolean(String)` | Gives the current phase a persistent id |
| `AddPhaseTag(name, icon, group, priority)` | `Boolean(String, String, String, Integer)` | Tags the current phase, such as with the opponent |
| `SetPhaseAttribute(group, value, priority)` | `Boolean(String, String, Integer)` | Sets a text attribute, such as the final score |
| `RequestPhaseRecording(phaseId)` | `Request(String)` | Asks what the recording holds for a phase |
| `OpenOverlayToPhase(phaseId)` | `Boolean(String)` | Opens the overlay at a phase |
| `OpenOverlayToEvent(eventId)` | `Boolean(String)` | Opens the overlay at an event |

### Behavior Notes

- **Times.** Offsets are seconds relative to now, and negative values are in the past, so a
  marker for a play that ended two seconds ago uses `-2.0`. Non-finite offsets and negative
  durations return `FALSE` (or `""`) with a diagnostic. Steam accepts ranges of at most 600
  seconds.
- **Priorities and icons.** Priorities range over `0..1000`; higher values show more
  prominently. Icons name one of the app's uploaded timeline icons or a built-in Steam icon such as
  `steam_star`, `steam_flag`, or `steam_checkmark`; an empty icon is allowed.
- **Ids.** Event ids are provider text (decimal handles on Steam) valid for the running process.
  Phase ids are the game's own, such as `"season-1972-game-34"`, and Steam accepts up to 63 bytes.
- **Recording queries.** `RequestEventRecording` completes with `Flag` `TRUE` when a recording
  covers the event. `RequestPhaseRecording` completes with the recorded milliseconds in `Value`
  and four details (see [Results by kind](#results-by-kind)). Both report nothing recorded while
  the player has game recording turned off.
- **Traps.** Empty titles, tooltip texts, ids, tag names, and groups; unknown `TimelineMode` or
  `TimelineClip` values; and priorities outside their range trap whether or not a provider is
  started. On Steam, an event id that is not a decimal handle traps while Steam is active.

### Zia Example

```zia
module Broadcast;

bind Zanna.Services as Services;

var inning: String = "";

func onGameStart(gameId: String, opponent: String) {
    Services.Timeline.SetGameMode(Services.TimelineMode.Playing);
    Services.Timeline.StartPhase();
    Services.Timeline.SetPhaseId(gameId);
    Services.Timeline.AddPhaseTag(opponent, "steam_flag", "Opponent", 100);
}

func onHomeRun(batter: String, runs: Integer) {
    Services.Timeline.AddEvent("Home run", batter + " drives in " + Zanna.Text.Fmt.Int(runs),
                               "steam_star", 900, -2.0, Services.TimelineClip.Featured);
}

func onScoreChanged(score: String) {
    Services.Timeline.SetTooltip(score, 0.0);
}

func onGameOver(finalScore: String) {
    Services.Timeline.SetPhaseAttribute("Final score", finalScore, 100);
    Services.Timeline.EndPhase();
    Services.Timeline.SetGameMode(Services.TimelineMode.Menus);
}

func start() {
    if Services.Platform.Init("steam", "480").IsOk {
        onGameStart("season-1972-game-34", "Chicago");
        onHomeRun("Ortiz", 2);
        onScoreChanged("Top 9th, 3-2");
        onGameOver("3-2");
    }
    Services.Platform.Shutdown();
}
```

## Zanna.Services.ActionInput

Reads controllers through the platform's action system (Steam Input). The game names what the
player does in an action manifest, such as `swing` or `aim`, and the player binds those actions to
any controller in the platform's own interface.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|---|---|---|
| `Start(manifestPath)` | `Boolean(String)` | Starts action input with an action manifest file, or `""` for the configuration published with the platform |
| `Stop()` | `Boolean()` | Stops action input |
| `ControllerIdAt(index)` | `String(Integer)` | Id of the connected controller at `index`, or `""` outside `0..ControllerCount-1` |
| `ControllerType(controllerId)` | `Integer(String)` | A `ControllerType` value |
| `GamepadIndex(controllerId)` | `Integer(String)` | Gamepad slot the controller emulates, or `-1` |
| `ActivateActionSet(controllerId, actionSet)` | `Boolean(String, String)` | Selects the actions in use, such as `batting` or `menu` |
| `ActivateLayer(controllerId, layer)` | `Boolean(String, String)` | Adds a layer of actions on top of the active set |
| `DeactivateLayer(controllerId, layer)` | `Boolean(String, String)` | Removes a layer |
| `DeactivateAllLayers(controllerId)` | `Boolean(String)` | Removes every layer |
| `IsPressed(controllerId, action)` | `Boolean(String, String)` | Whether a digital action is held |
| `AnalogX(controllerId, action)` | `Float(String, String)` | Horizontal value of an analog action |
| `AnalogY(controllerId, action)` | `Float(String, String)` | Vertical value of an analog action |
| `IsActionActive(controllerId, action)` | `Boolean(String, String)` | Whether an action is available in the active set and layers |
| `ActionLabel(action)` | `String(String)` | The action's localized name from the manifest |
| `OriginCount(controllerId, actionSet, action)` | `Integer(String, String, String)` | Physical inputs bound to the action (at most 8) |
| `OriginAt(controllerId, actionSet, action, index)` | `Integer(String, String, String, Integer)` | Origin id of one bound input, or `0` |
| `OriginLabel(origin)` | `String(Integer)` | Localized name of an input, such as `A Button` |
| `OriginGlyphPath(origin, size)` | `String(Integer, Integer)` | PNG file showing the input at a `GlyphSize` |
| `Vibrate(controllerId, left, right)` | `Boolean(String, Float, Float)` | Runs the rumble motors at strengths `0` to `1`; `0, 0` stops them |
| `SetLedColor(controllerId, red, green, blue)` | `Boolean(String, Integer, Integer, Integer)` | Sets the controller light |
| `ResetLedColor(controllerId)` | `Boolean(String)` | Restores the light color the player chose |
| `ShowBindingPanel(controllerId)` | `Boolean(String)` | Opens the platform's binding screen |

### Properties

| Property | Type | Description |
|---|---|---|
| `IsStarted` | `Boolean` (read-only) | `TRUE` while action input runs |
| `ControllerCount` | `Integer` (read-only) | Connected controllers (at most 16), updated by each platform pump |

### Behavior Notes

- **The manifest.** An action manifest lists action sets (with `Button`, `StickPadGyro`, and
  `AnalogTrigger` actions), layers, and localized names; Steamworks documents the format as the
  Steam Input action manifest. Pass a file path during development and `""` once the
  configuration is published with the game in Steamworks. Relative paths resolve against the
  working directory, and a missing file makes `Start` return `FALSE` with
  `Services: ActionInput.Start found no action manifest at '<path>'`.
- **When Steam refuses the manifest.** Steam accepts a manifest only after a controller has been
  used with the app in the current Steam session. Until then `Start` still starts action input
  (`IsStarted` is `TRUE`), returns `FALSE`, and records why; the manifest is applied as soon as a
  controller connects, before `EventKind.ControllerConnected` is reported. Starting takes about a
  second while Steam waits.
- **Controller ids.** Ids are provider text (decimal handles on Steam) that stay the same when a
  controller reconnects. An empty id means every connected controller: activation also reaches
  controllers connected later, `IsPressed` and `IsActionActive` ask whether any controller
  qualifies, `AnalogX` and `AnalogY` read the controller whose vector is longest (so both axes come
  from the same controller), `Vibrate` and the light members reach each controller, and the
  other members use the first controller. On Steam a malformed id traps, for example
  `Services.ActionInput.IsPressed: Steam controller id 'abc' must be an integer in 1..18446744073709551614`.
- **Reading actions.** Actions outside the active set and layers read as not pressed and `0`.
  Sticks report `-1..1`; mouse-like inputs such as trackpads report deltas. An action the manifest
  does not define records a diagnostic such as
  `Services: ActionInput.IsPressed found no digital action 'jump' in the action manifest`, and an
  unknown set records `Steam: action set 'fielding' is not in the action manifest`.
- **Events.** While action input runs, `ControllerConnected` and `ControllerDisconnected` carry
  the controller id in `EventText`, and `ControllerConfigured` reports that a controller's
  bindings loaded (`EventFlag` is `TRUE` when they bind actions, `EventValue` holds the binding
  revision). Rebuild button prompts on `ControllerConfigured`.
- **Glyphs.** Origin ids are provider values; `OriginLabel` and `OriginGlyphPath` need no
  controller. Steam's glyphs live in the Steam installation and load with `Pixels.LoadPng`;
  outside Windows the binding returns them with `/` separators.
- **Before `Start`.** Members return `FALSE`, `0`, `-1`, or `""`, and record
  `Services: ActionInput.<Member> needs ActionInput.Start first`.
- **Traps.** Empty action, set, and layer names, negative origins, unknown `GlyphSize` values,
  strengths outside `0..1`, and color components outside `0..255` trap whether or not a
  provider is started.

### Zia Example

```zia
module Batting;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Time.Clock as Clock;
bind Zanna.Services as Services;

var prompt: String = "Swing";
var swinging: Boolean = false;

// Follow the player's bindings: "Press A Button to swing" on Xbox, "X Button" on PlayStation.
func updatePrompt() {
    var origin = Services.ActionInput.OriginAt("", "batting", "swing", 0);
    if origin != 0 {
        prompt = "Press " + Services.ActionInput.OriginLabel(origin) + " to swing";
        // OriginGlyphPath(origin, GlyphSize.Medium) names a PNG of the same button.
    }
}

func handleEvents() {
    var kind = Services.Platform.PollEvent();
    while kind != Services.EventKind.None {
        if kind == Services.EventKind.ControllerConfigured {
            updatePrompt();
        }
        kind = Services.Platform.PollEvent();
    }
}

func start() {
    if Services.Platform.Init("steam", "480").IsErr {
        return;
    }
    if !Services.ActionInput.Start("input/steam_input_manifest.vdf") {
        Say("Action manifest not applied yet: see Platform.Diagnostics()");
    }
    // An empty controller id also reaches controllers connected later.
    Services.ActionInput.ActivateActionSet("", "batting");
    updatePrompt();
    Say(prompt);

    // A game pumps from its frame loop (Canvas.Poll); this sketch pumps for ten seconds.
    var until = Clock.NowMs() + 10000;
    while Clock.NowMs() < until {
        Services.Platform.Update();
        handleEvents();
        var pressed = Services.ActionInput.IsPressed("", "swing");
        if pressed && !swinging {
            var aimX = Services.ActionInput.AnalogX("", "aim");
            Say("Swing, aiming " + Fmt.NumFixed(aimX, 2));
            Services.ActionInput.Vibrate("", 0.6, 0.3);
        } else if !pressed && swinging {
            Services.ActionInput.Vibrate("", 0.0, 0.0);
        }
        swinging = pressed;
        Clock.Sleep(16);
    }
    Services.ActionInput.Stop();
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.Workshop

Lists, installs, browses, and publishes the game's user-generated content (Steam Workshop). Items
are addressed by provider-defined ids (decimal item numbers on Steam).

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|---|---|---|
| `SubscribedIdAt(index)` | `String(Integer)` | Id of the subscribed item at `index`, or `""` |
| `IsSubscribed(itemId)` | `Boolean(String)` | The player is subscribed to the item |
| `IsInstalled(itemId)` | `Boolean(String)` | The item's files are on disk (possibly out of date) |
| `NeedsUpdate(itemId)` | `Boolean(String)` | The item still needs a download or an update |
| `IsDownloading(itemId)` | `Boolean(String)` | A download is running or queued |
| `InstallFolder(itemId)` | `String(String)` | Folder holding an installed item's files, or `""` |
| `InstallSize(itemId)` | `Integer(String)` | Installed size in bytes, or `0` |
| `InstallTime(itemId)` | `Integer(String)` | When the item was installed or updated (Unix seconds), or `0` |
| `DownloadedBytes(itemId)` | `Integer(String)` | Bytes of the current download that arrived |
| `DownloadTotalBytes(itemId)` | `Integer(String)` | Size of the current download |
| `Download(itemId, highPriority)` | `Boolean(String, Boolean)` | Downloads or updates the item |
| `Subscribe(itemId)` | `Request(String)` | Subscribes the player to the item |
| `Unsubscribe(itemId)` | `Request(String)` | Unsubscribes the player |
| `Query(order, page, requiredTags, searchText)` | `Request(Integer, Integer, String, String)` | One page (from 1) of the game's items in a `WorkshopQuery` order |
| `QueryUser(list, page)` | `Request(Integer, Integer)` | One page of a `WorkshopList` of the player's |
| `QueryItems(itemIds)` | `Request(String)` | Details of specific items (comma-separated ids) |
| `CreateItem()` | `Request()` | Creates an empty item owned by the player |
| `StartUpdate(itemId)` | `String(String)` | Begins an update of the player's item; returns the update id, or `""` |
| `SetTitle(updateId, title)` | `Boolean(String, String)` | Sets the title |
| `SetDescription(updateId, description)` | `Boolean(String, String)` | Sets the description |
| `SetMetadata(updateId, metadata)` | `Boolean(String, String)` | Stores game-defined text with the item |
| `SetTags(updateId, tags)` | `Boolean(String, String)` | Replaces the tags (comma-separated) |
| `SetVisibility(updateId, visibility)` | `Boolean(String, Integer)` | Sets a `WorkshopVisibility` |
| `SetContent(updateId, folder)` | `Boolean(String, String)` | Uploads a folder's files as the item's content |
| `SetPreview(updateId, file)` | `Boolean(String, String)` | Sets the preview image |
| `SubmitUpdate(updateId, changeNote)` | `Request(String, String)` | Sends the update |
| `UpdateStatus(updateId)` | `Integer(String)` | A `WorkshopUpdateStatus` for a submitted update |
| `UpdateProgress(updateId)` | `Float(String)` | Progress of the current upload stage, `0` to `1` |
| `DeleteItem(itemId)` | `Request(String)` | Deletes the player's item |

### Properties

| Property | Type | Description |
|---|---|---|
| `SubscribedCount` | `Integer` (read-only) | Items the player subscribed to |

### Behavior Notes

- **Using subscribed items.** Steam downloads subscribed items in the background. Read an item's
  files from `InstallFolder` once `IsInstalled` is `TRUE`; call `Download(itemId, TRUE)` for one
  that `NeedsUpdate`, and watch for `EventKind.WorkshopItemInstalled` (EventText: the item id)
  and `EventKind.WorkshopItemDownloaded` (EventFlag: success, EventResultCode: the provider
  result). `EventKind.WorkshopSubscriptionChanged` reports subscriptions made in the overlay or
  on the website (EventFlag `TRUE` for a subscription).
- **Queries.** A query request completes with `ItemCount` items read with `Request.ItemAt(i)` as
  `WorkshopItem` objects, `Value` holding the number of matching items on every page (pages hold
  up to 50 items on Steam), and `Flag` set when Steam answered from its local cache. Queries return
  ready-to-use items made for the running app. `requiredTags` keeps items carrying every listed
  tag, and `searchText` keeps items whose title or description matches; use
  `WorkshopQuery.TextSearch` to rank by the match. `QueryItems` takes at most 50 ids on Steam.
- **Publishing.** `CreateItem` completes with the new item id in `Text`. `StartUpdate` then
  returns an update id for the setters, and `SubmitUpdate` completes with the item id in `Text`.
  When `Flag` is `TRUE` on either request, the player must accept the Steam Workshop agreement
  before the item becomes visible; open `steam://url/CommunityFilePage/<itemId>` with
  `Overlay.OpenWebPage` to show it. Content folders and preview files resolve against the working
  directory, and a missing one returns `FALSE` with a diagnostic such as
  `Services: Workshop.SetContent found no folder at '/path'`. Only the player's own items can be
  updated or deleted.
- **Failures.** A request Steam refuses fails with the provider result, for example
  `Steam: SubscribeItem('4000000000') failed (EResult 9)`.
- **Ids.** On Steam, item ids must be integers in `1..18446744073709551615` and update ids in
  `0..18446744073709551614`; anything else traps while Steam is active, such as
  `Services.Workshop.IsInstalled: Steam Workshop item id 'abc' must be an integer in 1..18446744073709551615`.
- **Traps.** Empty ids, unknown constants, pages below 1, and more than 100 ids in `QueryItems`
  trap whether or not a provider is started.

### Zia Example

```zia
module Leagues;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Time.Clock as Clock;
bind Zanna.IO.Path as Path;
bind Zanna.Services as Services;

// Load every custom league the player subscribed to; download the ones that are not ready.
func loadSubscribedLeagues() {
    var i = 0;
    while i < Services.Workshop.SubscribedCount {
        var id = Services.Workshop.SubscribedIdAt(i);
        if Services.Workshop.IsInstalled(id) && !Services.Workshop.NeedsUpdate(id) {
            Say("League files in " + Path.Join(Services.Workshop.InstallFolder(id), "league.json"));
        } else {
            Services.Workshop.Download(id, true);
        }
        i = i + 1;
    }
}

// Publish a league folder the player built; returns once Steam has the item.
func publishLeague(folder: String, title: String) {
    var created = Services.Workshop.CreateItem();
    while !created.IsDone {
        Services.Platform.Update();
        Clock.Sleep(16);
    }
    if !created.Succeeded {
        Say("Could not create the item: " + created.Error);
        return;
    }
    var update = Services.Workshop.StartUpdate(created.Text);
    Services.Workshop.SetTitle(update, title);
    Services.Workshop.SetTags(update, "league");
    Services.Workshop.SetContent(update, folder);
    Services.Workshop.SetVisibility(update, Services.WorkshopVisibility.Public);
    var submitted = Services.Workshop.SubmitUpdate(update, "First upload");
    while !submitted.IsDone {
        Services.Platform.Update();
        Say("Uploading " + Fmt.NumFixed(Services.Workshop.UpdateProgress(update) * 100.0, 0) + "%");
        Clock.Sleep(250);
    }
    if submitted.Succeeded && submitted.Flag {
        // The item stays hidden until the player accepts the Workshop agreement.
        Services.Overlay.OpenWebPage("steam://url/CommunityFilePage/" + submitted.Text, false);
    }
}

func start() {
    if Services.Platform.Init("steam", "480").IsErr {
        return;
    }
    loadSubscribedLeagues();

    var popular = Services.Workshop.Query(Services.WorkshopQuery.Popular, 1, "league", "");
    while !popular.IsDone {
        Services.Platform.Update();
        Clock.Sleep(16);
    }
    var i = 0;
    while i < popular.ItemCount {
        var item = popular.ItemAt(i);
        Say(item.Title + " (" + Fmt.Int(item.VotesUp) + " votes up)");
        i = i + 1;
    }
    Services.Platform.Shutdown();
}
```

---

## Zanna.Services.WorkshopItem

One Workshop item returned by a query. Items keep their values after the request is released.

**Type:** Instance class (returned by `Request.ItemAt`)

### Properties

| Property | Type | Description |
|---|---|---|
| `Id` | `String` (read-only) | Item id |
| `Title` | `String` (read-only) | Title |
| `Description` | `String` (read-only) | Description |
| `OwnerId` | `String` (read-only) | The author's user id (decimal SteamID64 on Steam) |
| `Tags` | `String` (read-only) | Comma-separated tags |
| `PreviewUrl` | `String` (read-only) | URL of the preview image, or `""` |
| `Metadata` | `String` (read-only) | Game-defined text set with `Workshop.SetMetadata`, or `""` |
| `Created` | `Integer` (read-only) | Creation time in Unix seconds |
| `Updated` | `Integer` (read-only) | Last update time in Unix seconds |
| `Visibility` | `Integer` (read-only) | A `WorkshopVisibility` value |
| `VotesUp` | `Integer` (read-only) | Up votes |
| `VotesDown` | `Integer` (read-only) | Down votes |
| `Size` | `Integer` (read-only) | Content size in bytes |
| `Score` | `Float` (read-only) | Vote score from `0` to `1` |

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
  builds, before `Platform.Init`: `Init` sets `SteamAppId` for the process, and from then on Steam
  treats the game as launched through the client and `RestartAppIfNecessary` returns `FALSE`.
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
| `ClientNotRunning`    | 6     | The platform client (for example Steam) is not running or not installed |
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
| `LaunchParametersChanged` | 6     | — (read `LaunchCommandLine` and `LaunchParameter` again)                         |
| `ServiceShutdown`         | 7     | — (save and call `Platform.Shutdown()`)                                          |
| `StatsStored`             | 8     | `EventFlag`: stored successfully; `EventResultCode`: provider result             |
| `AchievementStored`       | 9     | `EventText`: achievement id; `EventFlag`: unlocked; `EventValue` of `EventTotal`: progress |
| `TextInputDismissed`      | 10    | — (the floating keyboard closed)                                                 |
| `AchievementIconReady`    | 11    | `EventText`: achievement id; `EventFlag`: unlocked icon; `EventValue`: `1` when the icon exists |
| `ControllerConnected`     | 12    | `EventText`: controller id (while `ActionInput` runs)                            |
| `ControllerDisconnected`  | 13    | `EventText`: controller id                                                       |
| `ControllerConfigured`    | 14    | `EventText`: controller id; `EventFlag`: bindings use actions; `EventValue`: binding revision |
| `WorkshopItemInstalled`   | 15    | `EventText`: item id                                                             |
| `WorkshopItemDownloaded`  | 16    | `EventText`: item id; `EventFlag`: succeeded; `EventResultCode`: provider result |
| `WorkshopSubscriptionChanged` | 17 | `EventText`: item id; `EventFlag`: subscribed                                  |

### Zanna.Services.Feature

| Name           | Value | Members it covers                        |
|----------------|-------|------------------------------------------|
| `Identity`     | 1     | `UserId`, `UserName`, `IsOnline`         |
| `Licensing`    | 2     | `IsLicensed`, `IsDlcInstalled`           |
| `Language`     | 3     | `Language`                               |
| `PlayerCount`  | 4     | `RequestPlayerCount`                     |
| `Achievements` | 5     | `Zanna.Services.Achievements`            |
| `Stats`        | 6     | `Zanna.Services.Stats`                   |
| `Leaderboards` | 7     | `Zanna.Services.Leaderboards`            |
| `Presence`     | 8     | `Zanna.Services.Presence`                |
| `Overlay`      | 9     | `Zanna.Services.Overlay`                 |
| `TextInput`    | 10    | `Zanna.Services.OnScreenKeyboard`        |
| `Cloud`        | 11    | `Zanna.Services.Cloud`                   |
| `LaunchParameters` | 12 | `LaunchCommandLine`, `LaunchParameter`  |
| `Timeline`     | 13    | `Zanna.Services.Timeline`                |
| `AppDetails`   | 14    | `DlcCount`, `DlcIdAt`, `DlcNameAt`, `DlcAvailableAt`, `BuildId`, `BranchName` |
| `AchievementIcons` | 15 | `Achievements.IconWidth`, `IconHeight`, `IconRgba`                        |
| `AchievementPercentages` | 16 | `Achievements.RequestGlobalPercentages`, `GlobalPercent`          |
| `ActionInput`  | 17    | `Zanna.Services.ActionInput`             |
| `Workshop`     | 18    | `Zanna.Services.Workshop`                |

### Zanna.Services.RequestKind

| Name                  | Value | Request                                      |
|-----------------------|-------|----------------------------------------------|
| `PlayerCount`         | 1     | `Platform.RequestPlayerCount()`              |
| `LeaderboardFind`     | 2     | `Leaderboards.Find`, `Leaderboards.FindOrCreate` |
| `LeaderboardUpload`   | 3     | `Leaderboards.Upload`                        |
| `LeaderboardDownload` | 4     | `Leaderboards.Download`                      |
| `TextInput`           | 5     | `OnScreenKeyboard.RequestText`               |
| `TimelineEventRecording` | 6  | `Timeline.RequestEventRecording`             |
| `TimelinePhaseRecording` | 7  | `Timeline.RequestPhaseRecording`             |
| `AchievementPercentages` | 8  | `Achievements.RequestGlobalPercentages`      |
| `WorkshopQuery`       | 9     | `Workshop.Query`, `QueryUser`, `QueryItems`  |
| `WorkshopSubscribe`   | 10    | `Workshop.Subscribe`                         |
| `WorkshopUnsubscribe` | 11    | `Workshop.Unsubscribe`                       |
| `WorkshopCreate`      | 12    | `Workshop.CreateItem`                        |
| `WorkshopSubmit`      | 13    | `Workshop.SubmitUpdate`                      |
| `WorkshopDelete`      | 14    | `Workshop.DeleteItem`                        |

### Zanna.Services.TimelineMode

| Name            | Value | Meaning                                                 |
|-----------------|-------|---------------------------------------------------------|
| `Playing`       | 1     | The player is playing                                   |
| `Staging`       | 2     | Play is being set up (a lobby, a lineup screen)         |
| `Menus`         | 3     | The player is in menus                                  |
| `LoadingScreen` | 4     | A loading screen is shown                               |

### Zanna.Services.TimelineClip

| Name       | Value | Meaning                                                  |
|------------|-------|----------------------------------------------------------|
| `None`     | 1     | Never suggest the event as a clip                        |
| `Standard` | 2     | May suggest the event as a clip                          |
| `Featured` | 3     | Suggest the event as a clip ahead of standard events     |

### Zanna.Services.ControllerType

| Name                       | Value | Controller                                   |
|----------------------------|-------|----------------------------------------------|
| `Unknown`                  | 0     | Unknown                                      |
| `SteamController`          | 1     | Steam Controller (2015)                      |
| `Xbox360`                  | 2     | Xbox 360 controller                          |
| `XboxOne`                  | 3     | Xbox One or Xbox Series controller           |
| `GenericGamepad`           | 4     | Generic (DirectInput) gamepad                |
| `PlayStation4`             | 5     | PlayStation 4 controller                     |
| `AppleMfi`                 | 6     | Apple MFi controller                         |
| `Android`                  | 7     | Android controller                           |
| `SwitchJoyConPair`         | 8     | A pair of Nintendo Switch Joy-Cons           |
| `SwitchJoyConSingle`       | 9     | A single Nintendo Switch Joy-Con             |
| `SwitchPro`                | 10    | Nintendo Switch Pro controller               |
| `MobileTouch`              | 11    | On-screen touch controller (Steam Link)      |
| `PlayStation3`             | 12    | PlayStation 3 controller                     |
| `PlayStation5`             | 13    | PlayStation 5 controller                     |
| `SteamDeck`                | 14    | Steam Deck built-in controls                 |
| `SteamOSHandheld`          | 15    | Built-in controls of another SteamOS handheld |
| `Switch2Pro`               | 16    | Nintendo Switch 2 Pro controller             |
| `SteamController2026`      | 17    | Steam Controller (2026)                      |
| `SteamFrameControllerPair` | 18    | Steam Frame controller pair                  |

### Zanna.Services.GlyphSize

| Name     | Value | Size on Steam       |
|----------|-------|---------------------|
| `Small`  | 0     | 32 by 32 pixels     |
| `Medium` | 1     | 128 by 128 pixels   |
| `Large`  | 2     | 256 by 256 pixels   |

### Zanna.Services.WorkshopQuery

| Name              | Value | Order                                         |
|-------------------|-------|-----------------------------------------------|
| `Popular`         | 1     | By votes                                      |
| `Newest`          | 2     | By publication date, newest first             |
| `Trending`        | 3     | By recent votes                               |
| `MostSubscribed`  | 4     | By unique subscriptions                       |
| `RecentlyUpdated` | 5     | By last update, newest first                  |
| `TextSearch`      | 6     | By how well items match the search text       |

### Zanna.Services.WorkshopList

| Name         | Value | The player's items                   |
|--------------|-------|--------------------------------------|
| `Published`  | 1     | Items the player published           |
| `Subscribed` | 2     | Items the player subscribed to       |
| `Favorited`  | 3     | Items the player marked as favorites |
| `VotedUp`    | 4     | Items the player voted up            |
| `Played`     | 5     | Items the player used                |

### Zanna.Services.WorkshopVisibility

| Name          | Value | Visible to                          |
|---------------|-------|-------------------------------------|
| `Public`      | 0     | Everyone                            |
| `FriendsOnly` | 1     | The author's friends                |
| `Private`     | 2     | The author                          |
| `Unlisted`    | 3     | Anyone with the link, but not listed |

### Zanna.Services.WorkshopUpdateStatus

| Name               | Value | Stage                                   |
|--------------------|-------|-----------------------------------------|
| `None`             | 0     | No upload in progress for the update id |
| `PreparingConfig`  | 1     | Processing the item's settings          |
| `PreparingContent` | 2     | Reading the content files               |
| `UploadingContent` | 3     | Uploading the content                   |
| `UploadingPreview` | 4     | Uploading the preview image             |
| `Committing`       | 5     | Committing the changes                  |

### Zanna.Services.LeaderboardScope

| Name         | Value | Range meaning                                        |
|--------------|-------|------------------------------------------------------|
| `Global`     | 0     | Absolute ranks, counted from 1                       |
| `AroundUser` | 1     | Offsets from the player's rank                       |
| `Friends`    | 2     | The player and their friends; range ignored          |

### Zanna.Services.LeaderboardSort

| Name         | Value | Ranks first            |
|--------------|-------|------------------------|
| `Ascending`  | 1     | The lowest score       |
| `Descending` | 2     | The highest score      |

### Zanna.Services.LeaderboardDisplay

| Name           | Value | Scores are             |
|----------------|-------|------------------------|
| `Numeric`      | 1     | Plain numbers          |
| `Seconds`      | 2     | Times in seconds       |
| `Milliseconds` | 3     | Times in milliseconds  |

### Zanna.Services.OverlayPage

| Name            | Value | Page                                         |
|-----------------|-------|----------------------------------------------|
| `Friends`       | 1     | The friends list                             |
| `Community`     | 2     | The platform community                       |
| `Players`       | 3     | People recently played with                  |
| `Settings`      | 4     | Overlay settings                             |
| `OfficialGroup` | 5     | The game's official group                    |
| `Stats`         | 6     | The player's stats for this game             |
| `Achievements`  | 7     | The player's achievements for this game      |

### Zanna.Services.NotificationPosition

| Name          | Value |
|---------------|-------|
| `TopLeft`     | 0     |
| `TopRight`    | 1     |
| `BottomLeft`  | 2     |
| `BottomRight` | 3     |

### Zanna.Services.TextInputMode

| Name         | Value | Keyboard                                                      |
|--------------|-------|---------------------------------------------------------------|
| `SingleLine` | 0     | One line; Enter finishes                                      |
| `MultiLine`  | 1     | Several lines; the player closes the keyboard                 |
| `Email`      | 2     | Email address layout                                          |
| `Numeric`    | 3     | Numeric layout                                                |
| `Password`   | 4     | Masked full-screen entry (`SingleLine` for the floating keyboard) |

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
core export fails `Init` with `Status.LibraryIncompatible`; one missing an interface or a feature
export starts with those features disabled (see `Platform.Diagnostics()` and
`Platform.HasFeature`).

The SDK is licensed to you by Valve under the Steamworks SDK Access Agreement, which covers
shipping `redistributable_bin` files with your game. Zanna does not include or download them.

### Building depots with zanna package

`zanna package --target steam-windows`, `steam-macos`, or `steam-linux` does the placement above
for you and produces a SteamPipe build root ready for `steamcmd`. Add the Steam settings to
`zanna.project` and point `steam-redist` at your SDK:

```text
steam-app-id 480
steam-redist ../steamworks/sdk
steam-depot windows 481
steam-depot macos 482
steam-depot linux 483
steam-set-live beta
```

```sh
zanna package . --target steam-macos -o mygame-steam
zanna package . --target steam-windows --executable build/win/mygame.exe -o mygame-steam
steamcmd +login <account> +run_app_build "$PWD/mygame-steam/scripts/app_build_480.vdf" +quit
```

Each run stages `content/<platform>/` with the executable, the redistributable, assets, and
`pack` groups where the runtime finds them, and rewrites `scripts/app_build_<app-id>.vdf` and
`manifests/<platform>.json`. The command checks that the redistributable matches the platform, the
executable's architectures, and SDK 1.61–1.65; fails when an executable that uses this provider has
no redistributable configured; and refuses to ship `steam_appid.txt`. On macOS it signs the dylib
and then the bundle with the two entitlements below merged into your `macos-entitlements`. Use
the launch path it reports (`mygame.exe`, `MyGame.app`, or `mygame`) in the Steamworks launch
options. The full reference is [Steam depots](../tools/cli.md#steam-depots).

### App ids and development runs

- `Init("steam", appId)` sets the `SteamAppId` and `SteamGameId` environment variables when they are
  absent, so a `steam_appid.txt` file is not needed during development. Never ship
  `steam_appid.txt`. Values Steam sets when it launches the game are never overwritten.
- The Steam client must be running and signed in to an account that owns the app. Valve's
  Spacewar sample app id `480` is available to every account for experiments and defines sample
  achievements, stats, and leaderboards.
- Under `zanna run`, the executable directory is the toolchain's, so point the provider at a
  library explicitly:

  ```sh
  ZANNA_SERVICES_STEAM_LIBRARY=/path/to/sdk/redistributable_bin/osx/libsteam_api.dylib zanna run main.zia
  ```

  ```powershell
  $env:ZANNA_SERVICES_STEAM_LIBRARY = "C:\steamworks\redistributable_bin\win64\steam_api64.dll"
  zanna run main.zia
  ```

### Configuring player features in Steamworks

- **Achievements and stats** are defined in the app admin under Stats & Achievements and must be
  published before the client sees them. Use the API names there as `Achievements` ids and `Stats`
  names. Steam loads the player's stats before the game starts, so they are readable right after
  `Init`.
- **Leaderboards** are created in the same section (or with `Leaderboards.FindOrCreate`). Set the
  sort method and display type there to match what the game uploads.
- **Rich presence** localization tokens (for `steam_display`) are uploaded as a localization file
  in the app admin.
- **Steam Cloud** needs a byte and file quota in the app admin before `Cloud.IsEnabled` can be
  `TRUE`.
- **Steam Deck**: games with text entry should show `OnScreenKeyboard.ShowFloating` when a text field
  gains focus, or use `OnScreenKeyboard.RequestText`.
- **Steam Input**: upload the action manifest and the default configurations in the app admin's
  Steam Input section and opt into the Steam Input API there; released games then call
  `ActionInput.Start("")`.
- **Workshop**: enable the Workshop for the app in the app admin (Workshop settings), define the
  tags players may use, and accept ready-to-use items there before publishing from the game.

### Overlay and startup order

- Call `Platform.Init` before creating a `Canvas`, `Canvas3D`, or `World3D`. If a GPU-presented
  window already exists, `Diagnostics()` records
  `Steam: initialized after a GPU-presented window was created; the desktop overlay may not attach`.
- The desktop Steam overlay draws on GPU-presented frames only: `Canvas3D` and `World3D` windows
  receive it, but the 2D `Canvas` and GUI applications present through the CPU and do not. That
  includes achievement notifications and the pages opened by `Overlay.Open`.
- Pause single-player games while `EventKind.OverlayChanged` reports the overlay open.
- On macOS, builds signed with the hardened runtime (the default for Developer ID signing) need the
  entitlements `com.apple.security.cs.disable-library-validation` and
  `com.apple.security.cs.allow-dyld-environment-variables` to load `libsteam_api.dylib` and the
  overlay. `zanna package --target steam-macos` adds them to the `macos-entitlements` file when it
  signs; bundles signed by other means need them in that file. Steam games cannot use the App
  Sandbox entitlement.

### Shutting down

Call `Platform.Shutdown()` when the player quits. Native Windows executables exit without running
exit handlers, and platform libraries may already be tearing down during process exit, so the
runtime never shuts the provider down on its own. Call `Stats.Store()` before shutting down when
stats changed since the last commit.

---

## Testing Without Steam

- Games need no special handling: without a started provider every query is neutral and every
  request fails with `Services: no platform services provider is started`, so the same code runs in
  development, CI, and non-Steam builds.
- To exercise the unavailable path deliberately, point `ZANNA_SERVICES_STEAM_LIBRARY` at a file that
  does not exist; `Init` then fails with `Status.LibraryNotFound`.
- Zanna's own tests run the Steam provider against from-scratch fake `steam_api` libraries built by
  the test tree (`src/tests/runtime/RTServicesFakeSteamApi.c`), on the VM and in native binaries,
  from Zia and from BASIC. The fakes model achievements (with icons that load a frame later, an
  achievement without icons, and global percentages), stats, a leaderboard with late-arriving
  player names, rich presence, overlay requests, both keyboards, cloud files, launch parameters,
  the timeline, app details, Steam Input with two controllers, and a Workshop with three items,
  plus a profile that exports none of the player features.

### Checking a real Steam setup

[`examples/apps/steam-check`](../../examples/apps/steam-check/) runs every class against a
running Steam client and prints one `PASS`, `FAIL`, `SKIP`, or `INFO` line per check. It is
read-only unless given `--write`, and the write checks put the account back where Steam allows it.
On 2026-09-14 it passed on macOS arm64 with Spacewar (480), as a VM run and as a
`zanna package --target steam-macos` bundle, against the SDK 1.61 redistributable and against
the SDK 1.65-generation library the Steam client ships:

- **Lifecycle:** `Init`, `Shutdown`, and a second `Init` in the same process.
- **Identity:** user id, name, language, license, and online state.
- **Player count.**
- **Achievements and stats:** readable right after `Init`, even when Steam did not launch the
  game. An unlock, commit, and relock arrive as `AchievementStored` then `StatsStored`.
- **Leaderboards:** find, find-or-create, keep-best upload, and Global, AroundUser, and Friends
  downloads with player names.
- **Presence:** `Set` and `Clear`.
- **Cloud:** write, read, delete, and quota.
- **Launch parameters, timeline, and app details:** every member reached Steam.
- **Achievement icons** (2026-09-15): 64 by 64 RGBA icons that decode to the Steamworks
  artwork, loaded after one `AchievementIconReady` event; Spacewar's icon-less achievement
  is reported once. Spacewar has no global percentages, so `RequestGlobalPercentages` fails
  with `ResultCode` `2` there (Steam's public Web API reports none for app 480 either), and only
  the fakes cover the success path.
- **Steam Input** (2026-09-15, no controller attached): `ActionInput.Start` initialized Steam
  Input on both redistributables, Steam refused the manifest until a controller is used with the
  app ("Timed out waiting for game mapping!", about 1.1 seconds), origin labels and glyph files
  resolved, and `Stop` shut Steam Input down. Actions, origins per controller, rumble, lights, the
  binding panel, and device events need a controller and are covered by the fakes only.
- **Workshop** (2026-09-15): on both redistributables the popular and newest pages of Spacewar's
  Workshop returned 50 of 4,317 items with titles, authors, tags, votes, scores, sizes, dates,
  metadata, and preview URLs, repeated queries reported Steam's cache, and item and user-list
  queries completed. Subscribing, downloads, and publishing change the account (and would download
  or publish content), so only the fakes cover them.

Steam's overlay, the keyboards, and presence seen by a friend need a Steam launch, Big Picture
or Steam Deck, and a second account; those are still to be checked. Under the hardened runtime
the bundle loads the redistributable only with the entitlements `zanna package` adds; without
them `Init` fails with `LibraryIncompatible` and macOS's library validation reason.

---

## Adding a Provider

Providers are C tables compiled into the `zanna_rt_services` runtime component. To add a store:

1. Implement an `rt_services_provider` table (`src/runtime/services/rt_services_provider.h`):
   `start`, `pump`, `stop`, and whichever query callbacks the store supports.
2. Fill the feature operation tables the store supports (`rt_services_achievement_ops`,
   `rt_services_stat_ops`, `rt_services_leaderboard_ops`, `rt_services_presence_ops`,
   `rt_services_overlay_ops`, `rt_services_text_input_ops`, `rt_services_cloud_ops`) and handle the
   request kinds it supports in `begin_request`. Leave the rest `NULL`; the neutral classes then
   report neutral values and failed requests. `begin_request` must never complete the request it
   is starting: the core registers the request only after the callback returns. Fail by returning
   `0` with a message, and complete later from `pump`.
3. List it in the provider registry in `src/runtime/services/rt_services.c`.
4. Optionally add a `Zanna.Services.<Provider>` extension class for store-only features.
5. If the store uploads depot directories, add a `StoreProfile` row in
   `src/tools/common/packaging/StoreDepotBuilder.cpp` (redistributable slots, required exports,
   provider marker, entitlements, forbidden files, build-script writer), its manifest directives,
   and its `zanna package` targets ([ADR 0354](../adr/0354-store-depot-packaging.md)). Staging,
   verification, and manifests are shared.
6. Add tests against a fake library and document the provider on this page.

The neutral classes, status codes, event kinds, and request objects stay unchanged. The contract,
including the policy for runtime-loaded redistributables, is
[ADR 0352](../adr/0352-platform-services-runtime-loaded-providers.md) and
[ADR 0353](../adr/0353-platform-services-player-features.md).
