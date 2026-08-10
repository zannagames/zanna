---
status: complete
audience: contributors
last-verified: 2026-08-10
---

# GUI Runtime Hardening Round Two (2026-08)

## 1. Summary and objective

This second-round program reviews the C runtime under
`src/runtime/graphics/gui` after the first 100-item hardening tranche. It
implements exactly 100 additional, non-duplicative boundary fixes: bounded
C-string export, finite numeric export, and validation of retained enum/state
domains. Existing successful-call behavior and the public runtime C ABI remain
unchanged.

## 2. Specification

- **Scope:** existing GUI runtime C implementation, its shared private helpers,
   focused runtime tests, CTest metadata when needed, and this traceability
   record. The retained GUI toolkit is used by tests but its public surface is
   not changed.
- **Out of scope:** runtime registry entries, public signatures, IL opcodes,
   grammar, verifier rules, cross-layer dependencies, external dependencies,
   and CI workflows. A discovered need in any of those areas requires a
   separate ADR.
- **Feature toggle:** none. These are unconditional safety properties of
   existing APIs.
- **Configuration:** none. Limits and fallback values are deterministic.
- **Text errors:** a null or non-terminated/oversized lower-layer C string
   becomes the canonical empty runtime string. No export scans beyond the
   existing 64 MiB GUI text policy.
- **Numeric errors:** NaN and infinity never cross a public GUI getter.
   Invalid signed geometry/value state becomes zero, invalid non-negative state
   becomes zero, invalid normalized state uses its documented default, and
   invalid positive scale/font state uses the existing identity/default value.
- **Enum errors:** out-of-domain retained values become each API's existing
   invalid/default sentinel. No private enum value is exposed accidentally.
- **Positive tests:** ordinary valid text, numeric values, and enum ordinals
   retain their existing values.
- **Negative tests:** null/oversized text, non-finite retained floats, negative
   dimensions, and corrupted enum fields fail closed without allocation based
   on unbounded scans or propagation of invalid values.
- **Edge tests:** values at every valid enum endpoint and normalized numeric
    endpoint remain accepted.

## 3. Resolution ledger

### 3.1 Bounded text egress

1. Bound accessibility snapshot C-string conversion.
2. Bound left-zone StatusBar text export.
3. Bound center-zone StatusBar text export.
4. Bound right-zone StatusBar text export.
5. Bound StatusBarItem text export.
6. Bound EditorBuffer full-text export.
7. Bound Dropdown selected-text export.
8. Bound CommandPalette selected-command export.
9. Bound CommandPalette query export.
10. Bound widget drop-type export.
11. Bound widget drop-data export.
12. Bound dropped-file export.
13. Bound synchronous open-dialog result export.
14. Bound synchronous save-dialog result export.
15. Bound synchronous folder-dialog result export.
16. Bound FileDialog error export.
17. Bound every FileDialog path-sequence element export.
18. Bound FileDialog primary and indexed-path export.
19. Bound FindBar live find-text export.
20. Bound FindBar retained find-text fallback export.
21. Bound FindBar live replacement-text export.
22. Bound FindBar retained replacement-text fallback export.
23. Bound Menu title export.
24. Bound MenuItem text export.
25. Bound MenuItem shortcut export.
26. Bound prompt-dialog text result export.
27. Bound MessageBox error export.
28. Bound clipboard text export.
29. Bound queued shortcut-ID export.
30. Bound legacy shortcut-ID export.
31. Bound App title export.
32. Bound Widget name export.
33. Bound TextInput selected-text export.
34. Bound TextInput composition-text export.
35. Bound TreeView edit-text export.
36. Bound TreeNode icon export.
37. Bound Tab title export.
38. Bound CodeEditor delta-JSON export.
39. Bound current Theme name export.
40. Bound OutputPane selection export.
41. Bound RadioButton text export.
42. Bound DataGrid cell export.
43. Bound PopupList selected-text export.
44. Bound VideoWidget error export.
45. Eliminate the leaked transient runtime string used for the VideoWidget Play label.
46. Eliminate the leaked transient runtime string used for the VideoWidget Pause label.
47. Eliminate the leaked transient runtime string used for the VideoWidget Stop label.

### 3.2 Finite numeric egress

48. Sanitize Font logical-size export.
49. Sanitize Widget minimum-width export.
50. Sanitize Widget minimum-height export.
51. Sanitize Widget flex export.
52. Sanitize Widget logical-X export.
53. Sanitize Widget logical-Y export.
54. Sanitize Widget logical-width export.
55. Sanitize Widget logical-height export.
56. Sanitize Widget screen-X export.
57. Sanitize Widget screen-Y export.
58. Sanitize Widget screen-width export.
59. Sanitize Widget screen-height export.
60. Sanitize ScrollView horizontal-offset export.
61. Sanitize ScrollView vertical-offset export.
62. Sanitize Slider value export.
63. Sanitize ProgressBar value export.
64. Sanitize StatusBarItem progress export.
65. Sanitize ToolbarItem screen-X export.
66. Sanitize ToolbarItem screen-Y export.
67. Sanitize ToolbarItem screen-width export.
68. Sanitize ToolbarItem screen-height export.
69. Sanitize MenuItem screen-X export.
70. Sanitize MenuItem screen-Y export.
71. Sanitize MenuItem screen-width export.
72. Sanitize MenuItem screen-height export.
73. Sanitize drop-event X export.
74. Sanitize drop-event Y export.
75. Sanitize VideoWidget position export.
76. Sanitize VideoWidget duration export.
77. Sanitize App backing-scale export.
78. Sanitize global wheel-speed export.
79. Sanitize App UI-scale export.
80. Sanitize App effective-scale export.
81. Sanitize App logical font-size export.
82. Sanitize SplitPane divider-position export.
83. Sanitize SplitPane first-minimum export.
84. Sanitize SplitPane second-minimum export.
85. Sanitize CodeEditor font-size export.
86. Sanitize Spinner value export.
87. Sanitize ThemePalette metric export.

### 3.3 Retained enum and state domains

88. Validate accessible-role export.
89. Validate live-region-mode export.
90. Validate VBox alignment export.
91. Validate VBox justification export.
92. Validate HBox alignment export.
93. Validate HBox justification export.
94. Validate Label alignment export.
95. Validate TreeView drop-position export.
96. Validate SplitPane orientation export.
97. Validate SplitPane collapsed-side export.
98. Validate Image filter export.
99. Validate DataGrid sort-direction export.
100. Validate Theme mode export.

## 4. Validation plan

- Focused `test_rt_gui_runtime` regression coverage for shared helpers and
  corrupted retained state.
- Complete CTest `gui` label.
- AddressSanitizer and UndefinedBehaviorSanitizer GUI-label run.
- Changed-file platform-policy lint and cross-platform smoke.
- Canonical warning-as-error macOS build and full test run.
- `git diff --check` and repository formatting for every modified source.

## 5. Validation results

- Focused GUI and VideoWidget runtime tests: 2/2 passed.
- Complete normal-build CTest `gui` label: 27/27 passed.
- AddressSanitizer CTest `gui` label: 27/27 passed.
- UndefinedBehaviorSanitizer CTest `gui` label: 27/27 passed.
- Strict changed-file platform-policy lint: clean.
- Cross-platform host smoke suite: passed.
- Canonical warning-as-error macOS build: completed, including the Zanna
  Studio native payload, 1,967/1,967 non-slow tests, follow-up smoke probes,
  audits, and installation.
- Existing test targets and labels already cover the changed runtime surfaces,
  so no CTest metadata change was necessary.
