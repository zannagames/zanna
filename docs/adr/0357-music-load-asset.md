---
status: accepted
audience: contributors
last-verified: 2026-09-13
---

# ADR 0357: Music streams from the asset manager

## Context

`Sound.LoadAsset` resolves audio through `Zanna.IO.Assets` (embedded image,
mounted ZPAK packs, then the loose filesystem), but `Music.Load` accepts only a
filesystem path: the ZannaAUD music backend opened WAV through a `FILE`, OGG
through a file-backed packet reader, and MP3 through a path-reading stream. A
game that ships its content in packs therefore had to ship every music track
as a loose companion tree beside the executable, outside the reviewed pack
inventory. Legacy Baseball carried 17 soundtrack files and its gameplay set
that way.

The MP3 stream already reads the whole encoded file into memory before
decoding frame by frame, and the OGG reader already has a memory constructor,
so "streaming" never depended on file I/O for the compressed formats — only on
how the bytes arrived.

## Decision

Add the static constructor `Zanna.Audio.Music.LoadAsset(name: String) -> Music`,
runtime function `rt_music_load_asset(rt_string) -> void*`, declared `owned`
with the result class `Zanna.Audio.Music`.

It loads the named asset's bytes through `rt_asset_load_raw` (so `asset://`
names, compressed pack entries and the loose fallback behave exactly as for
`Sound.LoadAsset`), detects the format from the magic bytes, and builds the
stream from memory. The handle plays, pauses, seeks, loops and crossfades
exactly like one from `Music.Load`. A missing, empty or undecodable asset
returns null; it never traps. The audio-disabled build traps with
"Music.LoadAsset: audio support not compiled in" (null name returns null), the
same contract as `Music.Load`.

Backend (ZannaAUD, not public ABI):

- `vaud_load_music_mem`, `vaud_load_music_ogg_mem`, `vaud_load_music_mp3_mem`
  copy the borrowed image, so the caller may free its buffer on return.
- A memory WAV stream keeps the image in `vaud_music::source_data` and converts
  the frames at the source cursor directly (`vaud_wav_decode_frames_mem`); no
  allocation or I/O on the refill path. File streams are unchanged.
- An OGG memory stream owns the image its packet reader borrows and releases the
  reader first.
- `mp3_stream_open_mem` shares the metadata scan with `mp3_stream_open`.

Memory cost is the encoded track size per open stream (a 4–5 MiB MP3 track;
at most two during a crossfade), which the MP3 path already paid.

Registry delta: one function. No IL, verifier, serialized-format or ownership
convention change.

## Validation

`test_vaud_core_fixes` drains memory and file WAV streams (mixer rate and a
resampled 48 kHz source) and compares every sample across a full pass, a
mid-stream seek and a rewind, after destroying the caller's image; it also
rejects null, empty and truncated images. `test_rt_audio_integration` mounts a
ZPAK holding a WAV, a stored MP3 and a deflated MP3, and checks duration, seek,
playback advance and the missing-asset null. `test_rt_audio_surface_link` links
the symbol and `test_rt_audio_unavailable` pins the disabled-build contract.
Legacy Baseball streams its soundtrack from `legacy-baseball-audio.zpak`.
