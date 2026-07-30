---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0229: Bound Native GZIP Decoding Before Output Allocation

## Status

Accepted (2026-07-29)

## Context

Buffered HTTP responses were capped at 256 MiB while reading their encoded
body, but transparent gzip decoding called `rt_compress_gunzip` first and
checked the decoded length afterward. A small compressed response could
therefore allocate the complete 256 MiB managed output before HTTP enforced
its limit.

The old concatenated-member path also copied decoded data repeatedly: each
member expanded into native storage, copied into managed `Bytes`, copied into a
native aggregate, copied into a final managed `Bytes`, and HTTP copied that
result back into a native response buffer. Besides allocator pressure, several
of those allocations could be live at the same time.

HTTP needs a decoder integration surface that applies resource limits while
DEFLATE output grows. Adding that C surface and making network transport depend
on its ownership/error contract requires an ADR. No language-level runtime
registration changes.

## Decision

The compression runtime adds:

```c
int rt_compress_gunzip_raw(const uint8_t *data,
                           size_t len,
                           size_t max_output,
                           size_t max_expansion_ratio,
                           size_t expansion_slack,
                           uint8_t **out_data,
                           size_t *out_len);
```

The helper validates every RFC 1952 member, including optional headers, CRC32,
ISIZE, and exact DEFLATE member boundaries. It returns one `malloc`-owned
aggregate buffer and no managed objects. All failures return zero with
`*out_data == NULL` and `*out_len == 0`; decoder traps are caught internally
after native temporary storage is released.

`max_output` is an absolute aggregate decoded-byte limit. When
`max_expansion_ratio` is nonzero, the decoder also applies:

```text
encoded bytes * max expansion ratio + expansion slack
```

with saturating arithmetic. The smaller of that value and `max_output` is
enforced during output growth, including across concatenated members. A zero
ratio disables only the ratio constraint.

The first decoded member becomes the aggregate allocation directly. Later
members receive only the aggregate remaining budget, are checksum-validated,
and are then appended. Managed `Gunzip` and `GunzipStr` use the same worker
with the existing 256 MiB absolute ceiling and no ratio limit, preserving
their language-level behavior while reducing intermediate copies.

Buffered HTTP gzip decoding uses a 256 MiB absolute ceiling, a 128:1 expansion
ratio, and 1 MiB of slack. The slack preserves small, legitimately repetitive
responses; beyond it, compressed input must scale with decoded output.
`Http.Download` continues to request identity encoding and stream directly to
its staged file.

## Consequences

- HTTP rejects excessive expansion before allocating or publishing the full
  decoded body.
- The normal single-member HTTP path has no compressed-body managed copy, no
  decoded managed copy, and no decoded native-to-native copy.
- Concatenated GZIP members share one absolute and ratio-derived budget.
- Highly repetitive HTTP responses above the compatibility slack can now be
  rejected even when their decoded size is below 256 MiB. Callers that control
  both endpoints can send identity encoding or reduce amplification.
- The new C helper is an additive ABI surface with explicit `free()` ownership;
  it is not registered as a Zia/BASIC runtime method.

## Alternatives Considered

- **Check `Bytes.Length` after managed gunzip.** Rejected because allocation
  and most decompression work have already occurred.
- **Trust the GZIP ISIZE trailer before decoding.** Rejected because ISIZE is
  modulo 2^32, is not trustworthy until CRC/stream validation succeeds, and
  concatenated streams have one trailer per member.
- **Apply only the 256 MiB absolute limit.** Rejected because a tiny payload
  could still force the full allowed allocation.
- **Disable transparent gzip decoding.** Rejected because it would remove an
  established HTTP feature rather than bound it.

## Validation

Focused compression tests cover exact-limit success, absolute-limit failure,
ratio failure, configurable slack, and aggregate failure across concatenated
members. A loopback HTTP regression serves a highly compressible response and
requires a protocol error before response publication. Existing gzip,
network, and high-level transport tests remain the compatibility baseline.
