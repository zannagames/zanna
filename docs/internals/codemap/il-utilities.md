---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: IL Utilities

Shared IL helper functions (`src/il/utils/`).

## Overview

- **Total source files**: 5 (.hpp/.cpp)

## Utilities

| File             | Purpose                                                 |
|------------------|---------------------------------------------------------|
| `UseDefInfo.cpp` | Temp-use counting and safe SSA value replacement implementation |
| `UseDefInfo.hpp` | Temp-use counting plus mutation-safe replacement helper |
| `Utils.cpp`      | IR queries implementation                                                                          |
| `Utils.hpp`      | IR queries: block membership, terminator classification, value replacement, next temp ID, block find |
