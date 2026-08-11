---
status: active
audience: contributors
last-verified: 2026-08-11
---

# Audio Runtime Hardening — August 2026

## Summary and objective

This is the working audit ledger for the 100-item audio runtime hardening
campaign. The objective is to make the dependency-free native audio stack
robust against malformed media, hostile sizes and parameters, allocation and
I/O failures, lifecycle races, and realtime performance regressions while
preserving the existing runtime C ABI.

The ledger counts a finding only when it has a concrete failure mode or a
measurable avoidable cost, an implementation change, and regression coverage
where the behavior is testable without physical audio hardware.

## Scope

In scope:

- `src/lib/audio`: WAV parsing, eager/streaming conversion, mixing, lifecycle,
  and the ALSA, AudioQueue, and WASAPI adapters.
- `src/runtime/audio`: runtime wrappers, Ogg/Vorbis/MP3 decoding, DSP, synthesis,
  playlists, sound banks, and 3D audio state.
- Audio-focused CTests, sanitizer/static-analysis findings, and internal/user
  documentation needed to define hardened behavior.

Out of scope for ABI-preserving batches:

- New or changed IL opcodes, verifier rules, language grammar, or runtime C ABI
  entries. Any such finding is held for an ADR before implementation.
- External codec, DSP, threading, or platform libraries. Zanna remains
  dependency-free apart from operating-system audio APIs.
- CI workflow changes.

## Feature toggle

Not required. These changes tighten existing behavior and preserve supported
inputs; allowing callers to re-enable unsafe parsing or unchecked arithmetic
would defeat the hardening objective.

## Configuration

None. Existing audio availability and silent-output configuration remains
unchanged.

## Error handling

- Malformed or structurally inconsistent WAV inputs fail with the existing
  `VAUD_ERR_FORMAT` category and a stable explanatory message.
- Null API parameters retain the existing `VAUD_ERR_INVALID_PARAM` behavior.
- Every valid output pointer is reset to an inert value before work that may
  fail, so callers never observe stale ownership-bearing results.
- Internal streaming conversion rejects unsupported layouts before reading or
  modifying caller buffers.
- Arithmetic that cannot be represented returns the existing failure value and
  performs no reads or writes.
- Vorbis header state advances only after one complete, correctly ordered
  packet has been validated. Truncation is sticky within a packet and cannot
  manufacture zero-valued fields.
- Failed Vorbis audio-packet calls publish null PCM and zero samples. Malformed
  entropy data is fatal for the packet rather than decoded as partial audio.

## Test strategy

- Given a malformed in-memory WAV and sentinel outputs, when loading fails,
  then every output is reset and no ownership is transferred.
- Given a declared RIFF boundary that is smaller or larger than the supplied
  container, when parsing reaches inconsistent data, then parsing fails.
- Given unsupported channel, bit-depth, or encoding values, when the buffered
  streaming converter is called, then it returns zero without consuming input
  or modifying output.
- Given unrepresentable PCM/resampler dimensions, when size or resampling
  helpers are called, then they return without stale output or signed overflow.
- Given a one-frame source, when resampled to any positive length, then the
  source frame is reproduced exactly in every output frame.
- Given truncated, reordered, repeated, unframed, or oversized Vorbis headers,
  when they are submitted, then parsing fails without advancing header state.
- Given corrupt Vorbis floor or residue entropy data, when audio decoding
  exhausts the packet, then decoding fails with inert output values.
- Given non-finite decoded samples, when PCM conversion runs, then NaN becomes
  silence and infinities saturate without undefined float-to-integer casts.
- Given corrupt mixer cursors, ring indices, registry counts, or DSP state,
  when a render runs, then state is repaired or retired before any array index,
  coefficient calculation, or PCM conversion.
- Given a saturated mixer clock, a paused context, or a null render context,
  when allocation/rendering occurs, then voice stealing remains deterministic
  and every valid output or fallback buffer is initialized safely.

## Finding ledger

Status values are `planned`, `implemented`, and `verified`.

| ID | Severity | Finding and change | Evidence | Status |
|---:|:---:|---|---|:---:|
| AUD-001 | High | Bound in-memory chunk traversal to the declared RIFF container instead of the entire supplied buffer. | `vaud_wav.c::parse_wav_header` used `size` as its only parse limit. | verified |
| AUD-002 | High | Reject a declared RIFF size that extends beyond the supplied in-memory buffer. | The RIFF size field at byte 4 was ignored. | verified |
| AUD-003 | Medium | Reject undersized RIFF containers that cannot contain the WAVE form type and chunks. | The RIFF size field was not structurally validated. | verified |
| AUD-004 | High | Apply the declared RIFF boundary to file-backed WAV chunk traversal. | `parse_wav_stream` bounded chunks only by physical file size, allowing trailing bytes to become chunks. | verified |
| AUD-005 | Medium | Reject duplicate `fmt ` chunks instead of silently replacing format metadata. | Both parsers overwrote `vaud_wav_info` on repeated `fmt ` chunks. | verified |
| AUD-006 | Medium | Reject duplicate `data` chunks instead of silently selecting whichever was encountered last before parsing stopped. | Both parsers reassigned `data_offset` and `data_size`. | verified |
| AUD-007 | Medium | Reset all in-memory WAV loader outputs before parsing can fail. | `vaud_wav_load_mem` left caller sentinels untouched on failure, unlike the file loader. | verified |
| AUD-008 | Low | Reset `vaud_pcm_s16_buffer_size` output on every failure path. | The helper could return false with a stale byte count. | verified |
| AUD-009 | High | Reject unsupported channel counts, sample widths, and WAV encoding identifiers in raw streaming conversion before reading input. | `vaud_wav_read_frames[_buffered]` accepted arbitrary positive values and decoded them as a different layout or silence. | verified |
| AUD-010 | High | Validate resampler input/output index products before entering interpolation loops. | `frame * channels + channel` and `out_idx * channels + ch` used signed products without a representability gate. | verified |
| AUD-011 | Low | Fast-path one-frame resampling by copying the constant frame rather than evaluating four-point cubic interpolation for every output sample. | Cubic edge clamping makes every tap identical for a one-frame input. | verified |
| AUD-012 | High | Reject Ogg page header flags outside the defined continuation/BOS/EOS mask. | `ogg_read_page` accepted all reserved header-type bits. | verified |
| AUD-013 | High | Reject an Ogg page marked as both BOS and a continuation. | The state machine treated this contradictory combination as a resynchronizing continuation. | verified |
| AUD-014 | High | Reject a repeated BOS page for an already-seen logical stream. | `process_page_packets` did not validate BOS against existing stream state. | verified |
| AUD-015 | High | Preserve BOS metadata when the first logical packet spans multiple pages. | BOS was published only when a packet completed on the BOS page, so continued identification packets lost the flag. | verified |
| AUD-016 | High | Reject EOS pages that leave an unterminated packet. | The parser allowed a continuation after an EOS page to complete and publish the packet. | verified |
| AUD-017 | Medium | Bound capture-pattern resynchronization work per page read. | A single call could scan an arbitrarily large junk prefix byte by byte. | verified |
| AUD-018 | Medium | Bound the number of no-packet pages consumed by one packet request. | A container with valid empty pages could monopolize a caller until physical EOF. | verified |
| AUD-019 | Low | Initialize the storage sentinel used for a successful empty Ogg packet. | The non-NULL one-byte allocation was returned with indeterminate contents. | verified |
| AUD-020 | Low | Pop a newly queued packet directly instead of recursively re-entering `ogg_reader_next_packet_ex`. | Every page-read success ended in an avoidable recursive call. | verified |
| AUD-021 | High | Make Vorbis bit-reader truncation sticky and return no partial field value. | `bits_read` returned an available prefix at EOF, allowing missing high bits to masquerade as zeros. | verified |
| AUD-022 | High | Accept a Huffman codeword that consumes exactly the final packet bit. | `codebook_decode_scalar` treated the valid end cursor as failure after reading a multi-bit code. | verified |
| AUD-023 | Medium | Require the identification header's exact 30-byte packet size. | `decode_identification` accepted arbitrary trailing bytes. | verified |
| AUD-024 | High | Validate the identification header framing byte. | Byte 29 was never examined, so unframed identification packets advanced decoder state. | verified |
| AUD-025 | High | Enforce the Vorbis I block-size exponent range of 6 through 13 before shifting. | Exponents 14 and 15 allocated unsupported 16K/32K decoder windows and overlap buffers. | verified |
| AUD-026 | High | Enforce identification, comment, and setup header order. | `vorbis_decode_header` independently ORed state bits and accepted comment/setup packets first. | verified |
| AUD-027 | High | Reject repeated Vorbis headers. | Repeated identification/setup parsing overwrote owned allocations and could leak prior decoder state. | verified |
| AUD-028 | High | Bound the comment vendor string by the remaining packet bytes. | `decode_comment` validated only the seven-byte signature and ignored the vendor length. | verified |
| AUD-029 | High | Bound the comment-list count and every length-prefixed user comment. | Truncated or fabricated comment lists were accepted without parsing. | verified |
| AUD-030 | Medium | Require exactly one valid comment framing byte after the parsed fields. | Missing framing and arbitrary trailing data were accepted. | verified |
| AUD-031 | High | Reject zero-progress ordered-codebook runs. | A truncated ordered codebook repeatedly decoded a zero run and could loop indefinitely. | verified |
| AUD-032 | High | Stop ordered-codebook parsing when the code length exceeds 32 bits. | Malformed run counts could advance `current_length` beyond the representable Huffman width. | verified |
| AUD-033 | High | Reject a setup header whose framing read reflects prior or current bitstream truncation. | EOF reads returned zero without distinguishing malformed data from a legitimate zero field. | verified |
| AUD-034 | High | Reject non-finite codebook minimum and delta scalars. | Crafted Vorbis float fields could introduce infinities into VQ expansion and downstream PCM conversion. | verified |
| AUD-035 | High | Reject non-finite values produced while expanding a codebook VQ table. | Finite operands can overflow their multiplication/addition and poison an entire decoded block. | verified |
| AUD-036 | High | Validate Vorbis packet output pointers and clear valid outputs before every failure path. | Early failures retained caller sentinels; fully initialized decoding would dereference null output pointers. | verified |
| AUD-037 | High | Check bit-reader failure after packet mode, window, amplitude, and floor-value reads. | Truncated audio packets could continue with synthesized zero fields. | verified |
| AUD-038 | High | Abort floor decoding when a masterbook or subclass Huffman decode fails. | A `-1` masterbook value was masked into a subclass index and a failed subclass value entered floor synthesis. | verified |
| AUD-039 | High | Clamp residue begin/end to the current half-block before sizing classification storage. | A 24-bit residue range could request hundreds of megabytes for a block with at most 4096 spectral bins. | verified |
| AUD-040 | High | Treat truncated residue classword/vector data as a packet failure. | Failure labels previously continued into coupling and IMDCT with partial residue buffers. | verified |
| AUD-041 | High | Allocate a larger PCM buffer before releasing the reusable old buffer. | On allocation failure, capacity remained nonzero while `pcm_out` became null, enabling a later null write. | verified |
| AUD-042 | High | Saturate non-finite and out-of-range samples before float-to-integer conversion. | Casting NaN, infinity, or an out-of-range float to `int` is undefined behavior. | verified |
| AUD-043 | Low | Remove an empty nested residue classification pass. | The loop visited every partition/channel pair without reading or writing state. | verified |
| AUD-044 | Low | Make the bounded residue cascade narrowing conversion explicit. | Strict conversion diagnostics flagged the implicit `int`-to-`uint8_t` assignment. | verified |
| AUD-045 | High | Make MP3 bit-reader truncation sticky and discard partial fields. | `mp3_bits_read` returned an accumulated prefix at EOF and callers treated it as a complete field. | verified |
| AUD-046 | High | Reject MP3 bit requests above 32 bits and guard byte-to-bit length conversion. | The reader accepted arbitrary positive counts even though shifts and its return type are 32-bit. | verified |
| AUD-047 | Medium | Null-check and clear the private MP3 frame-header output before parsing. | `mp3_parse_header` dereferenced both pointers unconditionally and retained partial fields after failure. | verified |
| AUD-048 | High | Account for the optional two-byte CRC in protected-frame payload sizing. | Frame `main_data_size` always subtracted only the four-byte header and side information. | verified |
| AUD-049 | Medium | Reject the reserved MPEG audio emphasis value. | Header emphasis bits `10` were accepted as a supported frame. | verified |
| AUD-050 | High | Require the exact version/channel-dependent side-information size and valid parser arguments. | `mp3_parse_side_info` ignored its size contract and always reported success. | verified |
| AUD-051 | Medium | Clear private side-information output before every failure. | Callers inspecting a rejected parse could retain ownership-neutral but stale structural fields. | verified |
| AUD-052 | High | Reject Layer III `big_values` counts above the 288-pair limit. | The 9-bit field was accepted up to 511 and only indirectly clamped later. | verified |
| AUD-053 | High | Reject window-switched side information with reserved block type zero. | The decoder treated the forbidden combination as a long block. | verified |
| AUD-054 | High | Fail side-information parsing when its final bit cursor is truncated. | Missing tail fields were synthesized as zero and the parser returned success. | verified |
| AUD-055 | High | Treat truncated ID3v2 prefixes and unsupported versions/reserved flags as malformed metadata. | The scanner either ignored a short `ID3` prefix or trusted arbitrary version/flag bytes. | verified |
| AUD-056 | High | Reject ID3v2 size bytes with their non-syncsafe high bit set. | High bits were silently masked, potentially moving audio scanning into tag payload. | verified |
| AUD-057 | Medium | Include the optional ID3v2.4 footer when computing the first audio offset. | Footer-bearing tags advanced only past the header and declared body. | verified |
| AUD-058 | Medium | Reset all metadata-scan outputs before validation. | Failed pre-scans could leave stale offsets and stream format values. | verified |
| AUD-059 | High | Reject channel-count or sample-rate changes during MP3 metadata pre-scan. | The pre-scan summed incompatible frames and deferred failure until decoding. | verified |
| AUD-060 | High | Detect total-sample integer overflow while walking MP3 frames. | Repeated `int` addition could overflow before the decoded-byte ceiling was applied. | verified |
| AUD-061 | Medium | Use subtraction-based frame-bound checks in metadata and frame decoding. | `position + frame_size` checks were vulnerable to unsigned wrap at representational limits. | verified |
| AUD-062 | Medium | Clear per-frame sample/channel/rate outputs before every internal decode failure. | The private frame decoder published only on success but did not clear caller sentinels. | verified |
| AUD-063 | High | Start protected-frame side information after the two CRC bytes. | CRC bytes were previously parsed as `main_data_begin` and granule fields. | verified |
| AUD-064 | High | Reject a frame whose `main_data_begin` exceeds available reservoir history. | The decoder silently substituted current-frame bytes and emitted incorrect audio. | verified |
| AUD-065 | High | Retain the most recent bytes when one reservoir append exceeds its capacity. | The old truncation kept the oldest prefix, which cannot satisfy future backward references. | verified |
| AUD-066 | High | Reject an over-capacity combined reservoir/frame payload instead of truncating it. | Silent truncation changed the entropy bitstream while continuing decode. | verified |
| AUD-067 | High | Bound every scalefactor and entropy read to its granule/channel `part2_3_length`. | Reads could cross into the next part, then rewind the cursor back to the declared end. | verified |
| AUD-068 | High | Propagate Huffman tree/bit-reader failure instead of substituting a zero pair. | A malformed tree walk was converted to spectral silence and decoding continued. | verified |
| AUD-069 | High | Roll back overlap and synthesis-filter state after a late frame failure. | A second-granule failure retained first-granule offsets/history and contaminated the next decoded frame. | verified |
| AUD-070 | High | Use cumulative, sample-rate-specific short-band boundaries for requantization indices. | `sfb * 3 * current_width` overlapped or skipped bands as widths changed and always used 44.1 kHz widths. | verified |
| AUD-071 | High | Place all 12 samples from each short IMDCT without folding its second half onto the first. | `% 6` mapped samples 6–11 onto the same six destinations as samples 0–5. | verified |
| AUD-072 | High | Use long IMDCTs for the first two subbands of mixed blocks. | Every subband of a mixed block was sent through the short transform. | verified |
| AUD-073 | High | Apply anti-alias butterflies to the long portion of mixed blocks. | Anti-aliasing was skipped whenever `block_type == 2`, including mixed long subbands. | verified |
| AUD-074 | High | Requantize mixed-block long and short regions with their respective scalefactors and subblock gains. | Mixed blocks followed the all-long requantization path and ignored parsed short-band factors. | verified |
| AUD-075 | High | Saturate non-finite synthesis output before float-to-integer conversion. | Direct conversion of NaN, infinity, or out-of-range values to `int` is undefined behavior. | verified |
| AUD-076 | Medium | Clear every valid batch-decode output before validating decoder and input pointers. | Null/short input failures could leave caller sentinels intact. | verified |
| AUD-077 | Medium | Clear streaming PCM output before validating the stream handle. | A null stream returned failure without clearing a valid output pointer. | verified |
| AUD-078 | Medium | Check seek-to-end and rewind failures while opening MP3 streams. | `fseek` results were ignored before `ftell` and file reads. | verified |
| AUD-079 | Medium | Clear a valid render buffer when the mixer context is null. | `vaud_mixer_render` returned without initializing caller-owned output. | verified |
| AUD-080 | High | Retire a voice whose sound has null sample storage or a nonpositive frame count before dereferencing it. | `mix_voice` trusted internal sound storage and dimensions. | verified |
| AUD-081 | High | Clamp a negative voice frame cursor before sample indexing. | A corrupted signed `position` was converted into an out-of-bounds sample index. | verified |
| AUD-082 | High | Reset a non-finite or negative fractional voice cursor before interpolation. | `frac_pos` reached integer conversion and sample indexing without a validity gate. | verified |
| AUD-083 | High | Sanitize voice occlusion target and smoothing state before DSP arithmetic. | Non-finite state could poison gain, filter history, metering, and PCM conversion. | verified |
| AUD-084 | High | Disable or Nyquist-cap invalid internal low-pass cutoff state. | A non-finite or unbounded cutoff entered coefficient calculation. | verified |
| AUD-085 | Medium | Retire a non-looping voice immediately when a render lands exactly on its final frame. | Exact-boundary voices remained spuriously active until the next callback. | verified |
| AUD-086 | High | Validate a music stream's current ring-buffer index before array access. | `current_buffer` indexed fixed-size arrays without a local range check. | verified |
| AUD-087 | High | Clamp a negative music buffer cursor before sample indexing. | A corrupted `buffer_position` could address samples before the ring slot. | verified |
| AUD-088 | High | Cap each music slot's published frame count to its physical buffer capacity. | `buffer_frames` was trusted as an array bound. | verified |
| AUD-089 | High | Mark a music slot empty when its sample buffer is null instead of dereferencing it. | Ring storage pointers were assumed valid whenever a positive frame count was published. | verified |
| AUD-090 | Medium | Saturate the public music position counter rather than overflowing it. | Long-running playback incremented signed `position` without an overflow guard. | verified |
| AUD-091 | High | Bound mixer iteration over the active-music registry to its fixed capacity. | Corrupt `music_count` values drove multiple loops beyond `active_music`. | verified |
| AUD-092 | High | Bound mixer iteration over the duck-rule registry to its fixed capacity. | Corrupt `duck_rule_count` values drove envelope and lookup loops out of bounds. | verified |
| AUD-093 | High | Sanitize duck amounts, timing constants, and envelope state before realtime arithmetic. | Non-finite rule fields could produce non-finite gains and undefined downstream conversions. | verified |
| AUD-094 | Medium | Sanitize and clamp the selected group duck gain before applying it. | The lookup returned raw rule state to every voice and music gain path. | verified |
| AUD-095 | Medium | Cache a silent render period while paused. | The paused path left a pre-pause fallback period available for a later lock miss. | verified |
| AUD-096 | High | Convert and cache the shared accumulator before releasing the mixer state mutex. | The successful path exposed `accum_buf` to another render before it finished reading it. | verified |
| AUD-097 | High | Select a stealable voice without adding one to a potentially saturated frame clock. | `frame_counter + 1` overflowed at `INT64_MAX`, and equal-maximum timestamps selected no victim. | verified |
| AUD-098 | Medium | Initialize `vaud_play_ex2` pitch in the same critical section that publishes the voice. | The old two-call implementation could render one period at unity pitch and reread a detached sound context. | verified |
| AUD-099 | Medium | Reject non-finite public low-pass cutoffs and cap finite cutoffs at Nyquist. | Positive infinity passed the setter's `cutoff > 0` check and entered mixer DSP state. | verified |
| AUD-100 | Medium | Replace non-finite public duck attack and release times with the minimum supported interval. | Positive infinity passed the setters' positivity checks and stalled envelope progression. | verified |

The campaign target is exactly 100 evidence-backed findings. Every entry has
an implementation and a regression or validation path; final status advances
to `verified` only after the complete prescribed validation succeeds.

## Verification commands

Targeted during implementation:

```sh
ctest --test-dir build -R 'test_vaud_(audit|core)_fixes' --output-on-failure
ctest --test-dir build -R 'test_(wav_stream|ogg_vorbis|mp3_decode|rt_ogg|rt_vorbis_internal)' --output-on-failure
ctest --test-dir build -R 'test_rt_mp3_internal' --output-on-failure
```

Cross-platform-sensitive batches additionally run:

```sh
./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh
```

The campaign closes only after a full prescribed build and test run succeeds.

## Batch results

### Batch 1 — RIFF/WAV boundary and resampler hardening

- Pre-fix regression run: `test_vaud_audit_fixes` failed on stale output
  values, ignored RIFF sizes, accepted duplicate chunks, and permissive raw
  layouts.
- Post-fix CTests: `test_vaud_audit_fixes`, `test_vaud_core_fixes`, and
  `test_wav_stream` passed.
- A standalone AddressSanitizer + UndefinedBehaviorSanitizer build of the WAV
  audit test passed with strict warnings enabled.

### Batch 2 — Ogg page and logical-stream integrity

- Pre-fix regression run: `test_rt_ogg` failed because reserved page flags
  were accepted. Additional tests cover contradictory/repeated BOS pages, BOS
  propagation across continuations, unterminated EOS packets, and bounded work.
- Post-fix CTests: `test_rt_ogg`, `test_ogg_vorbis`, and
  `test_rt_vorbis_internal` passed.
- A standalone AddressSanitizer + UndefinedBehaviorSanitizer build of
  `RTOggTests.cpp` plus `rt_ogg.c` passed with strict warnings enabled.

### Batch 3 — Vorbis parser and packet-decoder hardening

- Pre-fix regression build failed because the bit reader had no truncation
  state. The behavioral cases additionally demonstrated exact-boundary
  Huffman rejection, permissive identification/comment packets, unordered and
  repeated headers, and stale packet outputs.
- Post-fix CTests: `test_rt_vorbis_internal`, `test_ogg_vorbis`, `test_rt_ogg`,
  and `test_mp3_decode` passed.
- A standalone strict-warning AddressSanitizer + UndefinedBehaviorSanitizer
  build of `RTVorbisInternalTests.c` passed.

### Batch 4 — MP3 parser, reservoir, and transform hardening

- Pre-fix internal-test compilation failed on the absent bit-reader error
  state, CRC accounting, bounded helpers, and safe sample conversion. Behavioral
  cases cover malformed side information/ID3 metadata, changing stream format,
  missing reservoir history, part-boundary reads, and late-frame state rollback.
- Post-fix CTests: `test_rt_mp3_internal`, `test_mp3_decode`,
  `test_rt_vorbis_internal`, `test_ogg_vorbis`, and `test_rt_ogg` passed.
- A standalone strict-warning AddressSanitizer + UndefinedBehaviorSanitizer
  build of `RTMp3InternalTests.c` passed.

### Batch 5 — Mixer, music ring, and public-control hardening

- Pre-fix regression execution crashed when a playing voice referenced invalid
  sample storage. Additional cases cover corrupt cursors, DSP state, music ring
  metadata, registry counts, saturated clocks, paused fallback buffers, and
  non-finite public controls.
- Post-fix CTests: `test_vaud_core_fixes`, `test_vaud_audit_fixes`,
  `test_wav_stream`, `test_rt_audio_mixing`, and the codec-focused tests passed.
- A standalone strict-warning AddressSanitizer + UndefinedBehaviorSanitizer
  build of `test_vaud_core_fixes.c` passed.
- The prescribed incremental macOS build passed all 1,968 enabled tests, the
  platform-policy lint, runtime-surface audit, cross-platform/native smoke
  slices, Zanna Studio build, and install stage. The audio-unavailable test was
  skipped as designed because the configured build had audio enabled.
