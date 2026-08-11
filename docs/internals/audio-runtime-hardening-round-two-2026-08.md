---
status: complete
audience: contributors
last-verified: 2026-08-11
---

# Audio Runtime Hardening, Round Two — August 2026

## Objective and scope

This ledger records the second, non-duplicative 100-item audio-runtime
hardening round (`AUD-101` through `AUD-200`). It continues after the codec and
mixer work in the [first-round ledger](audio-runtime-hardening-2026-08.md) and
focuses on container detection, decoder-wrapper contracts, procedural
synthesis, insert effects, sound banks, music generation, playlists, and music
crossfades. The runtime C ABI and IL surface remain unchanged.

A row is counted only when it has a concrete safety/correctness/performance
failure mode and a corresponding source change. Status becomes `verified` only
after the complete validation matrix at the end of this document passes.

## Finding ledger

| ID | Area | Finding and implemented change | Evidence | Status |
|---:|---|---|---|:---:|
| AUD-101 | Decode | A four-byte `RIFF` prefix misclassified arbitrary RIFF files as audio; require the `WAVE` form type. | `rt_audio_decode.c::detect_audio_format_mem` | verified |
| AUD-102 | Decode | Unsupported Ogg bitstream versions passed signature detection; require capture version zero. | `detect_audio_format_mem` | verified |
| AUD-103 | Decode | A truncated four-byte `ID3` prefix was accepted as MP3; require the complete ten-byte ID3v2 header. | `detect_audio_format_mem` | verified |
| AUD-104 | Decode | Reserved/unsupported ID3 major versions were accepted; allow only ID3v2.2 through v2.4. | `detect_audio_format_mem` | verified |
| AUD-105 | Decode | The reserved ID3 revision value `0xff` passed detection; reject it. | `detect_audio_format_mem` | verified |
| AUD-106 | Decode | Non-syncsafe ID3 size fields passed detection; reject any high bit in the four size bytes. | `detect_audio_format_mem` | verified |
| AUD-107 | Decode | A sync-looking MP3 header with reserved MPEG version passed detection; validate the version field. | `detect_audio_format_mem` | verified |
| AUD-108 | Decode | A sync-looking MP3 header with reserved layer passed detection; validate the layer field. | `detect_audio_format_mem` | verified |
| AUD-109 | Decode | Free/reserved bitrate indices were treated as supported MP3 frames; require a supported table index. | `detect_audio_format_mem` | verified |
| AUD-110 | Decode | Reserved MP3 sample-rate indices passed detection; reject index three. | `detect_audio_format_mem` | verified |
| AUD-111 | Decode | Reserved MP3 emphasis passed the lightweight detector; reject it before decoder allocation. | `detect_audio_format_mem` | verified |
| AUD-112 | Decode | File-format detection passed null/empty paths to the filesystem adapter; reject them locally. | `rt_audio_decode.c::detect_audio_format` | verified |
| AUD-113 | Decode | File detection read only four bytes and could not validate RIFF/ID3 structure; probe twelve bytes. | `detect_audio_format` | verified |
| AUD-114 | Decode | The common Ogg reader conversion retained stale outputs on early failure; clear both outputs first. | `ogg_decode_reader_to_wav` | verified |
| AUD-115 | Decode | File-backed Ogg open failures retained caller sentinels; validate outputs/path and clear outputs first. | `ogg_file_to_wav` | verified |
| AUD-116 | Decode | Memory-backed Ogg open failures retained caller sentinels; validate data/size/outputs and clear first. | `ogg_mem_to_wav` | verified |
| AUD-117 | Decode | Memory MP3 validation/allocation failures retained caller sentinels; clear valid outputs before work. | `mp3_data_to_wav` | verified |
| AUD-118 | Decode | File MP3 open/seek/read failures retained caller sentinels; clear outputs and reject null arguments first. | `mp3_file_to_wav` | verified |
| AUD-119 | Decode | Decoded PCM was copied to WAV in host byte order; emit each signed sample explicitly as little-endian. | `build_wav_from_pcm` | verified |
| AUD-120 | Decode | Tiny Ogg streams immediately reserved 65,536 frames; start at 4,096 and retain geometric growth. | `ogg_decode_reader_to_wav` | verified |
| AUD-121 | Synth | WAV sizing failures left optional sizes stale; reset both destinations before validation. | `rt_synth.c::synth_wav_sizes` | verified |
| AUD-122 | Synth | Duration conversion failures left a stale sample count; reset it before validation. | `synth_duration_to_samples` | verified |
| AUD-123 | Synth | Procedural PCM was copied to WAV in host byte order; encode samples explicitly as little-endian. | `samples_to_sound` | verified |
| AUD-124 | Synth | Tone synthesis allocated/rendered a full buffer when audio was unavailable; fail before work. | `rt_synth_tone` | verified |
| AUD-125 | Synth | Sweep synthesis allocated/rendered a full buffer when audio was unavailable; fail before work. | `rt_synth_sweep` | verified |
| AUD-126 | Synth | Noise synthesis allocated/rendered a full buffer when audio was unavailable; fail before work. | `rt_synth_noise` | verified |
| AUD-127 | Synth | Preset dispatch could enter custom preset rendering when audio was unavailable; fail at the dispatcher. | `rt_synth_sfx` | verified |
| AUD-128 | Synth | Tone oscillator phase grew for the full render and relied on risky floating-to-integer normalization; wrap each step. | `rt_synth_tone`, `synth_advance_phase` | verified |
| AUD-129 | Synth | Sweep oscillator phase had the same unbounded-growth/conversion risk; keep it normalized per sample. | `rt_synth_sweep` | verified |
| AUD-130 | Synth | The two-tone coin preset also accumulated unbounded phase; use the common bounded advance. | `sfx_coin` | verified |
| AUD-131 | Synth | Sweep interpolation divided by `num_samples`, never reaching the requested end frequency; use `num_samples - 1`. | `rt_synth_sweep` | verified |
| AUD-132 | Synth | Tone recomputed its constant fade length for every sample; hoist it out of the render loop. | `rt_synth_tone` | verified |
| AUD-133 | Synth | Sweep recomputed its constant fade length for every sample; hoist it out of the render loop. | `rt_synth_sweep` | verified |
| AUD-134 | Synth | LCG high-word conversion to `int16_t` was implementation-defined above `INT16_MAX`; convert through bounded `int32_t`. | `rt_synth_noise` | verified |
| AUD-135 | Synth | Noise decay never reached its endpoint and recomputed constants per sample; use the final-frame denominator and hoisted fade length. | `rt_synth_noise` | verified |
| AUD-136 | Effects | Feedback sanitation called `fabsf` before rejecting NaN/Inf; reject non-finite values first. | `rt_audio_fx.c::sanitize_feedback` | verified |
| AUD-137 | Effects | Failed reverb-line allocation could leave stale structural outputs; initialize the whole line before allocation. | `alloc_reverb_line` | verified |
| AUD-138 | Effects | Non-finite biquad input entered coefficient arithmetic; normalize input before updating state. | `biquad_sample` | verified |
| AUD-139 | Effects | A previously poisoned biquad history permanently silenced future clean blocks; repair histories before use. | `biquad_sample` | verified |
| AUD-140 | Effects | Biquad history updates could store newly generated NaN/Inf; sanitize both state writes. | `biquad_sample` | verified |
| AUD-141 | Effects | The private biquad block path accepted non-positive frame counts; reject them consistently. | `process_biquad` | verified |
| AUD-142 | Effects | The private delay block path accepted non-positive frame counts; reject them consistently. | `process_delay` | verified |
| AUD-143 | Effects | A damaged delay cursor indexed outside its ring; reset it before the first dereference. | `process_delay` | verified |
| AUD-144 | Effects | Damaged delay feedback/wet/dry state entered realtime arithmetic; re-clamp feedback/wet and derive dry. | `process_delay` | verified |
| AUD-145 | Effects | Non-finite delay input could suppress valid delayed output and contaminate feedback; sanitize both channels first. | `process_delay` | verified |
| AUD-146 | Effects | Non-finite retained delay samples entered mix arithmetic; repair ring values as they are read. | `process_delay` | verified |
| AUD-147 | Effects | Damaged comb cursors indexed outside reverb delay lines; repair each cursor before dereference. | `process_comb` | verified |
| AUD-148 | Effects | Non-finite comb excitation entered the reverb network; sanitize it locally. | `process_comb` | verified |
| AUD-149 | Effects | A poisoned comb filter value persisted indefinitely; repair it before the one-pole update. | `process_comb` | verified |
| AUD-150 | Effects | Damaged all-pass cursors indexed outside their rings; repair each cursor before dereference. | `process_allpass` | verified |
| AUD-151 | Effects | Non-finite all-pass input or retained samples propagated through the network; sanitize both before arithmetic. | `process_allpass` | verified |
| AUD-152 | Effects | Partial/corrupt reverb line storage could be dereferenced in the sample loop; validate all 24 stereo pairs once per block. | `reverb_lines_valid`, `process_reverb` | verified |
| AUD-153 | Effects | Damaged room/damping/wet/dry state entered realtime arithmetic; clamp the public ranges and rederive dry. | `process_reverb` | verified |
| AUD-154 | Effects | Non-finite reverb input could poison the excitation sum; sanitize both dry channels and the mono sum. | `process_reverb` | verified |
| AUD-155 | Effects | Effect chains had no bound on retained memory or callback work; cap each group at 32 inserts. | `append_fx`, `rt_audio_fx.h` | verified |
| AUD-156 | Effects | `ClearAll` exposed partially cleared global state one group at a time; detach every chain under one lock and free afterward. | `rt_audio_fx_clear_all` | verified |
| AUD-157 | Effects | Frame/channel index products could exceed `size_t` on 32-bit targets; reject unrepresentable blocks. | `rt_audio_fx_process_group` | verified |
| AUD-158 | SoundBank | The cached entry count could drift from authoritative slots; add a bounded recount helper. | `rt_soundbank.c::soundbank_recount` | verified |
| AUD-159 | SoundBank | Finalization released slots without resetting aggregate state; zero the count after release. | `rt_soundbank_finalize` | verified |
| AUD-160 | SoundBank | Register-by-path performed I/O/decode before discovering a full bank; preflight replacement/free capacity first. | `rt_soundbank_register` | verified |
| AUD-161 | SoundBank | Path registration incremented a potentially stale count; recompute after publishing the slot. | `rt_soundbank_register` | verified |
| AUD-162 | SoundBank | Handle registration incremented a potentially stale count; recompute after publishing the slot. | `rt_soundbank_register_sound` | verified |
| AUD-163 | SoundBank | Removal blindly decremented the cached count and could underflow damaged state; recount after clearing. | `rt_soundbank_remove` | verified |
| AUD-164 | SoundBank | The public count getter exposed stale/out-of-range state; return a fresh bounded recount. | `rt_soundbank_count` | verified |
| AUD-165 | MusicGen | Centbeat conversion failures left a stale frame result; reset it before validation. | `rt_musicgen.c::mg_centbeats_to_frames` | verified |
| AUD-166 | MusicGen | Sine normalization converted arbitrary growing doubles to `int64_t`; require normalized phases and remove the cast. | `mg_sin` | verified |
| AUD-167 | MusicGen | Waveform normalization repeated the same risky double-to-integer cast; remove it and centralize phase advance. | `mg_waveform`, `mg_advance_phase` | verified |
| AUD-168 | MusicGen | Main waveform phase grew across every note; wrap it after each sample. | `mg_render_note` | verified |
| AUD-169 | MusicGen | Vibrato phase grew across every note; wrap it after each modulation sample. | `mg_render_note` | verified |
| AUD-170 | MusicGen | Tremolo phase grew across every note; wrap it after each modulation sample. | `mg_render_note` | verified |
| AUD-171 | MusicGen | The private noise initializer dereferenced null state; make it inert for invalid input. | `mg_noise_init` | verified |
| AUD-172 | MusicGen | The private noise sampler dereferenced null state; return silence for invalid input. | `mg_noise_sample` | verified |
| AUD-173 | MusicGen | Invalid noise cutoff values could destabilize the one-pole filter; clamp to `[1, Nyquist]`. | `mg_noise_sample` | verified |
| AUD-174 | MusicGen | Noise LCG narrowing to `int16_t` was implementation-defined; convert the high word through bounded `int32_t`. | `mg_noise_sample` | verified |
| AUD-175 | MusicGen | WAV sizing failures left optional sizes stale; reset both destinations first. | `mg_wav_sizes` | verified |
| AUD-176 | MusicGen | A negative retained channel count made `AddChannel` write before the channel array; reject it. | `rt_musicgen_add_channel` | verified |
| AUD-177 | MusicGen | Channel lookup trusted a retained count above the fixed array capacity; validate the aggregate before indexing. | `mg_get_channel` | verified |
| AUD-178 | MusicGen | A negative retained note count made `AddNote` write before the note array; reject it. | `rt_musicgen_add_note_vel` | verified |
| AUD-179 | MusicGen | The BPM getter exposed retained values outside builder limits; return the supported range. | `rt_musicgen_get_bpm` | verified |
| AUD-180 | MusicGen | The length getter exposed negative/over-cap retained values; clamp against tempo and the five-minute limit. | `rt_musicgen_get_length` | verified |
| AUD-181 | MusicGen | The channel-count getter exposed negative/over-cap retained values; clamp to fixed storage. | `rt_musicgen_get_channel_count` | verified |
| AUD-182 | MusicGen | Retained channel settings could bypass public clamps and overflow render arithmetic; validate every setting before allocation/rendering. | `mg_channel_renderable` | verified |
| AUD-183 | MusicGen | A damaged BPM could divide by zero or produce invalid frame timing; validate it in `Build`. | `rt_musicgen_build` | verified |
| AUD-184 | MusicGen | `Build` trusted channel counts above fixed storage; reject them before either scan. | `rt_musicgen_build` | verified |
| AUD-185 | MusicGen | `Build` trusted a retained song length beyond its tempo-specific cap; reject it. | `rt_musicgen_build` | verified |
| AUD-186 | MusicGen | `Build` trusted retained swing outside `[0,100]`, risking shift arithmetic; reject it. | `rt_musicgen_build` | verified |
| AUD-187 | MusicGen | Empty/silent songs allocated the full five-minute accumulator before producing no audio; detect no active channel first. | `rt_musicgen_build` | verified |
| AUD-188 | MusicGen | Channels without portamento allocated/copied/sorted every multi-note array unnecessarily; keep insertion order when sorting is not needed. | `rt_musicgen_build` | verified |
| AUD-189 | MusicGen | Rendered stereo PCM was copied to WAV in host byte order; emit every sample explicitly as little-endian. | `rt_musicgen_build` | verified |
| AUD-190 | Playlist | Shuffle mapping trusted a stale order with a different cardinality than the track list; require equal lengths. | `rt_playlist.c::get_track_index` | verified |
| AUD-191 | Playlist | Shuffle payloads were returned without checking the actual track range; reject invalid mapped indices. | `get_track_index` | verified |
| AUD-192 | Playlist | A missing current actual track silently fell back to shuffle slot zero; clear the selection instead. | `playlist_set_current_from_actual` | verified |
| AUD-193 | Playlist | Shared position selection accepted invalid/stale cursors; release music and reset state on invalid targets. | `playlist_select_position` | verified |
| AUD-194 | Playlist | Skip-on-error scans used `current + 1` without protecting retained extreme cursors; use subtraction/range-based advancement. | `playlist_select_position`, `rt_playlist_play` | verified |
| AUD-195 | Playlist | Removing the current shuffled track selected an arbitrary entry after reshuffle; preserve its pre-removal logical successor. | `rt_playlist_remove` | verified |
| AUD-196 | Playlist | `Next` could overflow an extreme retained cursor; derive the boundary case before incrementing. | `rt_playlist_next` | verified |
| AUD-197 | Playlist | `Previous` could underflow an extreme retained cursor; derive the boundary case before decrementing. | `rt_playlist_prev` | verified |
| AUD-198 | Playlist | Public current/boolean/volume/shuffle/repeat queries exposed invalid retained state; normalize every result. | Playlist property getters | verified |
| AUD-199 | Playlist | Updating an idle playlist called process-global audio maintenance; return before that work when no track is active. | `rt_playlist_update` | verified |
| AUD-200 | Crossfade | Extreme durations could retain two streams effectively forever; clamp direct and playlist crossfades to one hour and document it. | `rt_audio.c`, `rt_playlist.c`, `audio.md` | verified |

## Regression coverage

- `test_rt_audio_integration`: strict RIFF/Ogg/ID3/MP3 signature detection,
  decoder failure-output contracts, bounded playlist crossfade values, and
  shuffled-current removal semantics.
- `test_rt_audio_fx`: the 32-insert ceiling and recovery after NaN input for
  biquad, delay, and reverb chains.
- `test_rt_soundbank`: fixed capacity, full-bank rejection, replacement at
  capacity, and slot reuse after removal.
- `test_rt_musicgen`: empty-song behavior, boundary values, every waveform and
  modulation path, maximum length/note boundaries, and successful rendering.
- `test_rt_audio_unavailable`: early builder failure behavior for synth and
  MusicGen in audio-disabled builds.

## Validation matrix

Targeted iteration:

```sh
ctest --test-dir build -R '^(test_rt_audio_integration|test_rt_audio_fx|test_rt_soundbank|test_rt_musicgen|test_rt_audio_unavailable)$' --output-on-failure
```

Cross-platform and policy checks:

```sh
./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh
```

## Completed validation

- Focused runtime tests passed for audio integration, effects, sound banks, and
  MusicGen; the audio-unavailable case skipped as designed in the audio-enabled
  build.
- Strict-warning AddressSanitizer and UndefinedBehaviorSanitizer builds passed
  all four focused runtime test executables.
- `lint_platform_policy.sh` completed cleanly.
- `run_cross_platform_smoke.sh` passed, including the audio-disabled surface
  link and native arm64 application probes.
- `ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_INSTALL=1 ./scripts/build_zanna_mac.sh`
  rebuilt the full project with warnings as errors, compiled the Zanna Studio
  native payload, passed all 1,968 enabled CTests, passed the runtime surface
  audit, and repeated the policy and smoke gates successfully. Installation was
  deliberately skipped because this round does not alter packaging.
