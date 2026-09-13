---
status: active
audience: public
last-verified: 2026-09-13
---

# Platform Services

> Distribution-platform services behind one provider-neutral API: user identity, licensing, DLC,
> platform events, achievements, stats, leaderboards, rich presence, overlay control, on-screen
> keyboards, and cloud files. Steam is the first provider.

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
- [Zanna.Services.Steam](#zannaservicessteam)
- [Constants](#constants)
- [Shipping with the Steam Provider](#shipping-with-the-steam-provider)
- [Testing Without Steam](#testing-without-steam)
- [Adding a Provider](#adding-a-provider)

For exact signatures, see the generated [Services reference](../generated/runtime/services.md).
The design is recorded in
[ADR 0352](../adr/0352-platform-services-runtime-loaded-providers.md) (platform, requests, Steam
provider), [ADR 0353](../adr/0353-platform-services-player-features.md) (player features), and
[ADR 0354](../adr/0354-store-depot-packaging.md) (store depot packaging).

---

## Overview

`Zanna.Services` connects a game to the store it is distributed through. The API is split in two:

- **Neutral classes** work the same for every provider: `Platform` (lifecycle, identity, licensing,
  events, requests), `Request`, the player features `Achievements`, `Stats`, `Leaderboards`,
  `Presence`, `Overlay`, `OnScreenKeyboard`, and `Cloud`, and the constant classes `Status`, `EventKind`,
  `Feature`, `RequestKind`, `LeaderboardScope`, `LeaderboardSort`, `LeaderboardDisplay`,
  `OverlayPage`, `NotificationPosition`, and `TextInputMode`.
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
| `EventTotal`      | `Integer` (read-only) | Total that a progress event's `EventValue` counts toward, or `0`         |
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

### Methods

| Method               | Signature          | Description                                                        |
|----------------------|--------------------|--------------------------------------------------------------------|
| `EntryRank(index)`   | `Integer(Integer)` | Global rank (from 1) of the entry at `index`                       |
| `EntryScore(index)`  | `Integer(Integer)` | Score of the entry at `index`                                      |
| `EntryUserId(index)` | `String(Integer)`  | Provider user id of the entry (decimal SteamID64 on Steam)         |
| `EntryUserName(index)` | `String(Integer)` | Display name of the entry's user, or `""` while it is unknown     |

### Results by kind

| `Kind`                | `Value`                      | `Flag`                     | `Text`              |
|-----------------------|------------------------------|----------------------------|---------------------|
| `PlayerCount`         | Players currently in game    | —                          | —                   |
| `LeaderboardFind`     | Entries on the board         | —                          | Leaderboard name    |
| `LeaderboardUpload`   | The player's new global rank | The stored score changed   | Leaderboard name    |
| `LeaderboardDownload` | Entries on the board         | —                          | Leaderboard name    |
| `TextInput`           | Length of the text in bytes  | —                          | The submitted text  |

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

### Properties

| Property | Type                  | Description                                  |
|----------|-----------------------|----------------------------------------------|
| `Count`  | `Integer` (read-only) | Number of achievements the app defines, or 0 |

### Behavior Notes

- `Unlock` only changes local state. Call `Stats.Store()` soon after an unlock (at the end of the
  game, for example) to commit it; the unlock notification appears then, and
  `EventKind.AchievementStored` with `EventFlag` `TRUE` confirms each stored unlock.
- `IndicateProgress` shows "10 / 40"-style notifications. It does not store progress (keep that
  in a stat) and does not unlock. `current` and `maximum` must satisfy
  `0 <= current <= maximum` with `maximum > 0`; Steam additionally rejects `0` and
  `current == maximum` and requires the achievement to still be locked.
- A rejected call returns `FALSE` and records a diagnostic such as
  `Steam: SetAchievement('ACH_WIN') failed; check that the achievement is defined for this app`.
- An empty id traps: `Services.Achievements.Unlock: achievement id must not be empty`.

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
- **Rate limits.** Steam limits how often `Store` may be called (on the order of minutes). Commit
  at natural break points such as the end of a game or season, never every frame.
- **Rejected values.** Steam integer stats are 32-bit and floating-point stats are single
  precision. `SetInt("Hits", 3000000000)` returns `FALSE` with
  `Steam: stat 'Hits' value 3000000000 is outside the int32 range`; non-finite floats return
  `FALSE` with `Services: Stats.SetFloat('<name>') rejected a non-finite value`. A stat of the wrong
  type, one the client may not write, or one outside its configured limits returns `FALSE` with a
  diagnostic naming the stat.
- **Reverted stats.** When Steam rejects part of a commit (for example an increment-only stat
  that decreased), `StatsStored` carries result code `8`, Steam restores the stored values, and
  `Diagnostics()` records
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
  Only one can be pending at a time. `maxLength` must be in `1..4096` and traps otherwise.
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
| `StatsStored`             | 8     | `EventFlag`: stored successfully; `EventResultCode`: provider result             |
| `AchievementStored`       | 9     | `EventText`: achievement id; `EventFlag`: unlocked; `EventValue` of `EventTotal`: progress |
| `TextInputDismissed`      | 10    | — (the floating keyboard closed)                                                 |

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

### Zanna.Services.RequestKind

| Name                  | Value | Request                                      |
|-----------------------|-------|----------------------------------------------|
| `PlayerCount`         | 1     | `Platform.RequestPlayerCount()`              |
| `LeaderboardFind`     | 2     | `Leaderboards.Find`, `Leaderboards.FindOrCreate` |
| `LeaderboardUpload`   | 3     | `Leaderboards.Upload`                        |
| `LeaderboardDownload` | 4     | `Leaderboards.Download`                      |
| `TextInput`           | 5     | `OnScreenKeyboard.RequestText`               |

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
  the test tree (`src/tests/runtime/RTServicesFakeSteamApi.c`), on the VM and in native binaries.
  The fakes model achievements, stats, a leaderboard with late-arriving player names, rich
  presence, overlay requests, both keyboards, and cloud files, plus a profile that exports none of
  the player features.

---

## Adding a Provider

Providers are C tables compiled into the `zanna_rt_services` runtime component. To add a store:

1. Implement an `rt_services_provider` table (`src/runtime/services/rt_services_provider.h`):
   `start`, `pump`, `stop`, and whichever query callbacks the store supports.
2. Fill the feature operation tables the store supports (`rt_services_achievement_ops`,
   `rt_services_stat_ops`, `rt_services_leaderboard_ops`, `rt_services_presence_ops`,
   `rt_services_overlay_ops`, `rt_services_text_input_ops`, `rt_services_cloud_ops`) and handle the
   request kinds it supports in `begin_request`. Leave the rest `NULL`; the neutral classes then
   report neutral values and failed requests.
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
