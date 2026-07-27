---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0042: HTTP TLS Verification Bypass API

## Status

Accepted

## Context

`Zanna.Network.HttpReq.SetTlsVerify(false)` disables HTTPS certificate and
hostname verification. The behavior is needed for local self-signed fixtures and
some compatibility tests, but the boolean setter is easy to copy into production
code because the dangerous value is visually small.

Removing the setter would break existing programs. The runtime needs to preserve
the feature while making insecure use explicit in source, docs, and API dumps.

## Decision

Add `Zanna.Network.HttpReq.AllowInsecureCertificatesForTesting()` as the
canonical public escape hatch for disabling HTTPS verification on a request.

The helper is equivalent to `SetTlsVerify(false)`, returns the same request for
fluent chaining, and is classified as `unsafe` in runtime API metadata.
`SetTlsVerify` remains available for compatibility and is also classified as
`unsafe` because API metadata cannot currently express value-dependent safety.

Production examples must keep certificate verification enabled. Documentation
may show the bypass only in explicitly local-test or unsafe sections.

## Consequences

- Existing code keeps compiling and running.
- New code has an intentionally loud method name when certificate verification
  is disabled.
- Generated API docs and audits can flag both the compatibility setter and the
  explicit bypass as unsafe.
- Future options-object work can keep the same naming policy with a field such
  as `insecure_skip_certificate_verification`.
