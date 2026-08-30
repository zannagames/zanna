---
status: active
audience: public
last-verified: 2026-08-17
---

# Audio

> Sound effects and music playback for games and applications.

**Part of the [Zanna Runtime Library](README.md)**

Core playback, synthesis, and low-level spatial classes live in the `Zanna.Audio` namespace.
Scene-bound `SoundListener3D` and `SoundSource3D` wrappers live in `Zanna.Graphics3D`.

`Audio.IsAvailable()` reports whether audio support was compiled into the runtime; it does **not**
probe the current device. `Audio.Init()` is the device/backend check and returns 0 on failure.
Sound and music loads initialize the backend lazily, so an explicit `Init()` is optional when the
program does not need to handle initialization failure separately.

In an audio-disabled build, `IsAvailable()` and `Init()` return false/0. Master and mix-group
volume settings still round-trip, and `Synth`/`MusicGen.Build` return `null`; non-null direct
Sound/Music load and playback operations raise an `InvalidOperation` trap because no usable
handle can be produced.

## Contents

- [Zanna.Audio.Sound](#zannaaudiosound)
- [Zanna.Audio.Music](#zannaaudiomusic)
- [Zanna.Audio.Voice](#zannaaudiovoice)
- [Zanna.Audio.Mixer (Static)](#zannaaudiomixer)
- [Zanna.Audio.Playlist](#zannaaudioplaylist)
- [Zanna.Audio.SoundBank](#zannaaudiosoundbank)
- [Zanna.Audio.Synth](#zannaaudiosynth)
- [Zanna.Audio.MusicGen](#zannaaudiomusicgen)
- [Mix Groups](#mix-groups)
- [Mix Group Effects](#mix-group-effects)
- [Spatial Audio](#spatial-audio)
- [Music Crossfading](#music-crossfading)
- [Audio File Format](#audio-file-format)
- [Limits and Behaviors](#limits-and-behaviors)

---

## Zanna.Audio.Sound

Sound effect class for short audio clips. Sounds are loaded entirely into memory for low-latency playback.

**Type:** Instance (obj)
**Constructor:** `Zanna.Audio.Sound.Load(path)`

### Static Methods

| Method       | Signature        | Description                                       |
|--------------|------------------|---------------------------------------------------|
| `Load(path)` | `Sound(String)`  | Load a sound from WAV, OGG Vorbis, or MP3. Returns `null` on failure |
| `LoadAsset(path)` | `Sound(String)` | Resolve WAV, OGG Vorbis, or MP3 bytes through `Zanna.IO.Assets`; accepts plain asset paths and `asset://` URIs |

### Methods

| Method                   | Signature                          | Description                                              |
|--------------------------|------------------------------------|----------------------------------------------------------|
| `Play()`                 | `Integer()`                        | Play once through the SFX mix group. Returns voice ID for control |
| `Play(volume, pan)`    | `Integer(Integer, Integer)`        | Play through the SFX mix group with volume (0–100) and pan (−100 to +100) |
| `Play(volume, pan, pitch)` | `Integer(Integer, Integer, Float)` | `Play` plus a playback-rate multiplier (0.25–4.0; 1.0 = native) |
| `PlayLoop(volume, pan)`  | `Integer(Integer, Integer)`        | Play looped through the SFX mix group with volume and pan |
| `Destroy()`              | `Void()`                            | Release this Sound reference and its decoded buffer when no references remain |

> **Voice limit:** Up to 32 sounds may play simultaneously. A 33rd `Play()` call stops
> the **oldest-started** playing non-looping sound to make room. This is voice stealing,
> not access-based LRU. The
> evicted sound stops with no error or notification. Looping sounds are evicted only
> when all 32 voices are looping.

Playback arguments are clamped to their documented ranges. Every `Play*` method returns `-1`
when the Sound is null, detached, or otherwise cannot start. A successful voice ID is positive;
0 and -1 are never issued as live IDs. At most 256 distinct Sound buffers can be loaded in one
audio context, including synthesized and `MusicGen.Build()` results.

`LoadAsset` searches embedded assets, mounted packs, then the filesystem. Every failure —
missing or zero-byte asset, decode error, or backend failure — returns `null`, matching
`Sound.Load`'s uniform nullable contract. `Destroy()` consumes one retained reference: do not use that reference
again unless another owner (for example, a `SoundBank`) retained the Sound.

### Voice Control

After playing a sound, you receive a voice ID that can be used with `Zanna.Audio.Voice`:

| Method                                 | Description                                       |
|----------------------------------------|---------------------------------------------------|
| `Zanna.Audio.Voice.IsPlaying(id)`      | Returns `true` if voice is still playing          |
| `Zanna.Audio.Voice.SetPan(id, pan)`    | Set pan: −100 = hard left, 0 = center, 100 = right |
| `Zanna.Audio.Voice.SetVolume(id, vol)` | Set voice volume (0–100)                          |
| `Zanna.Audio.Voice.Stop(id)`           | Stop a playing voice                              |

### Zia Example

```zia
module SoundDemo;

bind Zanna.Terminal;
bind Zanna.Audio.Mixer as Audio;
bind Zanna.Audio.Sound as Sound;
bind Zanna.Audio.Voice as Voice;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Audio.Init();

    var snd = Sound.LoadAsset("assets/audio/laser.wav");
    if snd != null {
        // Play with default settings
        var id = snd.Play();

        // Play with volume and pan
        var id2 = snd.Play(80, -50);  // 80% volume, panned left

        // Control the playing voice
        Voice.SetVolume(id2, 50);
        Say("Playing: " + Fmt.Bool(Voice.IsPlaying(id2)));

        // Play looping
        var loopId = snd.PlayLoop(60, 0);
        Voice.Stop(loopId);
    }

    Audio.Shutdown();
}
```

### Example

```basic
' Load sound effects
DIM laser AS Zanna.Audio.Sound
DIM explosion AS Zanna.Audio.Sound

laser = Zanna.Audio.Sound.Load("laser.wav")
explosion = Zanna.Audio.Sound.Load("explosion.wav")

IF NOT Zanna.Core.Object.RefEquals(laser, NOTHING) THEN
    ' Play with default settings
    laser.Play()

    ' Play with custom volume and pan
    DIM voiceId AS INTEGER
    voiceId = laser.Play(80, -50)  ' 80% volume, panned left

    ' Control the playing sound
    Zanna.Audio.Voice.SetVolume(voiceId, 50)
END IF

' Play looping background sound
DIM engineSound AS INTEGER
engineSound = laser.PlayLoop(60, 0)

' Later, stop the loop
Zanna.Audio.Voice.Stop(engineSound)
```

---

## Zanna.Audio.Music

Buffered music class for longer audio tracks. Playback uses incremental decode and fixed-size buffers for memory efficiency.

**Type:** Instance (obj)
**Constructor:** `Zanna.Audio.Music.Load(path)`

> **Concurrent limit:** Up to **4** music streams may be loaded at the same time.
> `Music.Load()` returns `null` if this limit is exceeded. Stop and free unused
> streams before loading new ones.

Ordinary `Play()` and `Resume()` promote a track to the foreground and stop unrelated unpaused
music. A crossfade temporarily plays its source and destination together; paused unrelated tracks
remain paused and retain their load slots.

> **Formats and sample rates:** Music accepts WAV, OGG Vorbis, and MP3. Any supported
> sample rate is resampled to the engine mix rate during playback.

### Static Methods

| Method       | Signature        | Description                                       |
|--------------|------------------|---------------------------------------------------|
| `Load(path)` | `Music(String)`  | Load music from WAV, OGG Vorbis, or MP3. Returns `null` on failure or when the 4-stream limit is reached |

### Properties

| Property   | Type    | Access | Description                        |
|------------|---------|--------|------------------------------------|
| `Duration` | Integer | Read   | Total duration in milliseconds     |
| `Position` | Integer | Read   | Current position in milliseconds   |
| `Volume`   | Integer | R/W    | Logical playback volume (0–100, clamped) |

### Methods

| Method         | Signature           | Description                              |
|----------------|---------------------|------------------------------------------|
| `CrossfadeTo(next, duration)` | `Void(Music, Integer)` | Fade to `next` over `duration` milliseconds |
| `Destroy()`    | `Void()`            | Release this stream reference and its load slot when no references remain |
| `IsPlaying()`  | `Boolean()`         | Returns `true` only while actively playing (not while paused) |
| `Pause()`      | `Void()`            | Pause playback; also freezes an active crossfade involving this track |
| `Play(loop)`   | `Void(Integer)`     | Play the track; stopped tracks restart at zero, and any nonzero `loop` enables looping. The argument is **not** a volume — set `Volume` before `Play` |
| `Resume()`     | `Void()`            | Resume a track paused by `Pause()` and reclaim foreground ownership |
| `Seek(ms)`     | `Void(Integer)`     | Seek to a clamped position in `[0, Duration]` milliseconds |
| `SetLoop(loop)` | `Void(Boolean)`    | Change the loop preference without restarting the track |
| `Stop()`       | `Void()`            | Stop playback and reset this stream's position to zero |

> **Seek behavior:** `Music.Seek(ms)` only repositions that stream. It does not cancel
> unrelated music or active playlist playback.

After `Destroy()`, the released reference must not be used again. `Audio.Shutdown()` instead leaves
the wrapper safe to release but permanently detaches its backend stream; that wrapper remains
inert, so load a new Music object after reinitializing audio.

### Zia Example

```zia
module MusicDemo;

bind Zanna.Terminal;
bind Zanna.Audio.Mixer as Audio;
bind Zanna.Audio.Music as Music;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Audio.Init();
    var c = Canvas.New("Music Player", 400, 200);

    var mus = Music.Load("background.ogg");
    if mus != null {
        mus.SetVolume(70);
        mus.Play(1);  // Looped

        Say("Duration: " + Fmt.Int(mus.get_Duration()) + " ms");

        while !c.get_ShouldClose() {
            c.Poll();
            Say("Pos: " + Fmt.Int(mus.get_Position()));
            c.Flip();
        }

        mus.Stop();
    }

    Audio.Shutdown();
}
```

### Example

```basic
' Initialize audio and create the window used by the loop.
Zanna.Audio.Mixer.Init()
DIM canvas AS Zanna.Graphics.Canvas = Zanna.Graphics.Canvas.New("Music Player", 400, 200)

' Load background music (WAV, at a supported sample rate)
DIM bgMusic AS Zanna.Audio.Music
bgMusic = Zanna.Audio.Music.Load("background.wav")

IF NOT Zanna.Core.Object.RefEquals(bgMusic, NOTHING) THEN
    ' Set initial volume
    bgMusic.Volume = 70

    ' Start playing (looped)
    bgMusic.Play(1)

    ' Check duration
    PRINT "Duration: "; bgMusic.Duration; " ms"

    ' Game loop
    DO WHILE NOT canvas.ShouldClose
        canvas.Poll()

        ' Display current position
        PRINT "Position: "; bgMusic.Position; " / "; bgMusic.Duration

        ' Pause/resume with space bar
        IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Space) THEN
            IF bgMusic.IsPlaying() THEN
                bgMusic.Pause()
            ELSE
                bgMusic.Resume()
            END IF
        END IF

        canvas.Flip()
    LOOP

    ' Cleanup
    bgMusic.Stop()
END IF

Zanna.Audio.Mixer.Shutdown()
```

---

## Zanna.Audio.Voice

Static class for controlling individual playing voices (sound instances).

**Type:** Static utility class

### Methods

| Method                     | Signature                      | Description                                              |
|----------------------------|--------------------------------|----------------------------------------------------------|
| `GetPitch(id)`             | `Float(Integer)`               | Current playback-rate multiplier (1.0 default)           |
| `GetLevel(id)`             | `Float(Integer)`               | Last mixed-block RMS source level; 0 when disabled/stopped |
| `IsPlaying(id)`            | `Boolean(Integer)`             | Check if voice is playing                                |
| `EnableMetering(id, enabled)` | `Void(Integer, Boolean)`    | Enable the per-voice RMS level tap used by `GetLevel`    |
| `SetLowpass(id, hz)`       | `Void(Integer, Float)`         | Direct lowpass cutoff in Hz (≤ 0 disables)               |
| `SetOcclusion(id, amt)`    | `Void(Integer, Float)`         | Occlusion 0 (open) … 1 (occluded); smoothed ~80 ms       |
| `SetPan(id, pan)`          | `Void(Integer, Integer)`       | Pan: −100 = hard left, 0 = center, +100 = hard right     |
| `SetPitch(id, pitch)`      | `Void(Integer, Float)`         | Playback-rate multiplier, clamped 0.25–4.0               |
| `SetVolume(id, vol)`       | `Void(Integer, Integer)`       | Set volume for a voice (0–100)                           |
| `Stop(id)`                 | `Void(Integer)`                | Stop a playing voice                                     |

> **Pan law:** Mono sounds use equal-power panning, so `pan=0` plays evenly in both
> channels without the center-volume drop of a linear pan law. Stereo sounds keep
> their original channel balance at `pan=0`; panning attenuates the far side.

> **Pitch:** Rates other than 1.0 resample with a fractional cursor (linear
> interpolation), so pitch and duration scale together — pitch 2.0 plays one
> octave up in half the time. Typical gunshot variation: `Play(vol, pan,
> 0.92 + 0.16 * rng)`.

> **Occlusion:** The amount maps to a perceptual lowpass sweep (~22 kHz open
> down to ~800 Hz fully occluded) plus up to −6 dB of attenuation. Drive it
> from your own line-of-sight checks each frame; the mixer smooths changes so
> cover flips never click. `SetLowpass` composes with occlusion — the lower
> cutoff wins (useful for scoped-focus or underwater effects).

> **Invalid IDs:** Mutators are safe no-ops for stopped or unknown IDs. Queries return
> `false` from `IsPlaying`, `1.0` from `GetPitch`, and `0.0` from `GetLevel`.
> Voice IDs never use 0 or -1 and are not reused while the older voice is still active.

Metering is disabled by default and has zero per-sample accumulation cost while off. Its level is
the pre-gain source RMS, so voice/group/master volume and spatial attenuation do not reduce the
value. This makes it suitable for lip-sync rather than output-bus metering.

### Zia Example

```zia
module VoiceDemo;

bind Zanna.Terminal;
bind Zanna.Audio;

func start() {
    // Initialize audio system
    Audio.Init();

    // Voice control (safe with invalid IDs)
    SayBool(Voice.IsPlaying(0));  // false
    Voice.Stop(0);               // safe no-op
    Voice.SetVolume(0, 50);      // safe no-op
    Voice.SetPan(0, -50);        // safe no-op (panned left)

    // Master volume
    Audio.SetMasterVolume(80);
    SayInt(Audio.GetMasterVolume());  // 80

    // Global controls
    Audio.PauseAll();
    Audio.ResumeAll();
    Audio.StopAllSounds();

    // Cleanup
    Audio.Shutdown();
}
```

---

## Zanna.Audio.Mixer

Global audio system control functions.

**Type:** Static utility class

### Properties

The counters are read-only snapshots for the current backend context. They return 0 before a live
context exists, after `Shutdown()`, in audio-disabled builds, or when a platform backend does not
expose that event. A new explicit or lazily initialized context starts them from zero.

| Property | Type | Description |
|----------|------|-------------|
| `RenderCalls` | Integer | Mixer render callback/chunk count |
| `MixerLockMisses` | Integer | Render chunks that could not acquire mixer state and used the fallback output path |
| `BackendWriteCalls` | Integer | Platform device write calls |
| `BackendPartialWrites` | Integer | Writes that accepted fewer frames than requested |
| `BackendWaits` | Integer | Backend waits for temporary write unavailability |
| `BackendXruns` | Integer | Backend underrun or suspend observations |
| `BackendRecoveries` | Integer | Backend recovery attempts, successful or not |
| `BackendWriteFailures` | Integer | Device writes that ultimately failed |
| `IsCrossfading` | Boolean | Whether any music crossfade slot is active; this query does not advance time |

### Methods

| Method                          | Signature                      | Description                                       |
|---------------------------------|--------------------------------|---------------------------------------------------|
| `GetMasterVolume()`             | `Integer()`                    | Get current master volume (0–100)                 |
| `Init()`                        | `Integer()`                    | Initialize the audio system. Returns 1 on success |
| `IsAvailable()`                 | `Boolean()`                    | Return whether audio support is compiled in (not whether device initialization will succeed) |
| `PauseAll()`                    | `Void()`                       | Pause all audio playback                          |
| `ResumeAll()`                   | `Void()`                       | Resume all audio playback                         |
| `SetMasterVolume(vol)`          | `Void(Integer)`                | Set master volume for all audio (0–100)           |
| `Shutdown()`                    | `Void()`                       | Shut down the audio system                        |
| `StopAllSounds()`               | `Void()`                       | Stop all playing sounds (does not affect music)   |
| `Update()`                      | `Void()`                       | Advance music crossfades and service streaming buffers |

`Audio.Init()` may be called again after a failed initialization or after
`Audio.Shutdown()`. `Audio.Shutdown()` detaches existing `Sound` and `Music` handles:
destroying those handles remains safe, but playback calls on them fail until the asset is
loaded again.
Call `Audio.Update()` once per frame when using streaming `Music` or direct
`Music.CrossfadeTo`; `Playlist.Update()` already forwards through the same update path.
A `Music` stream holds only ~0.5 s of decoded audio ahead of the mixer and every
refill happens inside `Update()`: a program that never calls it hears the prefill and
then silence, while `IsPlaying()` keeps reporting `true`.
The mixer also attempts a locked buffer prefill if playback reaches an empty music buffer before end-of-stream has been reported, so transient stream-buffer gaps do not force the rest of the render block to silence.

Master volume is clamped to 0–100 and persists as logical settings state across shutdown/re-init.
`PauseAll()` freezes the mixer globally without changing each Music object's individual pause
flag. `StopAllSounds()` affects Sound voices only; it does not stop Music or playlists.

### Zia Example

```zia
module AudioDemo;

bind Zanna.Terminal;
bind Zanna.Audio.Mixer as Audio;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var ok = Audio.Init();
    Say("Init: " + Fmt.Int(ok));

    Audio.SetMasterVolume(80);
    Say("Volume: " + Fmt.Int(Audio.GetMasterVolume()));

    Audio.PauseAll();
    Audio.ResumeAll();
    Audio.StopAllSounds();
    Audio.Shutdown();
    Say("Done");
}
```

### Example

```basic
' Initialize audio
Zanna.Audio.Mixer.Init()

' Set master volume
Zanna.Audio.Mixer.SetMasterVolume(80)

' Pause all audio during pause menu
SUB ShowPauseMenu()
    Zanna.Audio.Mixer.PauseAll()

    ' ... show menu ...

    Zanna.Audio.Mixer.ResumeAll()
END SUB

' Stop all sound effects when changing levels
SUB ChangeLevel(level AS INTEGER)
    Zanna.Audio.Mixer.StopAllSounds()

    ' ... load new level ...
END SUB

' Cleanup before exit
Zanna.Audio.Mixer.Shutdown()
```

---

## Zanna.Audio.Playlist

Mutable queue of music paths with navigation, shuffle, repeat, volume, auto-advance, and optional
crossfades. A Playlist owns its path strings and only loads its current Music stream; during a
crossfade it temporarily retains both streams.

**Type:** Instance (obj)
**Constructor:** `Playlist.New()`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Count` | Integer | Read | Number of queued path entries |
| `Current` | Integer | Read | Actual zero-based track index, or -1 before selection/when empty |
| `IsPlaying` | Boolean | Read | Cached playlist state; false while paused or after a failed load |
| `IsPaused` | Boolean | Read | Whether playlist playback is paused |
| `Volume` | Integer | R/W | Logical volume, clamped to 0–100 and applied immediately |
| `Shuffle` | Boolean | R/W | Use a runtime-RNG permutation while preserving `Current` as the actual index |
| `Repeat` | Integer | R/W | 0 = none, 1 = all, 2 = current track; other values clamp into this range |
| `Crossfade` | Integer | R/W | Auto-transition fade duration in ms, clamped to 0–3,600,000 |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(path)` | `Void(String)` | Append a path |
| `Insert(index, path)` | `Void(Integer, String)` | Insert at an index clamped to `[0, Count]` |
| `Remove(index)` | `Void(Integer)` | Remove an entry; invalid indices are ignored |
| `Clear()` | `Void()` | Stop playback and remove every entry |
| `Get(index)` | `String(Integer)` | Return a retained path, or an empty String when out of range |
| `Play()` | `Void()` | Resume the paused current track or load/play the current (initially first) entry |
| `Pause()` | `Void()` | Pause the current track and any crossfade that owns it |
| `Stop()` | `Void()` | Stop and rewind the current track without changing `Current` |
| `Next()` | `Void()` | Move forward; repeat-all wraps, while repeat-none stops at the last entry |
| `Previous()` | `Void()` | Move backward; repeat-all wraps, while other modes clamp at the first entry |
| `Jump(index)` | `Void(Integer)` | Select an actual queue index; invalid indices are ignored |
| `Update()` | `Void()` | Service streaming/crossfades and auto-advance a successfully playing track |

`Add(null)` and `Insert(index, null)` store an empty path. `Get` therefore cannot distinguish an
out-of-range index from a deliberately empty entry. Paths are not validated until selected;
`Play()` has no result value, so inspect `IsPlaying`. `Play()` and auto-advance skip
unloadable entries: when the selected path fails to load, subsequent entries are tried with the
usual repeat/shuffle wrap semantics, bounded by the track count, so one bad track cannot stall
the queue. If every entry fails, the playlist stops cleanly and `IsPlaying` stays false.

Changing `Next`, `Prev`, or `Jump` preserves the stopped/paused/playing state. Crossfade applies
only when changing tracks while playback is active and both old and new Music handles exist;
paused or stopped navigation is immediate. `Update()` must be called regularly while playing. It
also calls `Audio.Update()`, so a separate audio update is unnecessary for that playlist.

Shuffle uses the global runtime RNG. Enabling it creates a fresh permutation, edits rebuild the
permutation, and repeat-all reshuffles when `Next()` wraps. `Zanna.Math.Random.Seed()` can make
this deterministic. `Jump(index)` and `Current` always refer to the original queue index, not the
permutation slot. Playlist objects are not thread-safe.

### Zia Example

```zia
module PlaylistDemo;

bind Zanna.Terminal;
bind Zanna.Audio;

func start() {
    var tracks = Playlist.New();
    tracks.Add("music/title.ogg");
    tracks.Add("music/level.ogg");
    tracks.Insert(1, "music/menu.ogg");

    tracks.Volume = 70;
    tracks.Repeat = 1;       // repeat all
    tracks.Shuffle = true;
    tracks.Crossfade = 750;

    SayInt(tracks.Count);    // 3
    Say(tracks.Get(0));

    // A real frame loop calls Play once, then Update every frame.
    // Failure to load these example paths simply leaves IsPlaying false.
    if Audio.Init() != 0 {
        tracks.Play();
        tracks.Update();
        tracks.Stop();
        Audio.Shutdown();
    }

    tracks.Clear();
}
```

---

## Zanna.Audio.SoundBank

Named sound registry that maps string names to loaded Sound objects. Games use SoundBank to manage sounds by name instead of managing Sound handles directly.

**Type:** Instance (obj)
**Constructor:** `SoundBank.New()`

### Properties

| Property | Type    | Access | Description                  |
|----------|---------|--------|------------------------------|
| `Count`  | Integer | Read   | Number of registered sounds  |

### Methods

| Method                     | Signature                          | Description                                                          |
|----------------------------|------------------------------------|----------------------------------------------------------------------|
| `Register(name, path)`    | `Integer(String, String)`          | Load WAV, OGG Vorbis, or MP3 and register under name. Returns 1 on success |
| `RegisterSound(name, sound)` | `Integer(String, Sound)`        | Register an existing Sound object (e.g., from Synth). Returns 1 on success |
| `Play(name)`              | `Integer(String)`                  | Play named sound. Returns voice ID, or -1 if not found              |
| `Play(name, vol, pan)`  | `Integer(String, Integer, Integer)` | Play with volume (0-100) and pan (-100 to 100)                     |
| `Has(name)`               | `Boolean(String)`                  | Check if name is registered                                         |
| `Get(name)`               | `Sound(String)`                    | Get the Sound object for a name, or null                            |
| `Remove(name)`            | `Void(String)`                     | Remove a named entry                                                |
| `Clear()`                 | `Void()`                           | Remove all entries                                                  |

Max 64 entries per bank. Non-null names, including the empty String, are matched by full runtime
String value and are not truncated; the bank is not thread-safe. Registering an existing name
replaces it without increasing `Count`. `Register` first loads the replacement, so a failed load
returns 0 and preserves the old entry. It also returns 0 (rather than trapping) in an
audio-disabled build.

`RegisterSound` accepts only a recognized Sound wrapper returned by `Sound.Load`, `Synth`, or
`MusicGen.Build` whose backend buffer is attached to the live audio context; generic objects and
wrappers detached by `Audio.Shutdown()` are rejected and return 0. Reload sounds in a new
audio context. The bank retains registered Sounds. `Get` returns another retained reference;
removing or clearing the bank does not invalidate references already returned to the caller.

### Zia Example

```zia
module BankDemo;

bind Zanna.Audio;

func start() {
    Audio.Init();
    var bank = SoundBank.New();

    // Load from files
    bank.Register("laser", "sfx/laser.wav");
    bank.Register("explode", "sfx/boom.wav");

    // Register synthesized sounds
    var coinSfx = Synth.Sfx(1);  // Coin preset
    bank.RegisterSound("coin", coinSfx);

    // Play by name
    bank.Play("laser");
    bank.Play("explode", 80, -50);  // 80% vol, panned left

    Audio.Shutdown();
}
```

---

## Zanna.Audio.Synth

Procedural sound synthesis — generates Sound objects from parameters without WAV files. All generated sounds are 16-bit PCM mono at 44100 Hz.

In audio-disabled builds, Synth methods return `null` instead of trapping. This lets
asset setup code degrade cleanly while direct `Sound.Load`, `Sound.Play`, and `Music`
playback APIs still report that audio support is unavailable.

**Type:** Static utility class

### Waveform Types

| Constant  | Value | Description          |
|-----------|-------|----------------------|
| Sine      | 0     | Smooth sine wave     |
| Square    | 1     | Digital square wave  |
| Sawtooth  | 2     | Ramp/sawtooth wave   |
| Triangle  | 3     | Triangle wave        |

### SFX Preset Types

| Constant  | Value | Description                                       |
|-----------|-------|---------------------------------------------------|
| Jump      | 0     | Quick ascending frequency sweep (square wave)     |
| Coin      | 1     | Two-tone high-pitched pickup sound                |
| Hit       | 2     | Short noise burst with fast decay                 |
| Explosion | 3     | Longer noise burst with slow decay                |
| Powerup   | 4     | Ascending sweep with triangle wave                |
| Laser     | 5     | Quick descending sweep with sawtooth              |

### Methods

| Method                              | Signature                                  | Description                                                                                       |
|-------------------------------------|--------------------------------------------|---------------------------------------------------------------------------------------------------|
| `Tone(freq, duration, wave)`        | `Sound(Integer, Integer, Integer)`         | Generate a fixed-frequency tone. freq: Hz (20-20000), duration: ms (1-10000), wave: waveform type |
| `Sweep(startHz, endHz, duration, wave)` | `Sound(Integer, Integer, Integer, Integer)` | Generate a frequency sweep between two frequencies                                              |
| `Noise(duration, volume)`           | `Sound(Integer, Integer)`                  | Generate white noise with quadratic decay. volume: 0-100                                          |
| `Sfx(type)`                         | `Sound(Integer)`                           | Generate a preset game sound effect                                                               |

`Tone` and `Sweep` clamp frequency to 20–20,000 Hz, duration to 1–10,000 ms, and waveform
to 0–3; the waveform argument selects oscillator shape, not volume. `Noise` clamps the same
duration range and volume to 0–100, and seeds its local generator from the runtime RNG. Unknown
`Sfx` preset values return `null`. Generated results consume ordinary Sound-buffer slots and use
the same playback/lifetime rules as file-loaded Sounds. Use `Sound.PlayEx`, `Sound.PlayLoop`, or
`Voice.SetVolume` to control playback volume.

### Zia Example

```zia
module SynthDemo;

bind Zanna.Audio;

func start() {
    Audio.Init();

    // Generate tones
    var beep = Synth.Tone(440, 200, 0);       // A4, 200ms, sine
    var buzz = Synth.Tone(220, 100, 1);       // A3, 100ms, square

    // Frequency sweeps
    var rising = Synth.Sweep(200, 800, 300, 0);  // Rising sine
    var laser = Synth.Sweep(1500, 200, 120, 2);  // Falling sawtooth

    // White noise
    var static_noise = Synth.Noise(500, 80);  // 500ms, 80% volume

    // Preset SFX (no WAV files needed!)
    var jumpSnd = Synth.Sfx(0);   // Jump
    var coinSnd = Synth.Sfx(1);   // Coin
    var hitSnd  = Synth.Sfx(2);   // Hit
    var boomSnd = Synth.Sfx(3);   // Explosion

    // Play them
    beep.Play();
    jumpSnd.Play();

    // Register in a SoundBank for easy access
    var bank = SoundBank.New();
    bank.RegisterSound("jump", jumpSnd);
    bank.RegisterSound("coin", coinSnd);
    bank.Play("jump");

    Audio.Shutdown();
}
```

---

## Zanna.Audio.MusicGen

Procedural music composition — a tracker-style sequencer that builds multi-channel songs with ADSR envelopes and chiptune effects. It pre-renders stereo 16-bit PCM at 44100 Hz into a standard Sound object, requiring zero external audio assets. Think NES/SNES-era music but at 44.1kHz with full ADSR envelopes.

**Type:** Mutable builder class

### Concepts

- **Centbeats:** Beat positions and durations use centbeats where 100 = 1 beat. At 120 BPM, 1 centbeat = 5ms. This provides sub-beat precision (16th notes = 25 centbeats, triplets = 33).
- **Centi-Hz:** Effect speeds (vibrato, tremolo, arpeggio) use centi-Hz where 100 = 1 Hz.
- **Cents:** Pitch offsets (detune, vibrato depth) use cents where 100 = 1 semitone.
- **MIDI Notes:** Standard MIDI numbering (60 = C4, 69 = A4 = 440 Hz, range 0-127).

### Waveform Types

| Constant  | Value | Description                              |
|-----------|-------|------------------------------------------|
| Sine      | 0     | Smooth sine wave (pads, soft leads)      |
| Square    | 1     | Pulse wave with variable duty cycle (classic chiptune lead) |
| Sawtooth  | 2     | Bright sawtooth (basses, strings)        |
| Triangle  | 3     | Soft triangle (NES bass channel)         |
| Noise     | 4     | Filtered noise (drums, percussion)       |

### Song Builder Methods

| Method                                        | Signature                                          | Description                                                                |
|-----------------------------------------------|----------------------------------------------------|----------------------------------------------------------------------------|
| `New(bpm)`                                    | `MusicGen(Integer)`                                | Create a new song; BPM is clamped to 20–300                                |
| `AddChannel(waveform)`                        | `Integer(Integer)`                                 | Add a channel (waveform clamps to 0–4). Returns index, or -1 if full/invalid |
| `SetEnvelope(ch, attack, decay, sustain, release)` | `Void(Integer, Integer, Integer, Integer, Integer)` | Set ADSR envelope: attack/decay/release in ms (0-5000), sustain in % (0-100) |
| `SetChannelVolume(ch, volume)`                   | `Void(Integer, Integer)`                           | Set channel volume (0-100, default 80)                                     |
| `SetDuty(ch, duty)`                           | `Void(Integer, Integer)`                           | Set square wave duty cycle (1-99, default 50). NES values: 12, 25, 50, 75 |
| `SetPan(ch, pan)`                             | `Void(Integer, Integer)`                           | Set stereo pan (-100=left, 0=center, 100=right)                            |
| `SetDetune(ch, cents)`                        | `Void(Integer, Integer)`                           | Constant pitch offset in cents (-1200 to 1200) for chorusing               |

### Effect Methods

| Method                                    | Signature                                   | Description                                                                              |
|-------------------------------------------|---------------------------------------------|------------------------------------------------------------------------------------------|
| `SetVibrato(ch, depth, speed)`            | `Void(Integer, Integer, Integer)`           | Pitch modulation. depth: cents (0-200), speed: centi-Hz (500 = 5 Hz)                    |
| `SetTremolo(ch, depth, speed)`            | `Void(Integer, Integer, Integer)`           | Volume modulation. depth: % (0-100), speed: centi-Hz (400 = 4 Hz)                       |
| `SetArpeggio(ch, semi1, semi2, speed)`    | `Void(Integer, Integer, Integer, Integer)`  | Rapid pitch cycling through [root, +semi1, +semi2] at speed centi-Hz. 1500 = 15 Hz (classic) |
| `SetPortamento(ch, speed)`                | `Void(Integer, Integer)`                    | Pitch glide between consecutive notes. speed: ms (0=off, 20-500 typical)                 |

### Note Methods

| Method                                        | Signature                                              | Description                                                  |
|-----------------------------------------------|--------------------------------------------------------|--------------------------------------------------------------|
| `AddNote(ch, beatPos, midiNote, duration)`    | `Integer(Integer, Integer, Integer, Integer)`          | Add a note. Returns 1 on success, 0 if channel is full      |
| `AddNoteVelocity(ch, beatPos, midiNote, dur, vel)` | `Integer(Integer, Integer, Integer, Integer, Integer)` | Add a note with explicit velocity (0-100). Default is 100    |

### Song Properties

| Property                   | Type                    | Description                                                       |
|----------------------------|-------------------------|-------------------------------------------------------------------|
| `Bpm` (read-only)          | `Integer`               | Tempo in beats per minute                                         |
| `Length` (read/write)       | `Integer`               | Song length in centbeats (100 = 1 beat)                           |
| `ChannelCount` (read-only) | `Integer`               | Number of channels added                                          |

### Song Finalization Methods

| Method | Signature | Description |
|---|---|---|
| `SetSwing(amount)`         | `Void(Integer)`         | Swing feel (0-100). Shifts off-beat notes forward. 0 = straight   |
| `SetLoopable(flag)`        | `Void(Boolean)`         | `true` = apply crossfade at loop boundary for click-free looping  |
| `Build()`                  | `Sound()`               | Pre-render all channels to a Sound object. Returns null on failure |

`Length`, note positions, and note durations are clamped to the maximum renderable
song span for the selected BPM. Notes whose start position is at or after that
maximum span are rejected. `Build()` returns `null` in audio-disabled builds.
Loopable output keeps the requested length and blends only the loop boundary; it
does not shorten the generated sound.

`Length` is not inferred from notes: `Build()` requires at least one channel and a positive
explicit length, although a song with no audible notes can still build as silence. Negative note
positions become 0; MIDI note and velocity values clamp to 0–127 and 0–100, and durations below
one centbeat become one. `AddNote*` returns 0 for an invalid channel, a full channel, or a start at
the five-minute boundary. Invalid channel indices on configuration setters are silent no-ops.

Effect setters clamp speed to 0–5,000 centi-Hz, arpeggio intervals to 0–24 semitones, and
portamento to 0–2,000 ms in addition to the ranges shown above. Builders are mutable and not
thread-safe. `Build()` allocates the entire stereo render and can return `null` on allocation or
Sound-buffer failure, especially near the five-minute maximum.

### Noise Channel — Percussion

The noise channel (type 4) uses the MIDI note number as a one-pole low-pass cutoff rather than as
an oscillator pitch. The cutoff follows the standard frequency of the note after clamping it to
10–120: low values produce darker noise for kicks/toms, middle values suit snares/claps, and high
values produce brighter hi-hat/cymbal noise.

### Limits

| Limit                    | Value      |
|--------------------------|------------|
| Max channels per song    | **8**      |
| Max notes per channel    | **4,096**  |
| Max song duration        | **5 min**  |
| Max BPM                  | **300**    |
| Min BPM                  | **20**     |
| Loop crossfade duration  | **10 ms**  |

### Zia Example

```zia
module MusicDemo;

bind Zanna.Audio;

func start() {
    Audio.Init();

    // Create a 4-bar chiptune loop at 140 BPM
    var song = MusicGen.New(140);

    // Channels
    var lead = song.AddChannel(1);    // square
    var bass = song.AddChannel(2);    // sawtooth
    var drums = song.AddChannel(4);   // noise

    // Lead: plucky square with vibrato
    song.SetEnvelope(lead, 10, 60, 50, 80);
    song.SetDuty(lead, 25);
    song.SetVibrato(lead, 12, 500);
    song.SetPan(lead, -20);

    // Bass: punchy saw
    song.SetEnvelope(bass, 5, 40, 80, 50);

    // Drums: percussive noise
    song.SetEnvelope(drums, 1, 20, 0, 30);

    // Melody: C5 E5 G5 C6 (quarter notes)
    song.AddNote(lead, 0, 72, 100);
    song.AddNote(lead, 100, 76, 100);
    song.AddNote(lead, 200, 79, 100);
    song.AddNote(lead, 300, 84, 100);

    // Bass: C3 half notes
    song.AddNote(bass, 0, 48, 200);
    song.AddNote(bass, 200, 48, 200);

    // Drums: kick on 1 & 3, snare on 2 & 4
    song.AddNote(drums, 0, 24, 25);      // dark kick noise
    song.AddNote(drums, 100, 60, 30);    // mid-band snare noise
    song.AddNote(drums, 200, 24, 25);    // dark kick noise
    song.AddNote(drums, 300, 60, 30);    // mid-band snare noise

    // Build and play
    song.Length = 400;         // 4 beats
    song.SetLoopable(true);    // seamless loop
    var sound = song.Build();

    if sound != null {
        var voice = sound.PlayLoop(70, 0);
        // voice can be stopped with Voice.Stop(voice)
    }

    Audio.Shutdown();
}
```

### BASIC Example

```basic
' Procedural music demo
Zanna.Audio.Mixer.Init()

DIM song AS Zanna.Audio.MusicGen = Zanna.Audio.MusicGen.New(120)
DIM channel AS INTEGER = song.AddChannel(1)
song.SetEnvelope(channel, 10, 50, 70, 100)
song.AddNote(channel, 0, 60, 100)
song.AddNote(channel, 100, 64, 100)
song.AddNote(channel, 200, 67, 100)
song.AddNote(channel, 300, 72, 100)
song.Length = 400
song.SetLoopable(TRUE)

DIM snd AS Zanna.Audio.Sound = song.Build()
IF NOT Zanna.Core.Object.RefEquals(snd, NOTHING) THEN
    DIM voiceId AS INTEGER = snd.PlayLoop(70, 0)
END IF

Zanna.Audio.Mixer.Shutdown()
```

---

## Mix Groups

Independent volume control for music vs sound effects. Players expect to adjust these separately in game settings menus.

### Group Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `MUSIC` | 0 | Music tracks (Music, Playlist) |
| `SFX` | 1 | Sound effects (Sound) |
| named groups | 100–255 | Up to 156 runtime-registered custom categories; ids 2–99 are not allocated |

### Audio Methods (Mix Groups)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Audio.SetGroupVolume(group, vol)` | `Void(Integer, Integer)` | Set group volume (0-100, clamped) |
| `Audio.GetGroupVolume(group)` | `Integer(Integer)` | Get group volume (100 if invalid group) |
| `Audio.RegisterGroup(name)` | `Integer(String)` | Register or find a named group. Built-ins `music` and `sfx` return 0 and 1 |
| `Audio.FindGroup(name)` | `Integer(String)` | Return a named group id, or -1 |
| `Audio.FindGroup(name)` | `Option[Integer](String)` | Return a named group id as `Some(id)`, or `None` |
| `Audio.SetGroupVolumeNamed(name, vol)` | `Void(String, Integer)` | Register if needed, then set volume |
| `Audio.GetGroupVolumeNamed(name)` | `Integer(String)` | Get a named group volume, or 100 if missing |
| `Audio.GroupName(group)` | `String(Integer)` | Return a registered group name, or empty string |
| `Audio.GroupAddLowpass(group, cutoffHz, q)` | `Integer(Integer, Float, Float)` | Add a low-pass filter insert and return an effect ID |
| `Audio.GroupAddHighpass(group, cutoffHz, q)` | `Integer(Integer, Float, Float)` | Add a high-pass filter insert and return an effect ID |
| `Audio.GroupAddPeaking(group, freqHz, q, gainDb)` | `Integer(Integer, Float, Float, Float)` | Add a peaking EQ insert and return an effect ID |
| `Audio.GroupAddDelay(group, ms, feedback, wet)` | `Integer(Integer, Float, Float, Float)` | Add a delay insert and return an effect ID |
| `Audio.GroupAddReverb(group, roomSize, damping, wet)` | `Integer(Integer, Float, Float, Float)` | Add a reverb insert and return an effect ID |
| `Audio.GroupSetReverb(group, fxId, roomSize, damping, wet)` | `Void(Integer, Integer, Float, Float, Float)` | Update an existing reverb without reallocating its delay lines |
| `Audio.GroupSetFxBypass(group, fxId, bypass)` | `Void(Integer, Integer, Boolean)` | Enable or bypass one group effect |
| `Audio.GroupRemoveFx(group, fxId)` | `Void(Integer, Integer)` | Remove one group effect |
| `Audio.GroupClearFx(group)` | `Void(Integer)` | Remove every effect from the group |
| `Audio.SetGroupDucking(trigger, target, amount, attackSec, releaseSec)` | `Void(String, String, Float, Float, Float)` | Sidechain duck: while `trigger` is audible, `target`'s gain eases toward `1 − amount` |

> **Ducking:** Groups are resolved (and registered on first use) by name. While
> any backend voice/music stream in the trigger group is in the playing state with stored
> volume above 0.001, the target group's gain follows an
> exponential envelope toward `1 − amount` over `attackSec` and recovers to
> unity over `releaseSec`. Re-registering the same (trigger, target) pair
> replaces the rule; `amount <= 0` removes it. Up to 8 rules may be active.
> Classic use: `Audio.SetGroupDucking("weapons", "music", 0.35, 0.03, 0.4)`.

This is activity detection, not a sample-level sidechain: master mute, silent sample data, and
post-voice filtering do not prevent a nominally active trigger. Amount above 1 clamps to 1;
non-positive or NaN amount removes the rule. Non-positive or NaN attack/release becomes 1 ms;
positive infinity is retained and effectively freezes that envelope direction. A ninth distinct
rule is silently ignored.

### Sound Methods (Group-Aware)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Sound.PlayGroup(group)` | `Integer(Integer)` | Play in mix group (applies group volume) |
| `Sound.PlayGroup(vol, pan, group)` | `Integer(Int, Int, Int)` | Play with volume/pan in group |
| `Sound.PlayLoopGroup(vol, pan, group)` | `Integer(Int, Int, Int)` | Loop with volume/pan in group |

Plain `Sound.Play`, `Sound.PlayEx`, and `Sound.PlayLoop` use the SFX group by default.
Group-specific play methods apply the requested group volume exactly once. Effective
volume = `voice_volume × group_volume / 100`; master volume is applied on top by the
audio system. An invalid group passed to a Sound `Play*Group` method falls back to SFX;
`SetGroupVolume` ignores invalid groups and `GetGroupVolume` returns 100.

Named groups are useful for settings such as UI, ambience, dialogue, and cutscene sound
without overloading the two built-ins. Registered named groups receive stable ids within
the current process and can be passed to `PlayGroup`, `PlayGroupWithVolumePan`, and
`PlayLoopGroup`. Registration returns -1 for an empty canonical name or after all 156 slots
are used.

Names are case-sensitive but are canonicalized before both registration and lookup: leading and
trailing spaces/tabs are removed and embedded `NUL` bytes become `_`. A canonical name longer
than 31 bytes is rejected (registration and lookup return -1) rather than truncated, so
distinct names can never alias the same group. `GroupName` returns the trimmed form. Both the
real-audio and audio-disabled implementations serialize group initialization, registration,
and volume state with the shared audio-state lock.

## Mix Group Effects

Mix-group effects are insert chains on the group bus. Every playing voice and
music stream first contributes to its group, the chain processes that group once,
then the processed result is mixed into the master output.

The built-in effects are:

| Effect | Parameters | Typical use |
|--------|------------|-------------|
| Low-pass | `cutoffHz`, `q` | Occlusion, underwater/muffled SFX |
| High-pass | `cutoffHz`, `q` | Radio/phone voices, thin UI cues |
| Peaking EQ | `freqHz`, `q`, `gainDb` | Boost or cut one band |
| Delay | `ms`, `feedback`, `wet` | Echoes and slapback |
| Reverb | `roomSize`, `damping`, `wet` | Room or ambience tails |

Effect creation allocates any needed delay/reverb buffers up front. The mixer
process path performs no per-block allocations, and bypassed effects remain in
the chain without changing the signal.

Add methods return a positive process-wide effect ID or -1 for an unregistered group/allocation
failure. There is no fixed chain-length cap. Filter frequency is bounded to 10–19,845 Hz; Q above
20 clamps to 20, while non-finite or Q below 0.05 selects 0.707. Delay time is bounded to
1–2,000 ms, feedback to 0–0.95, and wet mix to 0–1. Reverb room size, damping, and wet mix clamp
to 0–1. `GroupSetReverb` changes only a matching reverb ID in the named group; wrong kind/group/id
is a no-op. Peaking `gainDb` is not range- or non-finite-sanitized, so callers must supply a finite,
practical value.

### Zia Example

```zia
module AudioFxDemo;

bind Zanna.Audio.Mixer as Audio;
bind Zanna.Audio.Sound as Sound;

func start() {
    if Audio.Init() == 0 {
        return;
    }

    var ambience = Audio.RegisterGroup("ambience");
    var sfx = Audio.RegisterGroup("sfx");

    var reverb = Audio.GroupAddReverb(ambience, 0.72, 0.35, 0.45);
    var occlusion = Audio.GroupAddLowpass(sfx, 1600.0, 0.707);

    var hit = Sound.LoadAsset("assets/audio/hit.wav");
    if hit != null {
        hit.PlayGroup(90, 0, sfx);
    }

    Audio.GroupSetFxBypass(sfx, occlusion, true);
    Audio.GroupRemoveFx(ambience, reverb);
    Audio.GroupClearFx(sfx);

    Audio.Shutdown();
}
```

Effects require compiled audio support. The plain mix-group volume/name APIs
remain available as settings state in audio-disabled builds.

## Spatial Audio

`Zanna.Audio.SpatialAudio3D` is the low-level spatial audio surface. Its
implementation lives under `src/runtime/audio/` with the rest of the audio
runtime. It computes linear distance attenuation and stereo pan before
delegating to normal `Sound` voice playback. The registry types the object
arguments generically, but callers must pass a `Zanna.Audio.Sound` and
`Zanna.Math.Vec3` handles in the positions shown below.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SpatialAudio3D.SetListener(position, forward)` | `Void(Vec3, Vec3)` | Set the fallback listener position and forward direction |
| `SpatialAudio3D.PlayAt(sound, position, maxDist, volume)` | `Integer(Sound, Vec3, Float, Integer)` | Play at a world position; returns a positive voice ID, or -1 on failure (the same sentinel as `Sound.Play*`) |
| `SpatialAudio3D.UpdateVoice(voice, position, maxDist)` | `Void(Integer, Vec3, Float)` | Recompute attenuation and pan for a moving voice |
| `SpatialAudio3D.SyncBindings(dt)` | `Void(Float)` | Sync object-backed listeners/sources bound to scene nodes or cameras |

`SetListener` changes only the fallback listener; an active `SoundListener3D` overrides it.
`PlayAt` clamps volume to 0–100. A non-positive or non-finite `maxDist` means infinite range at
play time. `UpdateVoice` instead reuses the distance recorded for that voice when `maxDist` is
non-positive or non-finite; an untracked voice defaults to 50 units. The low-level public methods
do not accept source velocity, so they update only attenuation and pan. Doppler playback-rate
changes come from the object-backed `SoundSource3D` path.

`SoundListener3D` and `SoundSource3D` remain in the
`Zanna.Graphics3D` namespace because those object wrappers bind to
`SceneNode` and `Camera3D`. They call into the audio-owned spatial math; the
graphics side owns only scene/camera binding and lifetime integration.

| Graphics3D type | Description |
|-----------------|-------------|
| `SoundListener3D` | Active listener pose, velocity, node/camera binding, and `IsActive` selection |
| `SoundSource3D` | Positional `Sound` instance with ref/max distance, velocity, looping, and optional node binding |

Scene-driven games usually update transforms, call `SceneGraph.SyncBindings(dt)`,
then trigger `SoundSource3D.Play()` or `SpatialAudio3D.PlayAt(...)` for the
frame. Direct spatial callers can skip the object wrappers and call
`SpatialAudio3D.SetListener`, `PlayAt`, and `UpdateVoice` with `Vec3` handles.
Spatial state and object binding are main-thread facilities. `SyncBindings` treats non-finite or
negative `dt` as zero and caps a large step at one second; it is a no-op when graphics support is
not compiled in.

---

## Music Crossfading

Smooth transitions between music tracks — the old track fades out while the new one fades in simultaneously.

One unpaused transition owns the foreground at a time. Starting another direct or playlist
crossfade cancels unrelated **unpaused** fades and stops both of their tracks. An unrelated paused
fade is preserved and can reclaim the foreground when resumed.

Pausing a track or playlist that owns a crossfade now pauses the fade clock too; `Audio.Update()`
and `Playlist.Update()` do not advance a paused transition.

The volume envelopes are linear. A non-positive duration performs an immediate cut, and a
same-track transition is a no-op. Passing `null` for one side, where the source language permits
it, fades in from silence or fades out to silence. Direct crossfades advance from wall-clock time
when `Audio.Update()` runs; playlist updates call that method internally. Playlist auto-crossfade
is used only when a current Music handle is actively playing and the replacement track loads
successfully—otherwise the playlist swaps or stops without a fade.

### Music Methods (Crossfade)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Music.CrossfadeTo(newMusic, duration)` | `Void(Music, Integer)` | Crossfade to new track over duration ms, preserving the destination track's loop flag |

### Audio Properties (Crossfade)

| Property | Type | Description |
|----------|------|-------------|
| `Audio.IsCrossfading` | Boolean (read-only) | True during an active crossfade; reading it does not advance or complete the fade |

### Audio Methods (Crossfade)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Audio.Update()` | `Void()` | Advance active crossfades and service streaming music buffers; call once per frame when using `Music.CrossfadeTo` or direct `Music` playback (paused crossfades stay frozen) |

### Playlist Properties (Crossfade)

| Property | Type | Description |
|----------|------|-------------|
| `Playlist.Crossfade` | Integer (read/write) | Auto-crossfade duration for track changes (0 = disabled) |

### Example

```zia
bind Zanna.Audio;

// Set up volume sliders
Audio.SetGroupVolume(0, 80);  // Music at 80%
Audio.SetGroupVolume(1, 100); // SFX at 100%

// Play SFX in the SFX group
explosionSound.PlayGroup(1);

// Crossfade between music tracks
currentTrack.CrossfadeTo(newTrack, 2000); // 2-second crossfade
Audio.Update(); // call each frame while direct music/crossfades run

// Enable auto-crossfade on playlist
playlist.Crossfade = 1000; // 1-second crossfade between tracks
playlist.Update();         // advances playback and pending playlist crossfades
```

---

## Audio File Format

Zanna Audio supports **WAV (PCM)**, **OGG Vorbis**, and **MP3** files. The format is
auto-detected from file magic bytes — no extension matching required.

| Property    | WAV                                      | OGG Vorbis                          | MP3                                 |
|-------------|------------------------------------------|-------------------------------------|-------------------------------------|
| Format      | PCM or 32-bit IEEE float                 | Vorbis I (baseline, Huffman-coded) | MPEG-1/2/2.5 Layer III              |
| Bit depth   | 8/16/24/32-bit PCM, or 32-bit float     | N/A (lossy compressed)             | N/A (lossy compressed)              |
| Channels    | Mono or Stereo                           | Mono or Stereo                     | Mono, Stereo, or Joint Stereo       |
| Sample rate | 1–384000 Hz (resampled to 44100) | 1–384000 Hz (resampled to 44100) | 1–384000 Hz (resampled to 44100) |

### Tips

1. **Sound effects:** Rates from 1 through 384000 Hz are accepted and resampled to 44100 Hz at load time.
2. **Music playback:** Rates from 1 through 384000 Hz are resampled during buffered playback.
3. **Memory:** Sounds are loaded entirely into memory; keep individual clips short. Eager WAV
   decode is capped at 512 MiB, while compressed Sound decode is capped at 100 MiB.
4. **Buffered decode:** WAV and OGG music read incrementally; MP3 music keeps the compressed file in memory and decodes frame-by-frame during playback.
5. **Float WAV endpoints:** 32-bit float WAV samples map full-scale `-1.0` to `-32768` and `+1.0` to `32767` when converted to the engine's 16-bit mix format.
6. **MP3 decoder scope:** the from-scratch Layer III decoder implements the complete ISO 11172-3 / 13818-3 codec — all 32 Huffman pair tables and both count1 tables, MPEG-1 (with `scfsi`), MPEG-2 LSF and MPEG-2.5, mono / stereo / M-S and intensity joint stereo, long / short / mixed blocks, CBR and VBR. It is verified at ~80 dB SNR against independent decoders (`docs/internals/mp3-decoder-conformance.md`). A frame that selects a reserved codebook (4 or 14) is rejected as unsupported data.
7. **Encoding:** Use a tool such as ffmpeg to convert source audio between formats:
   ```
   ffmpeg -i input.wav output.ogg                      # OGG Vorbis
   ffmpeg -i input.wav -codec:a libmp3lame output.mp3 # MP3
   ffmpeg -i input.mp3 -ac 2 -f wav output.wav      # stereo music
   ffmpeg -i input.mp3 -ac 1 -f wav sfx.wav         # mono sound effect
   ```

---

## Limits and Behaviors

| Limit | Value | Notes |
|-------|-------|-------|
| Max simultaneous Sound voices | **32** | Oldest-started non-looping voice is evicted when full; looping voices are considered only if every voice loops |
| Max distinct loaded Sound buffers | **256** | Includes file-loaded, synthesized, and MusicGen-built Sounds |
| Max loaded Music streams | **4** | `Music.Load()` returns `null` when exceeded |
| Supported audio formats | **WAV, OGG Vorbis, MP3** | Sounds load fully into memory; music uses buffered incremental playback |
| Music sample rate | **1-384000 Hz** | Automatically resampled to 44100 Hz |
| Sound sample rate | **1-384000 Hz** | Resampled to 44100 Hz at load time |
| Pan range | −100 to +100 | −100 = hard left, 0 = center, +100 = hard right |
| Volume range | 0 to 100 | Applies to Sound, Music, and Voice |
| Max SoundBank entries | **64** | Keys are exact strings; long names remain distinct |
| Max MusicGen channels | **8** | Per song builder instance |
| Max MusicGen notes/channel | **4,096** | `AddNote()` returns 0 when full |
| Max MusicGen duration | **5 min** | `Build()` caps at 5 minutes |
| Max decoded compressed Sound data | **100 MiB** | OGG/MP3 Sounds that decode beyond this return `null` |
| Max eager WAV decoded data | **512 MiB** | Oversized WAV Sounds are rejected by the audio backend |

Behavior notes:
`Sound.Load(path)` and `Music.Load(path)` reject paths containing embedded `NUL` bytes and return `null` instead of passing a truncated path to the backend.
`Playlist.Add(path)` and `Playlist.Insert(index, path)` treat a null path as an empty string entry.
`Playlist.Insert(index, path)` clamps out-of-range indices to the valid `[0, Count]` insertion range.
`Music.CrossfadeTo` keeps the destination track's current loop setting instead of forcing one-shot playback.
`Music.CrossfadeTo` rejects detached handles after `Audio.Shutdown()`; load the asset again after reinitializing audio.
`Playlist.Shuffle` uses the runtime RNG, so `Zanna.Math.Random.Seed()` can make shuffle order reproducible.
Zero-length music streams fail to load rather than entering loop-rewind playback.

---

## See Also

- [Graphics](graphics/README.md) - Canvas and visual rendering
- [Input](input.md) - Keyboard and mouse input
- [Time](time.md) - Timing for audio synchronization
