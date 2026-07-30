---
status: active
audience: contributors
last-verified: 2026-07-30
---

# ADR 0231: Fail Closed on Unknown Crypto Policy Identifiers

## Status

Accepted (2026-07-30)

## Context

The process-wide crypto module has exactly two operating modes, `COMPAT` and
`APPROVED`, and a closed catalogue of service identifiers. Its C policy
boundary nevertheless accepted every non-`APPROVED` mode value as
compatibility mode. In compatibility mode, the service predicate also allowed
every integer not present in the service catalogue.

That behavior is fail-open. A corrupt value, an unchecked cast, or a future
enum member reaching an older runtime can silently enable the broad
compatibility policy instead of rejecting an unrecognized request. Because
`rt_crypto_module_set_mode` and `rt_crypto_module_service_allowed` are part of
the runtime C contract, tightening their accepted identifiers requires an ADR.

## Decision

`rt_crypto_module_set_mode` accepts only:

- `RT_CRYPTO_MODULE_MODE_COMPAT`
- `RT_CRYPTO_MODULE_MODE_APPROVED`

Every other value returns `0` before initialization, self-testing, locking, or
mutation. The prior mode, lifecycle state, status, and DRBG state remain
unchanged.

`rt_crypto_module_service_allowed` accepts only the currently declared service
range from `RT_CRYPTO_SERVICE_AES_GCM` through
`RT_CRYPTO_SERVICE_SIPHASH`. Unknown values return `0` in every module mode,
including compatibility mode.

The existing Boolean result contract remains unchanged. No new runtime
function, language binding, feature toggle, or configuration key is added.
Future enum additions must update the validation boundary and explicitly state
their compatibility/approved policy.

## Consequences

- Invalid or version-skewed policy identifiers can no longer broaden crypto
  access.
- Valid `COMPAT` and `APPROVED` callers retain their existing behavior.
- The Zia/BASIC enable, disable, and query wrappers are unaffected because they
  already pass declared enum values.
- C embedders that relied on arbitrary integers being normalized to
  compatibility mode now receive a deterministic rejection and must pass the
  declared compatibility value explicitly.

## Alternatives Considered

- **Continue normalizing every non-approved value to compatibility mode.**
  Rejected because a security-policy boundary should not silently broaden
  access for malformed input.
- **Clamp unknown values to the current mode.** Rejected because success would
  conceal the caller bug and make configuration results ambiguous.
- **Trap on unknown values.** Rejected because the existing C policy boundary
  reports transition and authorization failure through Boolean results.

## Validation

The focused crypto runtime test passes an out-of-range mode and service
identifier, requires both operations to return false, and verifies that the
previous mode remains unchanged. Existing approved-mode self-tests, service
allow/deny checks, DRBG generation, AES-GCM, password hashing, and round-trip
coverage remain the compatibility baseline.
