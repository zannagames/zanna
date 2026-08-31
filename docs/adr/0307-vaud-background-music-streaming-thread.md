---
status: active
audience: contributors
last-verified: 2026-08-31
---

# ADR 0307: Background Music Streaming Thread

## Status

Accepted (2026-08-31)

## Context

`Zanna.Audio.Music` streams from disk through a small decoded ring:
`VAUD_MUSIC_BUFFER_COUNT` (3) buffers of `VAUD_MUSIC_BUFFER_FRAMES` (8192)
frames — about 557 ms of lookahead. Every refill happened inside
`vaud_update()`, reached from Zia only through `Zanna.Audio.Mixer.Update()` on
the app thread. The realtime mixer is consume-only (ADR-era hardening in
`vaud_mixer.c` removed all decode from the render path), so any app-thread
stall longer than the ring — a loading screen's asset batch, a long frame, or
`Sound.Load` decoding a whole MP3 while holding `audio_state_lock` (the same
lock `rt_audio_update` needs) — drained the ring and produced silence while
`IsPlaying()` kept reporting `true`. Legacy Baseball's boot soundtrack audibly
stuttered during preload, and its season day-sim screen stalled menu music
entirely. The documented contract ("a program that never calls `Update()`
hears the prefill and then silence") pushed every host loop into hand-pumping
audio through code paths that cannot always run.

## Decision

Give every `vaud_context` a background music streamer thread:

- `vaud_create()` starts one low-duty thread (`VAUD_STREAM_THREAD_ENABLE`,
  default on) that calls the shared refill pass
  (`vaud_update_service_refills`) every `VAUD_STREAM_THREAD_INTERVAL_MS`
  (default 50 ms), sleeping in 1 ms slices so `vaud_destroy()` joins promptly.
  Thread-creation failure is non-fatal — the context works with app-thread
  `vaud_update()` as the only pump, exactly as before.
- `vaud_destroy()` stops and joins the streamer immediately after setting the
  teardown flags, before platform shutdown and stream detach.
- Refill exclusivity needs no new locking: per-stream `refill_in_progress`
  and per-slot `buffer_refilling[]` claims are taken under the context mutex,
  so the streamer and app-thread `vaud_update()` never decode the same stream.
  The streamer takes no runtime-level locks (`audio_state_lock` in
  particular), so `Sound.Load` can no longer starve music.
- Two latent races become real with a second refiller and are fixed here:
  `vaud_music_clear_buffers` no longer zeroes `buffer_refilling[]` (it runs in
  the unlocked window of a forced refill; clearing the claims let the realtime
  mixer advance `current_buffer` mid-refill), and the "wait for refill, then
  relock" pattern in play/stop/seek/free/detach is replaced by
  `vaud_music_lock_no_refill()`, which returns holding the mutex with the
  no-refill condition observed under that same hold — free/detach remove the
  stream from the registry inside that hold, so the streamer can never claim a
  stream being torn down.
- `Mixer.Update()` keeps its signature and remains required each frame for
  crossfade envelopes and playlist auto-advance; ring refills through it are
  now a best-effort top-up. No IL, verifier, or runtime C ABI surface changes.

Alternatives considered: a larger ring (still fails on multi-second stalls,
adds latency to every seek/play prefill), condition-variable wakeups (more
sync surface for no audible benefit at a 557 ms ring), and decoding on the
platform render thread (violates the consume-only realtime contract).

## Consequences

- Music survives arbitrary app-thread stalls; loading screens and blocking
  work no longer cause dropouts in any zanna program.
- `docs/zannalib/audio.md` and the `vaud_update` doc block are rewritten:
  `Update()` is documented as the crossfade/playlist pump, and the stale claim
  that the mixer performs a locked prefill is removed.
- Each context owns one extra mostly-sleeping thread (~20 wakes/s scanning at
  most `VAUD_MAX_MUSIC` streams); decode work moves off the app thread.
- Playlist auto-advance still requires app-thread `Playlist.Update()`; a
  stalled app thread keeps audio playing but defers track changes.

## Tests

`src/tests/unit/runtime/TestMusicStreamThread.cpp` (`test_music_stream_thread`):
a looping MP3 fixture plays past the ~557 ms prefill ceiling with zero
`vaud_update()` calls from the test thread (fails before this ADR), and
repeated play/stop/free cycles under the live streamer stay safe.
`RTAudioIntegrationTests.cpp` adds the same stall regression at the runtime
layer (`rt_music_play` with no `rt_audio_update`).
