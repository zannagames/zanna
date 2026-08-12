---
status: complete
audience: contributors
last-verified: 2026-08-11
---

# GUI Runtime Hardening Round Three (2026-08)

## 1. Summary and objective

This third-round program reviews the C runtime under
`src/runtime/graphics/gui` after the first two 100-item hardening tranches. It
implements 100 additional, non-duplicative public-boundary fixes plus eight
supporting consistency migrations. The central invariant is transactional text
handling: invalid, inaccessible, oversized, or allocation-failed runtime text
must not be mistaken for intentional empty text and must not partially mutate a
GUI object.

Existing successful-call behavior and the public runtime C ABI remain
unchanged.

## 2. Specification

- **Scope:** existing GUI runtime C implementation, its shared private helpers,
  focused runtime tests, and this traceability record. The retained GUI toolkit
  is used by tests but its public surface is not changed.
- **Out of scope:** runtime registry entries, public signatures, IL opcodes,
  grammar, verifier rules, cross-layer dependencies, external dependencies,
  and CI workflows. A discovered need in any of those areas requires a
  separate ADR.
- **Feature toggle:** none. These are unconditional safety properties of
  existing APIs.
- **Configuration:** none. The existing 64 MiB GUI text policy is reused.
- **Ingress errors:** failed display-text conversion makes constructors and
  append operations fail without publishing an object or element. Setters
  preserve the old value. Multi-argument operations convert every required
  value before mutating state.
- **Identifier/path errors:** failed no-NUL conversion preserves state or
  returns the API's existing failure sentinel. Embedded NUL values remain
  rejected for identifiers and paths.
- **Egress errors:** retained byte spans larger than the GUI text policy, or a
  non-empty span with a null data pointer, become the canonical empty runtime
  string.
- **Positive tests:** valid text, explicit empty strings, and null values where
  the API already treats null as empty retain their previous behavior.
- **Negative tests:** a registered runtime string with a forged over-policy
  length is rejected without allocating a 64 MiB fixture. Constructors return
  null, append operations add nothing, and setters preserve prior text.

## 3. Resolution ledger: 100 public-boundary fixes

### 3.1 Application, bars, and controls

1. Reject failed App window-title conversion instead of substituting a default title.
2. Preserve a StatusBarItem icon when its semantic icon name cannot be converted.
3. Make named Toolbar button creation fail when its icon name cannot be converted.
4. Make named Toolbar button-with-text creation fail when its icon name cannot be converted.
5. Make named Toolbar toggle creation fail when its icon name cannot be converted.
6. Preserve a ToolbarItem icon when its path cannot be converted.
7. Preserve a ToolbarItem icon when its semantic name cannot be converted.
8. Make Dropdown item insertion fail instead of inserting an empty label after conversion failure.
9. Preserve a Dropdown placeholder when replacement conversion fails.
10. Make ListBox item insertion fail instead of passing a null label downstream.
11. Bound ListBox item-text byte-span export.
12. Preserve a ListBoxItem icon when its semantic name cannot be converted.

### 3.2 Palettes, tooltips, and notifications

13. Preserve the CommandPalette placeholder when replacement conversion fails.
14. Preserve the CommandPalette query when replacement conversion fails.
15. Reject Tooltip.Show text before allocating or mutating the shared tooltip.
16. Convert both Tooltip.ShowRich fields before allocating or mutating the shared tooltip.
17. Preserve a Widget tooltip when replacement conversion fails.
18. Convert both Widget rich-tooltip fields before replacing tooltip text.
19. Suppress an Info toast when message conversion fails.
20. Suppress a Success toast when message conversion fails.
21. Suppress a Warning toast when message conversion fails.
22. Suppress an Error toast when message conversion fails.
23. Reject Toast construction before allocating its wrapper when message conversion fails.

### 3.3 File-dialog boundaries

24. Bound direct escaped path-list length use in FileDialog path counting.
25. Bound direct escaped path-list length use in FileDialog indexed lookup.
26. Require successful synchronous Open-dialog title conversion.
27. Require successful synchronous Open-dialog filter conversion when supplied.
28. Require successful synchronous Open-dialog default-path conversion when supplied.
29. Require successful OpenMultiple-dialog title conversion.
30. Require successful OpenMultiple-dialog default-path conversion when supplied.
31. Require successful OpenMultiple-dialog filter conversion when supplied.
32. Require successful Save-dialog title conversion.
33. Require successful Save-dialog filter conversion when supplied.
34. Require successful Save-dialog default-name conversion.
35. Require successful Save-dialog default-path conversion when supplied.
36. Require successful SelectFolder-dialog title conversion.
37. Require successful SelectFolder-dialog default-path conversion when supplied.
38. Preserve an asynchronous FileDialog title when replacement conversion fails.
39. Preserve an asynchronous FileDialog default filename when replacement conversion fails.

### 3.4 Containers and menus

40. Make GroupBox construction fail when title conversion fails.
41. Preserve a GroupBox title when replacement conversion fails.
42. Make Menubar menu insertion fail when title conversion fails.
43. Make Menu item insertion fail when label conversion fails.
44. Require successful Menu shortcut-item label conversion before insertion.
45. Require successful Menu shortcut conversion before shortcut-item insertion.
46. Make Menu submenu insertion fail when title conversion fails.
47. Make ContextMenu item insertion fail when label conversion fails.
48. Require successful ContextMenu shortcut-item label conversion before insertion.
49. Require successful ContextMenu shortcut conversion before shortcut-item insertion.
50. Make ContextMenu submenu insertion fail when title conversion fails.

### 3.5 Message boxes

51. Require successful Info-dialog title conversion before construction.
52. Require successful Info-dialog message conversion before construction.
53. Require successful Warning-dialog title conversion before construction.
54. Require successful Warning-dialog message conversion before construction.
55. Require successful Error-dialog title conversion before construction.
56. Require successful Error-dialog message conversion before construction.
57. Require successful Question-dialog title conversion before construction.
58. Require successful Question-dialog message conversion before construction.
59. Require successful Confirm-dialog title conversion before construction.
60. Require successful Confirm-dialog message conversion before construction.
61. Require successful Prompt-dialog title conversion before construction.
62. Require successful Prompt-dialog message conversion before construction.
63. Reject stateful MessageBox construction when title conversion fails.
64. Destroy a partially created stateful MessageBox when message conversion fails.
65. Reject an unconvertible custom button label instead of silently substituting `OK`.

### 3.6 Navigation and shortcut state

66. Convert a Breadcrumb path before clearing its existing items.
67. Convert a Breadcrumb separator before clearing its existing items.
68. Convert Breadcrumb item-list text before clearing its existing items.
69. Reject Breadcrumb item insertion when its visible label cannot be converted.
70. Allocate optional Breadcrumb item data before insertion and cleanly unwind on failure.
71. Require Shortcut description conversion before registering or updating a shortcut.
72. Bound direct runtime-string length use in Shortcut triggered-ID matching.

### 3.7 Basic widgets

73. Make Label construction fail when text conversion fails.
74. Preserve Label text when replacement conversion fails.
75. Preserve a Label icon when its semantic name cannot be converted.
76. Bound Label selected-text byte-span export.
77. Make Button construction fail when text conversion fails.
78. Preserve Button text when replacement conversion fails.
79. Preserve Button icon text when replacement conversion fails.
80. Preserve a Button icon when its semantic name cannot be converted.
81. Preserve TextInput text when replacement conversion fails.
82. Bound full TextInput byte-span export.
83. Preserve a TextInput placeholder when replacement conversion fails.
84. Make Checkbox construction fail when text conversion fails.
85. Preserve Checkbox text when replacement conversion fails.
86. Make TreeView node insertion fail when text conversion fails.
87. Reject TreeView inline-edit startup when initial-text conversion fails.
88. Bound TreeNode text byte-span export.
89. Bound TreeNode stable-ID byte-span export.

### 3.8 Complex widgets

90. Make Tab insertion fail when title conversion fails.
91. Preserve a Tab title when replacement conversion fails.
92. Bound Tab stable-ID byte-span export.
93. Preserve a Tab tooltip when replacement conversion fails.
94. Preserve a Tab icon when its semantic name cannot be converted.
95. Bound direct runtime-string length use in CodeEditor.SetText.
96. Suppress OutputPane append when text conversion fails.
97. Suppress OutputPane append-line when text conversion fails.
98. Suppress styled OutputPane append when text conversion fails.
99. Bound OutputPane input byte-span export.
100. Make RadioButton construction fail when text conversion fails.

## 4. Additional consistency migrations

- Add one shared helper for policy-bounded, known-length GUI byte-span export.
- Preserve RadioButton text when replacement conversion fails.
- Suppress PopupList item insertion when text conversion fails.
- Preserve a PopupList filter when replacement conversion fails.
- Bound CodeEditor identifier-word byte-span export.
- Bound CodeEditor line byte-span export.
- Make EditorBuffer construction fail when initial-text conversion fails.
- Refactor named Toolbar icon conversion to report failure separately from the
  intentional no-icon result, enabling its public operations to be transactional.

## 5. Validation plan

- Focused `test_rt_gui_runtime`, VideoWidget contract, and GUI runtime-manifest tests.
- Complete CTest `gui` label.
- AddressSanitizer and UndefinedBehaviorSanitizer GUI-label runs.
- Changed-file platform-policy lint and cross-platform smoke.
- Canonical warning-as-error macOS build and full test run.
- `git diff --check`, repository formatting, and focused static analysis.

## 6. Validation results

- Focused GUI runtime-manifest, VideoWidget contract, and runtime regression
  tests: 3/3 passed.
- Complete normal-build CTest `gui` label: 27/27 passed.
- AddressSanitizer CTest `gui` label: 27/27 passed with
  `detect_leaks=0`; Apple Clang's sanitizer reports that leak detection is not
  supported on this host.
- UndefinedBehaviorSanitizer CTest `gui` label: 27/27 passed with
  halt-on-error enabled.
- Platform-policy lint: clean.
- Cross-platform host smoke suite: passed, including terminal/BASIC, Zia paint,
  chess and Crackman, disabled-feature surface links, linker probes, native
  AArch64 chess, Crackman and Studio-completion probes, and displayed Studio
  smoke.
- Exhaustive warning, performance, and portability `cppcheck` analysis of all
  13 changed GUI runtime translation units: clean.
- Canonical clean warning-as-error macOS build: completed, including the Zanna
  Studio native payload, 1,968/1,968 non-slow tests, runtime-surface audit,
  follow-up smoke probes, and installation.
- `git diff --check` and repository formatting: clean.
- Existing test targets and labels already cover the changed runtime surfaces,
  so no CTest metadata change was necessary.
