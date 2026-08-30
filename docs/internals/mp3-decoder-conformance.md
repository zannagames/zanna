# MP3 (Layer III) decoder conformance — 2026-08-29

The runtime's from-scratch MPEG Layer III decoder (`src/runtime/audio/rt_mp3.c`,
`rt_mp3_tables.h`) had never decoded a real music file correctly. Legacy
Baseball's soundtrack surfaced it (every `media/Soundtrack` track stopped 24 ms
in), and the review below fixed the decoder end to end rather than working
around it. This note records what was wrong, how it was verified, and the
method to re-verify after any future change.

## Defects found (all fixed)

| Stage | Defect | Effect |
|---|---|---|
| Huffman | Only codebooks 0-3, 5, 6 existed; 7-13, 15-31 fell back to a "bit-width approximation"; a frame selecting one of them aborted the stream (`-2`) | Every real encode stopped after the silent Info frame |
| Huffman | The three trees that did exist (1, 2/3, 5/6) did not match ISO Table B.7 (bits inverted / mis-assigned; tables 2≠3 and 5≠6 were shared) | Even codebook-only streams decoded garbage |
| count1 | Quad table A was read as fixed 4-bit codes and table B as one bit per value | Wrong ±1 lines and desynchronised sign bits |
| Scalefactors | MPEG-1 `scfsi` ignored (granule 1 re-read bits it should copy); LSF (MPEG-2/2.5) partitions used the MPEG-1 `slen` table | Over-read part2 bits → Huffman data truncated; LSF streams unusable |
| Requantize | Subblock gain used 2^(−4·sbg) instead of 2^(−2·sbg); the last long band (21) and short band (12) were never requantized; short-block values were placed in bitstream order but the IMDCT read subband order (no reorder) | Short blocks (transients) collapsed |
| Stereo | M/S applied after the IMDCT; intensity stereo absent | Joint-stereo encodes (LAME's default) wrong |
| Hybrid | No frequency inversion of odd subbands' odd samples | Aliasing across the whole spectrum |
| Synthesis | Matrixing cosine used (2k+1)(2j+1+32)/2 instead of (16+k)(2j+1); the U vector gathered the wrong V halves; `D[]` blocks 5-6 carried flipped signs | Broadband ~13 dB SNR even when everything above was right |
| Tables | Only MPEG-1 scalefactor band tables; MPEG-2.5 11025/12000 Hz needs the 16 kHz short bands | LSF / 2.5 streams mis-banded |

## What the decoder is now

Full ISO 11172-3 / 13818-3 Layer III: MPEG-1, MPEG-2 (LSF) and MPEG-2.5; mono,
stereo, dual and joint (M/S + intensity) stereo; long, short, start, stop and
mixed blocks; CBR and VBR; the bit reservoir; count1 overrun rollback per the
standard. Every Huffman tree is generated from the ISO code tables and
`mp3_huffman_self_check()` proves each one is a complete prefix code over
exactly the standard's value square (`test_mp3_decode` runs it).

## Verification method

Synthetic one-second test signals (L: 440 + 1320 Hz, R: 880 + 3000 Hz) were
encoded with LAME 4.0 across MPEG-1 48/44.1/32 kHz (stereo, joint, mono, VBR),
MPEG-2 24/22.05/16 kHz and MPEG-2.5 12/11.025/8 kHz, plus a real 48 kHz 200 s
soundtrack track. Each was decoded by two independent decoders (Apple
CoreAudio via `afconvert`, and `lame --decode` / minimp3, which agree with each
other at 86-106 dB) and compared with the runtime's PCM after best-lag
alignment:

| Stream | SNR vs reference |
|---|---|
| MPEG-1 48 kHz stereo 64 kbps | 81.7 dB |
| MPEG-1 48 kHz joint 128 kbps | 81.7 dB |
| MPEG-1 44.1 kHz mono 64 kbps | 78.7 dB |
| MPEG-1 32 kHz joint VBR | 82.2 dB |
| MPEG-2 24 / 22.05 / 16 kHz | 81.7 / 81.6 / 81.5 dB |
| MPEG-2.5 12 / 11.025 / 8 kHz | 81.5 / 81.3 / 78-81 dB |
| Real 48 kHz 200 s track | 75.6 dB |

An rms error of ~0.7 LSB is the 16-bit rounding floor: the output is the
reference decoders' output up to rounding. Four of the LAME streams are
embedded as regression fixtures (`src/tests/common/Mp3Fixtures.hpp`) and
`test_mp3_decode` verifies their tone content, channel separation and that the
streaming API decodes every frame; `test_rt_audio_integration` loads one
through `Music.Load` and checks the reported duration.

To re-verify after a decoder change: decode a LAME-encoded file with
`mp3_stream_decode_frame` to raw PCM, decode the same file with `lame --decode`
(or `afconvert -f WAVE -d LEI16`), align on the best lag in ±4000 samples and
expect > 70 dB SNR.
