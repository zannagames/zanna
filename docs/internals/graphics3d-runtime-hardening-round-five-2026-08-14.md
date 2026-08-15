---
status: complete
audience: contributors
last-verified: 2026-08-14
---

# Graphics3D Runtime Hardening Program, Round Five (2026-08-14)

## 1. Scope

This ledger records a fifth non-duplicative review of the C runtime under
`src/runtime/graphics/3d`. It closes exactly 50 hostile asset-ingress,
transactionality, diagnostic-integrity, and hot-path findings in the raw glTF
JSON scanner and shared asset diagnostic layer. Earlier Graphics3D ledgers are
the baseline; none of their closed findings are recounted here.

The work does not change the runtime C ABI, IL opcode set, language grammar,
serialized scene format, platform policy, external dependencies, or CI
workflows. Malformed content continues through the existing recoverable
`RT_ASSET_ERROR_CORRUPT` path. No ADR is required.

## 2. Corrected issue ledger

Evidence keys:

- `JSON`: adversarial scanner, Unicode, range, schema, and regression cases in
  `test_rt_gltf`.
- `DIAG`: diagnostic-code, scope, warning, and report cases in
  `test_rt_asset_load_errors`.
- `REGRESSION`: the existing glTF corpus, including sparse accessors,
  animations, variants, meshopt, and supported extensions.
- `STATIC`: focused source review and compiled-translation-unit static analysis.
- `BUILD`: warning-as-error macOS build through `build_zanna_mac.sh`.
- `G3D`: complete `graphics3d` CTest label.
- `SAN`: AddressSanitizer and UndefinedBehaviorSanitizer validation.
- `PLATFORM`: platform-policy lint and cross-platform smoke.

| ID | Area | Class | Finding and resolution | Evidence |
|---|---|---|---|---|
| G3H5-001 | JSON whitespace | Correctness | Locale `isspace` admitted vertical tab and form feed even though JSON permits only space, tab, CR, and LF. Use an exact byte predicate everywhere. | JSON |
| G3H5-002 | JSON cursor | Memory safety | Whitespace skipping dereferenced a null claimed buffer and preserved cursors beyond `len`. Make it null-safe and clamp out-of-range positions to the bounded end. | JSON |
| G3H5-003 | Raw UTF-8 | Validation | Lone continuation bytes and illegal leading bytes could pass through strings. Validate every non-ASCII lead before advancing. | JSON |
| G3H5-004 | Raw UTF-8 | Validation | Overlong two-, three-, and four-byte encodings were accepted as opaque bytes. Enforce the shortest scalar encoding for every width. | JSON |
| G3H5-005 | Raw UTF-8 | Memory safety | Truncated multibyte sequences could reach the closing-quote scan. Check the remaining bounded length before reading each continuation. | JSON |
| G3H5-006 | Raw UTF-8 | Validation | Bad continuation bytes inside otherwise plausible sequences were not rejected. Validate every required continuation lane. | JSON |
| G3H5-007 | Raw UTF-8 | Validation | UTF-8 encodings of surrogate code points and values above U+10FFFF entered glTF strings. Reject both non-scalar ranges. | JSON |
| G3H5-008 | Unicode escapes | Validation | A high UTF-16 surrogate could stand alone. Require its adjacent escaped low surrogate before accepting the string. | JSON |
| G3H5-009 | Unicode escapes | Validation | Lone low surrogates and mismatched high/low pairs were accepted independently. Reject both malformed forms. | JSON |
| G3H5-010 | String extraction | Correctness | Decoding `\u0000` into a C string silently exposed a truncated value. Keep the JSON syntax valid but reject C-string extraction of U+0000. | JSON |
| G3H5-011 | Key matching | Validation | The allocation-free key fast path compared unvalidated high bytes. Validate and advance by complete UTF-8 sequences before matching. | JSON |
| G3H5-012 | String sizing | Memory safety | Unicode append, allocation, and surrogate lookahead used overflow-prone addition or failed to reserve the terminator. Use subtraction-checked capacity and remaining-length guards. | JSON, STATIC |
| G3H5-013 | glTF root | Schema | Integral validation accepted a top-level array even though a glTF document root must be an object. Require the root object explicitly. | JSON |
| G3H5-014 | Asset metadata | Schema | A wrong-typed root `asset` value survived the lightweight schema walk. Require its value to be an object. | JSON |
| G3H5-015 | Root definitions | Schema | Root definition collections could use a scalar/object in place of their required arrays. Apply object-array mode to all core root collections. | JSON |
| G3H5-016 | Root definitions | Schema | Root definition arrays could contain numbers, strings, or nulls instead of objects. Require every definition element to be an object. | JSON |
| G3H5-017 | Integral fields | Schema | Recognized scalar integer fields rejected fractional numbers but let strings, booleans, and null pass. Require an exact numeric integer token. | JSON |
| G3H5-018 | Boolean fields | Schema | Recognized boolean-like fields let null and strings bypass numeric checks. Require a boolean literal or exact numeric zero/one. | JSON |
| G3H5-019 | Index arrays | Schema | `children`, `joints`, scene `nodes`, and mapping `variants` validated only elements that happened to be numeric. Require every element to be an exact integer. | JSON |
| G3H5-020 | Attribute maps | Schema | Accessor-index attribute values could be strings or null. Require every direct semantic value to be an exact integer. | JSON |
| G3H5-021 | Morph targets | Schema | Scalar entries in a morph `targets` array bypassed attribute-map validation. Require each entry to be an attribute object. | JSON |
| G3H5-022 | Extensions | Schema | Recognized extension payloads could be scalars while the later importer expected objects. Enforce object payloads before recursive interpretation. | JSON |
| G3H5-023 | Sparse/animation fields | Compatibility | Globally tightening `indices` and `target` would reject their valid sparse-accessor and animation object forms. Preserve those two schema-polymorphic objects while rejecting wrong scalar types. | JSON, REGRESSION |
| G3H5-024 | Root nodes | Compatibility | Root `nodes` contains definition objects, while scene-local `nodes` contains integer indices. Track root context so each form receives its correct element rule. | JSON, REGRESSION |
| G3H5-025 | Material variants | Compatibility | Root `KHR_materials_variants.variants` contains definition objects, while primitive mappings contain integer indices. Track extension-root context to validate both correctly. | JSON, REGRESSION |
| G3H5-026 | Meshopt | Compatibility | Core primitive `mode` is integral but `EXT_meshopt_compression.mode` and `filter` are string enums. Give meshopt payloads a dedicated typed context. | JSON, REGRESSION |
| G3H5-027 | Delimiter matching | Validation | The matching helper accepted arbitrary delimiter pairs and could accept crossed object/array nesting. Delegate to the strict value parser and allow only `{}` or `[]`. | JSON |
| G3H5-028 | Object lookup | Memory safety | A null JSON buffer could be scanned by direct-property helpers. Reject null input before any byte access and leave sentinel outputs intact. | JSON |
| G3H5-029 | Object ranges | Memory safety | Invalid ends beyond `len` and non-object claimed ranges were scanned as though trusted. Validate the exact half-open container range first. | JSON |
| G3H5-030 | Object transaction | Correctness | Lookup returned an early matching member before discovering a malformed suffix. Continue through the closing brace and publish only after full validation. | JSON |
| G3H5-031 | Duplicate keys | Correctness | Direct lookup silently selected the first of duplicate queried keys. Treat the ambiguous lookup as failure and retain sentinel outputs. | JSON |
| G3H5-032 | Object strings | Memory safety | String extraction decoded with document `len`, allowing it to scan beyond the caller's claimed object. Bound decoding to the exact located value end. | JSON |
| G3H5-033 | Top-level arrays | Correctness | Root array lookup returned before validating malformed trailing members or bytes. Validate one complete root document before publishing its range, with null/key guards. | JSON |
| G3H5-034 | Array transaction | Correctness | Array item lookup could expose an early element from a later-malformed array. Save the candidate locally and publish it only after the exact closing bracket. | JSON |
| G3H5-035 | Array iteration | Performance | Item lookup validated the whole array, then rescanned it to reach the item. Validate and locate in one pass. | JSON |
| G3H5-036 | Array index | Undefined behavior | A signed traversal index could overflow on hostile element counts. Use a non-wrapping `size_t` cursor while retaining the public nonnegative `int` contract. | JSON, STATIC |
| G3H5-037 | Array strings | Memory safety | Array string extraction used document `len` instead of the exact item range. Bound decoding to the validated item end. | JSON |
| G3H5-038 | Number extraction | Performance | Every numeric array read allocated and freed heap storage. Use a 128-byte stack fast path and allocate only unusually long tokens. | JSON |
| G3H5-039 | Boolean extraction | Correctness | An invalid present boolish token coerced an arbitrary fallback to zero/one. Return the caller's exact fallback on parse failure. | JSON |
| G3H5-040 | Boolean extraction | Performance | Numeric boolish values performed a second full object lookup through the integer getter. Parse the already-located span once. | JSON |
| G3H5-041 | Error codes | Diagnostic integrity | Arbitrary enum-backed values leaked into the public diagnostic code. Normalize every setter path to the canonical range, mapping invalid values to `CORRUPT`. | DIAG |
| G3H5-042 | Error clearing | Diagnostic integrity | Setting code `NONE` retained stale message and truncation state. Route it through the complete error clear operation. | DIAG |
| G3H5-043 | Load depth | Undefined behavior | Signed nested-load depth could overflow. Store it as saturating `uint64_t` state so hostile nesting cannot wrap through zero. | DIAG, STATIC |
| G3H5-044 | Load scopes | Correctness | An unmatched successful scope exit at depth zero erased a real prior error. Ignore unmatched exits and clear only a matched outermost success. | DIAG |
| G3H5-045 | Warning formatting | Diagnostic integrity | `add_warningf` discarded the temporary formatter's truncation result after copying its bounded output. Carry the source truncation bit into the visible slot. | DIAG |
| G3H5-046 | Warning suppression | Accounting | The first overflow hid both the displaced sixteenth warning and the new seventeenth warning but counted only one. Initialize the summary count to two and keep the first 15 stable. | DIAG |
| G3H5-047 | Warning suppression | Undefined behavior | The suppression counter could increment past `INT64_MAX`. Saturate it while continuing to emit the stable summary. | DIAG, STATIC |
| G3H5-048 | Import report | Correctness | The report buffer covered raw warnings but not their sixfold worst-case JSON escaping, producing truncated invalid JSON. Size it for the proven worst case plus fixed fields. | DIAG |
| G3H5-049 | Report assembly | Memory safety | The bounded append primitive assumed nonnull pointers, positive capacity, and an in-range used count. Centralize byte appends with explicit guards and subtraction-safe capacity math. | DIAG, STATIC |
| G3H5-050 | Report Unicode | Validation | Malformed warning UTF-8 was copied into nominal JSON. Preserve valid sequences and escape each malformed byte as `\u00xx`, keeping every report strict JSON. | DIAG |

## 3. Validation record

The expanded tests were first run against the previous runtime. `test_rt_gltf`
reported the new whitespace failures and then faulted in null object lookup;
`test_rt_asset_load_errors` reported three failing cases covering invalid error
codes, lost formatted-warning truncation, and truncated worst-case JSON. This
established that the new cases exercised real pre-change behavior.

Completed focused validation:

```text
./build/src/tests/test_rt_gltf
1267/1267 assertions passed

./build/src/tests/test_rt_asset_load_errors
11/11 cases passed

ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ... ./scripts/build_zanna_mac.sh
incremental warning-as-error build passed

ctest --test-dir build -L graphics3d --output-on-failure -j8
155/155 passed, including display/GPU/native lanes and the 121-second soak

ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  ctest --test-dir build-g3h5-asan -L graphics3d -LE 'slow|native_run' ...
152/152 sanitizer-compatible tests passed under AddressSanitizer

UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-g3h5-ubsan -L graphics3d -LE 'slow|native_run' ...
152/152 sanitizer-compatible tests passed under UndefinedBehaviorSanitizer
final focused refresh after test-handle cleanup: 1267/1267 and 11/11 passed
under both sanitizer builds

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --inconclusive --force -j6
106/106 Graphics3D C translation units scanned; changed units had no findings

./scripts/lint_platform_policy.sh
Platform policy lint: clean

./scripts/run_cross_platform_smoke.sh --build-dir build
host smoke slice passed, including macOS arm64 native and display-backed checks

./scripts/build_zanna_mac.sh
clean warning-as-error build and Zanna Studio native compile passed
1973/1973 ordinary tests passed; one configuration-inapplicable test skipped
runtime surface audit passed: 7869 functions, 532 classes, 9228 declarations
platform lint and cross-platform smoke passed; install passed at an
unprivileged temporary prefix
```

The full cppcheck scan emitted ten diagnostics in unchanged sources. Manual
inspection confirmed eight intentional subtraction-safe allocation guards that
divide `SIZE_MAX` by element size and two documented ownership transfers into
retained particle/water staging slots. Focused scans of both changed C
translation units emitted no diagnostics.

The fuzz smoke lane was also attempted, but this AppleClang installation does
not ship `libclang_rt.fuzzer_osx.a`, so its targets cannot link. The deterministic
adversarial corpus, ASan, and UBSan lanes above provide the executable hostile
input coverage available on this host. The final clean repository closure gate
passed with the results above.

The ledger contains exactly 50 unique rows, `G3H5-001` through `G3H5-050`,
with no gaps or duplicates.
