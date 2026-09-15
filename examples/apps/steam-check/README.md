# steam-check

`steam-check` exercises every `Zanna.Services` class against a real Steam client and a real
Steamworks redistributable. Each check prints `PASS`, `FAIL`, `SKIP`, or `INFO`. The last line is
`RESULT: <n> passed, <n> failed, <n> skipped`, and the exit code is `1` when a check failed. Use it
to confirm a Steam setup before wiring services into a game, and after each Steamworks SDK update.

Without Steam (no client, or no redistributable) it checks that every member stays neutral.

The read-only checks cover the lifecycle, identity, app details, feature groups, the player
count, achievements (including the first achievement's icon and the global unlock
percentages), Steam Input, the Workshop (subscribed items, the first page of the app's items,
an item's details, and the player's uploads), stats, leaderboards, and cloud quota. Apps without global
percentages, Spacewar among them, report `SKIP` for the percentage check because Steam answers
with EResult 2.

The Steam Input check starts `ActionInput` with `steam_input_manifest.vdf` (a small batting and
menu manifest beside this program; `--manifest PATH` picks another), lists the connected
controllers, and reads a button label and glyph. Steam accepts a manifest only once a controller
has been used with the app in the current Steam session, so without a controller the check
reports the refusal as `INFO` and skips the action checks. With a controller it also activates a
set, reads an action label and origin, and runs the rumble motors briefly.

## Running

The Steam client must be running and signed in. Under `zanna run` the provider cannot find the
redistributable beside the toolchain, so name it explicitly:

```sh
ZANNA_SERVICES_STEAM_LIBRARY=/path/to/sdk/redistributable_bin/osx/libsteam_api.dylib \
    zanna run examples/apps/steam-check
```

```powershell
$env:ZANNA_SERVICES_STEAM_LIBRARY = "C:\steamworks\sdk\redistributable_bin\win64\steam_api64.dll"
zanna run examples/apps/steam-check
```

Arguments follow `--`:

| Argument | Effect |
|---|---|
| `appId` | Steam app id; default `480` (Spacewar, available to every account) |
| `--write` | Also run the checks that touch the signed-in account (see below) |
| `--window` | Open a `Canvas3D` window for 20 seconds: overlay status, `Overlay.Open`, notification position, floating keyboard |
| `--keyboards` | With `--window`, also request full-screen text entry |
| `--restart` | Call `Steam.RestartAppIfNecessary` first; the program exits if Steam relaunches it |
| `--board NAME` | Leaderboard to find and download (Spacewar: `Feet Traveled`) |
| `--achievement ID` | Achievement for the write checks (Spacewar: `ACH_WIN_ONE_GAME`) |
| `--int-stat NAME` | Integer stat to read and commit (Spacewar: `NumGames`) |
| `--score N` | With `--write`, upload `N` to `--board` keeping the best score |
| `--manifest PATH` | Steam Input action manifest; default `examples/apps/steam-check/steam_input_manifest.vdf` |

For app `480` the board, achievement, and stat default to the Spacewar names above.

## What `--write` changes

The write checks put the account back where they can:

- The achievement is unlocked, committed, and relocked when it started locked.
- The integer stat is committed with its current value. Steam stats can be increment-only, which
  means a raised value could never be lowered again, so the check never changes the value.
- Rich presence is set and cleared again.
- A small cloud file, `zanna_steam_check.txt`, is written, read, and deleted.
- The recording timeline gets a game mode, a tooltip, a phase with a tag and an attribute, a
  marker (removed again), and a short range event. They appear only in the local game recording.
- `Leaderboards.FindOrCreate` runs on `--board` and creates the board if it does not exist.
- A score is uploaded only with `--score`. Leaderboard entries cannot be removed.

## Limits

- The desktop overlay attaches only to games the Steam client launched (on macOS and Linux Steam
  injects it at launch). A game started from a terminal reports `Overlay.IsEnabled` as `false`
  and never receives `OverlayChanged`.
- The floating and full-screen keyboards need Steam Deck or Big Picture mode.
- Presence can be checked only from a friend's Steam client.
