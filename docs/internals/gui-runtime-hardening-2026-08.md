---
status: complete
audience: contributors
last-verified: 2026-08-10
---

# GUI Runtime Hardening Program (2026-08)

## 1. Summary and objective

This program reviews the C runtime under `src/runtime/graphics/gui` and records
100 non-duplicative correctness, memory-safety, resource-bound, portability,
and performance improvements. The objective is to make every public GUI call
fail closed on hostile boundary values, keep retained state internally
consistent, bound attacker-controlled work, and preserve successful-call
behavior on macOS, Windows, and Linux.

## 2. Scope

In scope are the existing GUI runtime implementation, its private helpers,
platform accessibility adapters, focused runtime and toolkit tests, CTest
metadata, and this traceability record. Runtime registry entries, public
function signatures, IL opcodes, language grammar, verifier rules, external
dependencies, and CI workflows are out of scope. Any newly discovered need to
change one of those surfaces pauses that item for an ADR instead of silently
expanding this program.

## 3. Hardening contract

- Public 64-bit integers and floating-point values are validated before they
  enter narrower toolkit storage or C conversions.
- Float-to-integer conversion is finite and saturating; unsigned revisions and
  counts saturate at the signed runtime boundary.
- Allocation growth is checked, transactional, and subject to an explicit
  logical resource ceiling where input can control work.
- Failed status-returning operations preserve previously published widget
  state.
- Traversals terminate on malformed, cyclic, or excessively large retained
  graphs and expose truncation where the public result supports it.
- Successful calls keep their existing values, ordering, ownership, and event
  semantics.

## 4. Specification

1. **Feature toggle:** none. These are safety properties of existing APIs and
   must not be optional.
2. **Configuration:** none. Bounds are deterministic implementation policy,
   not environment-dependent behavior.
3. **Errors:** existing void/default-return APIs remain no-op or return their
   documented sentinel for invalid input. Existing trapping APIs retain their
   exact diagnostics. No new public diagnostic is introduced without a
   separately reviewed contract.
4. **Positive tests:** given ordinary valid inputs, when each hardened API is
   called, then its existing result and mutation remain unchanged.
5. **Negative tests:** given non-finite values, signed extrema, invalid enum or
   index values, and malformed retained mirrors, when a hardened API is
   called, then it does not execute undefined behavior, allocate without a
   policy bound, or partially publish state.
6. **Edge tests:** given values exactly at each supported boundary, when the
   API is called, then narrowing and saturation are deterministic on all
   supported data models.

## 5. Evidence keys

- `RUNTIME`: `test_rt_gui_runtime` focused runtime contract coverage.
- `GUI`: complete CTest `gui` label.
- `SAN`: AddressSanitizer and UndefinedBehaviorSanitizer focused run.
- `STATIC`: exhaustive warning, performance, and portability analysis.
- `PLATFORM`: repository platform-policy lint and cross-platform smoke.
- `BUILD`: canonical warning-as-error scripted build and test pass.

## 6. Resolution ledger

Every item below is complete. `RUNTIME` covers boundary and malformed-mirror
regressions; `GUI` covers the retained toolkit; `SAN` covers both under ASan
and UBSan. Shared evidence is summarized in section 7.

### 6.1 Numeric boundaries and narrowing

1. Added deterministic NaN/overflow-safe `f64` to `i64` saturation.
2. Added deterministic NaN/overflow-safe `f64` to `i32` saturation.
3. Centralized `u64` to public signed-integer saturation.
4. Centralized `size_t` to public signed-integer saturation.
5. Prevented `DpiToPhysical` multiplication from overflowing before conversion.
6. Made Widget width conversion defined for non-finite retained geometry.
7. Made Widget height conversion defined for non-finite retained geometry.
8. Made Widget X conversion defined for non-finite retained geometry.
9. Made Widget Y conversion defined for non-finite retained geometry.
10. Bounded TreeView hit-test X before narrowing to toolkit float storage.
11. Bounded TreeView hit-test Y before narrowing to toolkit float storage.
12. Saturated TabBar hit-test X before narrowing to toolkit `int`.
13. Saturated TabBar hit-test Y before narrowing to toolkit `int`.
14. Saturated OutputPane cell-width results.
15. Saturated OutputPane cell-height results.
16. Made OutputPane text measurement reject conversion OOM and saturate its result.
17. Rejected non-finite CodeEditor wrap widths and character widths.
18. Saturated CodeEditor characters-per-row calculations.
19. Saturated CodeEditor cursor-pixel X results.
20. Saturated CodeEditor cursor-pixel Y results.
21. Kept full `i64` precision in CodeEditor line-at-pixel arithmetic.
22. Saturated CodeEditor visual-row narrowing.
23. Kept full `i64` precision in CodeEditor column-at-pixel X arithmetic.
24. Saturated wrapped CodeEditor row narrowing.
25. Saturated wrapped CodeEditor column-in-row narrowing.
26. Widened wrapped row-times-column arithmetic before clamping.
27. Saturated unwrapped CodeEditor column narrowing.
28. Saturated EditorBuffer revision results.
29. Routed CodeEditor performance counters through the shared saturating policy.
30. Saturated FindBar replace-all counts.
31. Saturated FindBar match counts.
32. Made CommandPalette query reads tolerate a lower-layer null query.
33. Saturated CommandPalette query generations.
34. Saturated full-paint frame counters.
35. Saturated partial-paint frame counters.
36. Saturated platform-event timestamps in public event views.
37. Saturated Widget IDs.
38. Routed TextInput unsigned counters through the shared signed policy.
39. Saturated public Widget semantic revisions.
40. Saturated Theme revisions.
41. Saturated retained CodeEditor revisions.
42. Saturated OutputPane line counts.
43. Saturated RadioGroup revisions.
44. Centralized safe DataGrid native-size conversion.

### 6.2 Accessibility graph hardening

45. Sanitized accessibility screen X.
46. Sanitized accessibility screen Y.
47. Sanitized accessibility screen width to a finite nonnegative bound.
48. Sanitized accessibility screen height to a finite nonnegative bound.
49. Saturated accessibility node IDs.
50. Saturated accessibility `labelForId` references.
51. Saturated accessibility widget revisions.
52. Saturated accessibility semantic revisions.
53. Saturated accessibility announcement revisions.
54. Bounded generic effective-enabled ancestor walks against parent cycles.
55. Bounded generic snapshot depth.
56. Bounded generic snapshot node count at 100,000.
57. Terminated generic snapshots containing cycles of invisible siblings.
58. Capped generic traversal storage exactly instead of overallocating a power of two.
59. Bounded Linux AT-SPI effective-enabled ancestor walks.
60. Replaced AT-SPI float rounding casts with defined saturation.
61. Bounded Linux AT-SPI snapshot node count.
62. Checked every AT-SPI node-array growth multiplication.
63. Checked every AT-SPI traversal-stack growth multiplication.
64. Bounded AT-SPI live-widget lookup work.
65. Bounded AT-SPI lookup-stack growth.
66. Replaced quadratic AT-SPI parent discovery with recorded parent indexes.
67. Replaced quadratic AT-SPI child population with one linear pass.
68. Removed the post-snapshot dependency on dereferencing live parent pointers.

### 6.3 Image and media publication

69. Added the decoder-aligned 64M-pixel runtime RGBA policy.
70. Applied the same 64M-pixel policy inside the retained Image toolkit.
71. Bounded raw RGBA Image uploads.
72. Bounded Pixels-to-Image conversion buffers.
73. Bounded Image region-update conversion buffers.
74. Bounded menu-icon conversion buffers.
75. Bounded CodeEditor gutter-icon conversion buffers.
76. Bounded VideoWidget frame-conversion scratch storage.
77. Bounded RenderTarget-to-Image readback dimensions.
78. Bounded direct Image producer borrows.
79. Bounded and validated direct Image producer commits.
80. Separated unpublished producer staging from published Image pixels.
81. Prevented abandoned same-size producer writes from changing the visible frame.
82. Prevented failed resized producer writes from clearing the previous frame.
83. Required commits to match the most recent authorized borrow dimensions exactly.
84. Added explicit cancellation for an unpublished producer frame.
85. Cancelled staging after failed RenderTarget readback.
86. Cancelled stale staging after invalid runtime borrow/commit requests.
87. Swapped published and staging buffers to reuse both allocations across frames.
88. Released staging storage when an Image is cleared.
89. Released staging storage when an Image is destroyed.
90. Avoided content-revision changes when clearing only abandoned staging.

### 6.4 Resource bounds, corrupted metadata, and observability

91. Added checked size-based and signed-count geometric-growth helpers.
92. Bounded MessageBox, CommandPalette, Minimap, FileDialog, RadioGroup,
    FindBar, and VideoWidget wrapper registries.
93. Bounded live-app, retired-font, dialog-stack, and app command-palette arrays.
94. Bounded shortcut registration and triggered-shortcut queues and rejected
    inconsistent signed count/capacity metadata.
95. Bounded CodeEditor gutter, fold, cursor, highlight, semantic-token, and
    custom-keyword arrays and rejected inconsistent metadata before indexing.
96. Bounded MessageBox buttons, Minimap markers, file-drop batches, and
    FileDialog selected-path snapshots.
97. Bounded subhandle population and both subhandle hash indexes while retaining
    the allocation-failure list fallback.
98. Bounded primary and overlay render traversal work so child/parent cycles terminate.
99. Bounded GUI-owned runtime strings and all module-local C-string duplication
    to 64 MiB, including embedded-NUL expansion arithmetic.
100. Rejected null selected-row text with nonzero length and bounded aggregate
     ListBox selection output, preventing uninitialized bytes and unbounded growth.

The runtime test was also added to the `gui` CTest label, increasing the label
from 26 to 27 tests so the principal boundary suite cannot be skipped by GUI-only
validation. The Image toolkit test now proves unpublished staging, exact commit
authorization, cancellation, reuse, and preservation of the prior frame.

## 7. Validation record

The pre-change baseline passed all 26 tests then labeled `gui` and the
separately labeled `test_rt_gui_runtime`. Final evidence on macOS arm64:

- `BUILD`: canonical incremental `build_zanna_mac.sh`, warning-as-error enabled.
- `RUNTIME`: focused runtime, manifest, Image toolkit, CodeEditor, VideoWidget,
  availability, and macOS-menu tests: 8/8 passed.
- `GUI`: `ctest --test-dir build -L gui --output-on-failure`: 27/27 passed.
- `SAN`: ASan+UBSan build plus the complete `gui` label: 27/27 passed with
  abort/halt-on-error settings.
- `PLATFORM`: changed-file platform-policy lint passed; the repository
  cross-platform smoke slice passed in full.
- `STATIC`: warning-as-error syntax compilation of `rt_gui_atspi_linux.c`
  passed using the runtime include/define surface. Exhaustive `cppcheck`
  warning, performance, and portability analysis completed all 30 GUI
  translation units and platform configurations without a diagnostic after
  suppressing only the pre-existing C++ cast warning in the included
  `rt_pixels_internal.h` outside this scope.
- `git diff --check` passed after formatting all modified source with the
  repository configuration.
