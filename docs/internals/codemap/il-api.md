---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: IL API

Public API facades (`src/il/api/`) for IL operations.

## Overview

- **Total source files**: 2 (.hpp/.cpp)

## Expected-Based API

| File               | Purpose                                                 |
|--------------------|---------------------------------------------------------|
| `expected_api.cpp` | Expected-returning wrappers implementation              |
| `expected_api.hpp` | Expected-returning wrappers for parse/verify operations |

### `il::api::v2` (`il/api/expected_api.hpp`, namespace `il::api::v2`)

Provides `Expected`-based wrappers around IL parsing and verification. On success the `Expected` is empty (`void`); on failure it carries a diagnostic payload.

**Functions:**

- `parse_text_expected(is, m)` — `il::support::Expected<void>`: parse IL text from input stream `is` into module `m`; returns a diagnostic on parse failure
- `verify_module_expected(m)` — `il::support::Expected<void>`: verify module `m`; returns a diagnostic on verification failure

**Dependencies:**

- `il::core::Module` (from `il/core/Module.hpp` via forward declaration)
- `il::support::Expected` (from `support/diag_expected.hpp`)
