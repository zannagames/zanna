---
status: active
audience: public
last-verified: 2026-09-01
---

# Appendix B: Zanna BASIC Reference

Zanna BASIC is the BASIC-family frontend supported by the current Zanna toolchain. The maintained language reference is:

- [BASIC Language Reference](../../languages/basic-reference.md)

Use that document, plus `zanna check`, as the source of truth for syntax and supported features. This appendix intentionally avoids repeating a long handwritten reference because the compiler and runtime surface evolve over time.

---

## Checked BASIC Example

```basic
REM A small complete Zanna BASIC program
DIM count AS INTEGER = 3
DIM total AS INTEGER = 0

FOR i = 1 TO count
    total = total + i
NEXT i

PRINT "total = "; total
```

Run or check BASIC programs with:

```bash
zanna check program.bas --diagnostic-format=json
zanna run program.bas --diagnostic-format=json
```

---

## Practical Notes

- Prefer complete `.bas` programs when sharing examples. Many BASIC snippets rely on declarations from surrounding context and should be labeled as pseudocode if copied into prose.
- Use the repository build scripts before running broad test suites.
- For runtime APIs, use `zanna --dump-runtime-api` and the `docs/zannalib/` pages.
- For diagnostics, use `zanna explain <CODE> --json`.
