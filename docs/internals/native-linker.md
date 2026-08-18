---
status: active
audience: contributors
last-verified: 2026-08-17
---

# Native Linker — Object File Linking & Executable Generation

The native linker provides a built-in object/archive link pipeline that reads object files, resolves symbols, merges
sections, applies relocations, and writes platform-native executables. Combined with the
[native assembler](native-assembler.md), this removes the system linker dependency from the executable link step.

> **Pipeline comparison:**
>
> - **Deprecated alias:** `.o` + runtime `.a` archives → NativeLinker (`--system-link` now maps here)
> - **Native path:** `.o` + runtime `.a` archives → NativeLinker → executable

---

## Table of Contents

1. [Overview](#overview)
2. [Linker Pipeline](#linker-pipeline)
3. [Archive Reader](#archive-reader)
4. [Object File Readers](#object-file-readers)
5. [Symbol Resolution](#symbol-resolution)
6. [Section Merging](#section-merging)
7. [Relocation Application](#relocation-application)
8. [Executable Writers](#executable-writers)
9. [Platform-Specific Details](#platform-specific-details)
10. [CLI Flags](#cli-flags)
11. [Source File Map](#source-file-map)
12. [Design Decisions](#design-decisions)

---

## Overview

### What the Linker Does

The native linker takes the user's compiled `.o` file plus the Zanna runtime archives (`.a` files) and produces a
runnable executable. It performs the classic linker pipeline:

1. Parse the user's `.o` file
2. Parse runtime `.a` archives
3. Iteratively extract archive members needed to resolve undefined symbols
4. Merge sections from all objects into output segments
5. Assign virtual addresses with platform-appropriate page alignment
6. Patch machine code with resolved symbol addresses (relocation application)
7. Write the final executable in the target format (ELF / Mach-O / PE)

### Supported Output Formats

| Format | Platform | Architecture | Page Size | Dynamic Imports |
|--------|----------|-------------|-----------|-----------------|
| ELF | Linux | x86_64 | 4KB | Supported |
| ELF | Linux | AArch64 | 4KB | Supported |
| Mach-O | macOS | x86_64 | 4KB | Not supported — macOS x86-64 is not a Zanna target |
| Mach-O | macOS | AArch64 (Apple Silicon) | **16KB** | Supported |
| PE | Windows | x86_64, AArch64 | 4KB | Supported |

Archive-only / fully self-contained links can use the native linker on every output-writer target above.
Programs that depend on libc, the Windows CRT, or OS frameworks require one of the native dynamic-import
targets above; unsupported shared-library targets now fail explicitly instead of delegating to the host linker.

### Validation Guardrails

Native-link regressions are caught in three layers:

1. `test_linker_platform_import_planners` validates the curated symbol-to-dylib/DLL mapping logic.
2. `test_linker_runtime_import_audit` scans every member of the built runtime and support archives on the host and fails on any unresolved import that is not explicitly classified by the native-link policy.
3. `scripts/run_cross_platform_smoke.sh` runs the archive-wide audit plus host-native smokes such as `native_smoke_chess_ai_arm64`, `native_smoke_crackman_movement_arm64`, and `native_smoke_zannastudio_completion_arm64` when the current host supports them.

On macOS, the planner keeps framework rules ordered from most-specific to broad prefixes. For example,
`CGImageSource*` imports bind to `ImageIO.framework` before the generic CoreGraphics `CG*` rule.

> **Critical:** macOS arm64 requires 16KB page alignment. The dynamic linker (`dyld`) rejects executables with
> incorrect page alignment. This is the single most important platform-specific detail in the linker.

---

## Linker Pipeline

The `NativeLinker` orchestrator (`NativeLinker.cpp`) ties the pipeline together:

```text
┌─────────────┐    ┌──────────────┐    ┌──────────────────┐
│ ObjFileReader│    │ArchiveReader │    │  SymbolResolver  │
│ (ELF/MachO/ │───▶│ (GNU/BSD/    │───▶│  (iterative      │
│  COFF)      │    │  COFF .lib)  │    │   demand-pull)   │
└─────────────┘    └──────────────┘    └────────┬─────────┘
                                                │
                                       ┌────────▼─────────┐
                                       │  SectionMerger   │
                                       │  (classify, merge│
                                       │   assign VAs)    │
                                       └────────┬─────────┘
                                                │
                                       ┌────────▼─────────┐
                                       │  RelocApplier    │
                                       │  (patch machine  │
                                       │   code bytes)    │
                                       └────────┬─────────┘
                                                │
                              ┌─────────────────┼──────────────────┐
                              ▼                 ▼                  ▼
                       ┌─────────────┐   ┌──────────────┐   ┌──────────┐
                       │ElfExeWriter │   │MachOExeWriter│   │PeExeWriter│
                       └─────────────┘   └──────────────┘   └──────────┘
```

---

## Archive Reader

**File:** `codegen/common/linker/ArchiveReader.hpp/.cpp`

Parses Unix `ar` archives (`.a` files) containing object files. The runtime libraries are compiled by CMake into `.a`
archives, one per component (base, oop, collections, text, io_fs, exec, threads, graphics, audio, network).

### Archive Format Variants

The reader handles all three major variants:

| Variant | Platform | Symbol Table | Long Names |
|---------|----------|-------------|------------|
| **GNU** | Linux | `/` member (big-endian count + offsets + NUL names) | `//` string table, `/offset` references |
| **BSD** | macOS | `__.SYMDEF SORTED` (ranlib array + string pool) | `#1/N` (N name bytes inline after header) |
| **COFF** | Windows | `/` member (same as GNU first linker member) | `//` string table (same as GNU) |

All variants share the `!<arch>\n` magic (8 bytes) and 60-byte member headers.

### Selective Extraction

Archives contain a symbol index mapping symbol names to member offsets. The linker uses this to extract only the
members that define needed symbols, rather than linking every member. This is critical for keeping executable sizes
reasonable — the full runtime is ~3,200 symbols across 11 component archives.

The archive parser validates member ranges, decimal size fields, symbol-table sizes, string-pool bounds, and
long-name offsets before using them. GNU long-name entries trim both NUL and newline terminators and remove the
trailing `/` marker, so member names are canonical before symbol-index matching. Duplicate archive-index entries
keep the first definition for compatibility and also retain the full candidate list so symbol resolution can retry
later members when an earlier indexed member has already been extracted or does not satisfy the unresolved name.
Malformed member trailer bytes, missing odd-member padding, trailing partial headers, and symbol indexes that point
at no real member are rejected instead of being ignored.

---

## Object File Readers

**Files:** `codegen/common/linker/ObjFileReader.hpp`, `ElfReader.cpp`, `MachOReader.cpp`, `CoffReader.cpp`

### Auto-Detection

The reader auto-detects format from magic bytes:

| Bytes | Format |
|-------|--------|
| `7F 45 4C 46` | ELF |
| `CF FA ED FE` | Mach-O 64-bit (little-endian) |
| `64 86` or `64 AA` | COFF (AMD64 or ARM64 machine field) |
| `00 00 FF FF` | COFF BigObj |

### Unified Representation

All three readers produce the same `ObjFile` structure:

```text
ObjFile
├── sections[]     → ObjSection (name, data bytes, relocations, alignment, flags)
├── symbols[]      → ObjSymbol  (name, binding, section index, offset)
├── format         → ELF / MachO / COFF
└── machine        → x86_64 / AArch64
```

### Format-Specific Handling

- **ELF**: Explicit addends from `.rela` sections and implicit addends from `.rel` sections are supported. Malformed
  `.rel` addend sites are rejected when the implicit addend would read past the target section. Extended section
  counts are decoded from section header 0. `SHT_GROUP` COMDAT groups carry their signature and member list into the
  linker object model, relocation table byte sizes must be exact multiples of their entry size, `SHN_COMMON` symbols
  are preserved as tentative definitions for linker-wide coalescing, weak undefined symbols stay
  undefined/weak-external, and absolute symbols keep their absolute values. Each relocation section is resolved
  through its own `sh_link` symbol table, and symbol tables must link to an in-bounds `SHT_STRTAB`. Symbols and
  relocation sections that reference unsupported or unmapped section indexes are hard errors.
- **Mach-O**: Addends are extracted from instruction bytes or from ARM64 `ADDEND` relocations. x86_64 branch/signed
  PC-relative addends are normalized to Zanna's internal `S + A - P` convention, where a raw displacement field of
  zero becomes internal addend `-4`. Leading `_` is
  stripped from external symbol names (Mach-O convention). Non-extern section-relative relocations resolve through
  synthetic local section symbols, ARM64 `ADDEND` payloads are sign-extended, consecutive addend records are
  rejected, ARM64 unsigned data-pointer relocations preserve their embedded 8-byte addends, and `__DWARF`/debug
  sections are decoded as non-alloc sections. `MH_SUBSECTIONS_VIA_SYMBOLS`
  `__TEXT,__text` inputs are split at local, weak, and global symbol boundaries for the native linker, preserving
  function-level dead-strip and ICF behavior even when the object writer emitted one Mach-O text section.
  Executable and writable flags are inferred from both section attributes and data-like segment names. Read-only
  Mach-O data segments such as `__DATA_CONST` and `__AUTH_CONST` remain data-segment sections without being treated
  as ordinary writable data.
- **COFF**: Addends are extracted per relocation kind. AMD64 `REL32[_n]` addends keep the raw 32-bit displacement
  field as the internal addend; the relocation applier owns the COFF end-of-field bias when patching. AMD64/ARM64
  `ADDR64` uses an 8-byte addend; ARM64 branch, ADRP page, and page-offset relocations decode their instruction
  fields so writer-emitted placeholders do not become bogus addends. Common symbols are preserved as tentative
  definitions with COFF-size-derived alignment.
  Weak extern fallback records, BigObj symbol tables, associative COMDAT section relationships, and COFF relocation
  overflow records are parsed. BigObj headers must carry the COFF BigObj class ID, relocations may not target
  auxiliary symbol records, weak externs must have valid auxiliary fallback records, and defined symbols must point
  inside their section. COMDAT `ANY`, `SAME_SIZE`, `EXACT_MATCH`, `LARGEST`, and `NODUPLICATES` policies are
  represented explicitly; unsupported selection values are rejected.

### Reader Validation

Readers reject unsupported file types, wrong machines, unsupported section alignment, oversized materialized
sections, out-of-bounds section data, unterminated or out-of-range ELF/Mach-O string-table names, invalid COFF
long-name string-table references, truncated section headers, truncated relocation tables, undersized Mach-O
load commands, dangling Mach-O ARM64 addend records, scattered Mach-O relocations, invalid relocation symbol
indexes, defined symbols whose offset/size extends past their section, and Mach-O relocation addend extraction sites
that do not fit inside the target section. Zero-fill sections keep their logical memory size without materializing
zero bytes into the object model. Debug and unwind metadata such as
`__compact_unwind`, `__eh_frame`, `.debug_line`, and `__DWARF` sections is preserved in the intermediate object
model for later passes. Executable links strip non-alloc debug sections by default and keep them only when
`NativeLinkerOptions::preserveDebugSections` is set.

Reader range checks avoid offset+size overflow before slicing input buffers, symbol tables, relocation tables, and
section contents. ELF section headers and relocation records are copied through aligned storage before decoding, so
malformed or unaligned byte buffers do not rely on host pointer alignment. Mach-O objects that contain relocation
records must also provide `LC_SYMTAB`; without it relocation symbol indexes cannot be mapped safely.

### Input Compatibility

Before generating synthetic dynamic-import stubs, `NativeLinker` checks that every real input object matches the
requested output target:

| Target | Required Object Format | Required Machine |
|--------|------------------------|------------------|
| Linux | ELF | selected architecture |
| macOS | Mach-O | selected architecture |
| Windows | COFF | selected architecture |

Missing extra objects, unreadable archives, format mismatches, and machine mismatches are hard link errors.
The compatibility check uses an explicit `ObjFile::synthetic` marker for linker-created helpers; user or archive
members whose names happen to start with `<` are still validated as real inputs.

---

## Symbol Resolution

**Files:** `codegen/common/linker/SymbolResolver.hpp/.cpp`

### Algorithm

Symbol resolution uses an iterative fixed-point algorithm:

1. **Seed**: Add the user's `.o` file. All its globals → defined, all its extern refs → undefined.
2. **Scan archives**: For each undefined symbol, including COFF weak-external fallback names that request library
   search or `IMAGE_WEAK_EXTERN_SEARCH_ALIAS`, check each archive's symbol index. If found, extract that member,
   parse it, add its definitions and new undefined refs. `IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY` fallbacks are kept as
   weak aliases but do not force archive extraction.
   Duplicate index entries are tried in archive order across fixed-point iterations.
3. **Repeat** until no new definitions are found (handles cross-archive dependencies).
4. **Classify remaining**: Unresolved symbols are marked as dynamic (expected from shared libraries).

### Precedence Rules

| Existing | New | Result |
|----------|-----|--------|
| Undefined | Global | Global wins |
| Undefined | Weak | Weak wins |
| Weak | Global | Global wins (strong overrides weak) |
| Global | Global | **Error** (multiply defined) |
| Global | Weak | Existing Global kept |
| Common | Common | Largest size/max alignment coalesced |
| Common | Global | Strong definition wins |
| Weak | Common(Global) | Common strong tentative definition wins |

Duplicate strong definitions in matching ELF/COFF COMDAT groups are resolved by their selection policy: `ANY` keeps
the first definition, `SAME_SIZE` requires equal section sizes, `EXACT_MATCH` requires equal section bytes,
attributes, memory size, alignment, associative-section identity, and relocation signatures, `LARGEST` selects the
largest contribution, and `NODUPLICATES` remains an error. Non-COMDAT strong definitions still use normal
multiply-defined diagnostics.

### Runtime Archive Order

If an archive index selects a member, that member must contain object bytes and parse successfully; a corrupt or
empty selected member is a hard error rather than being marked extracted and silently skipped. Archives are searched
in the order provided by `RuntimeComponents.hpp`, which resolves dependencies:

```text
network → threads → audio → graphics → exec → io_fs → text → collections → arrays → oop → base
```

Base is always included. Other components are pulled in based on which `rt_*` symbols the program references.
Windows CRT/runtime shim names that Zanna intentionally supplies from its own runtime archives are not downgraded to
dynamic imports. That exception is scoped to Zanna runtime archive objects; arbitrary user or third-party archives
still produce normal multiply-defined diagnostics.

ELF/COFF common symbols are materialized once after archive resolution into a linker-owned zero-fill section. This
matches tentative-definition semantics and avoids one `.common` allocation per input object. Mach-O underscore
fallback lookup is restricted to macOS; on ELF/COFF, `foo` and `_foo` are distinct symbols.

---

## Section Merging

**Files:** `codegen/common/linker/SectionMerger.hpp/.cpp`, `LinkTypes.hpp`

### Section Classification

Input sections are classified by name and attributes:

| Class | Sections | Permissions |
|-------|----------|-------------|
| **Text** | `.text`, `__TEXT,__text` | R-X |
| **Rodata** | `.rodata`, `.rdata`, `__TEXT,__const` | R-- |
| **Data** | `.data`, `__DATA,__data` | RW- |
| **BSS** | `.bss` | RW- (no file backing) |
| **TLS Data** | `.tdata`, `__DATA,__thread_data` | RW- (thread-local) |
| **TLS BSS** | `.tbss`, `__DATA,__thread_bss` | RW- (thread-local, zero-filled) |

Platform metadata sections that loaders or runtimes find by name are preserved instead of being folded into generic
rodata/data buckets. This includes ObjC metadata, Windows `.pdata`/`.xdata`, ELF EH/note sections, and Mach-O
`__DATA_CONST`/`__AUTH_CONST` sections. Preserved sections reject incompatible alloc/TLS/zero-fill storage
attributes, but conservatively merge executable, writable, and data-segment placement flags when same-named metadata
arrives from a mix of reader-created and linker-synthetic inputs.

### Layout

Output sections are laid out in order: `.text` → `.rodata` → `.data` → `.tdata` → `.tbss` → `.bss`.
TLS data and TLS BSS stay adjacent in address order so ELF `PT_TLS` and PE TLS metadata do not span ordinary
process-global BSS.

Each section starts on a page boundary. Within a section, input chunks are concatenated with their alignment
requirements preserved. Empty alloc sections are skipped only when they have no relocations and no live symbols, so
zero-size labels remain addressable. Chunk sorting uses section class as the primary key and alignment only within
the same class, so a high-alignment data or rodata contribution cannot move ahead of executable code.

Zero-fill inputs carry a logical memory size separately from file-backed bytes. The merger records chunk sizes using
that logical size, but does not append zero bytes to the output `data` vector. This keeps `.bss`/`.tbss` compact
while still giving symbols, TLS layout, segment memory sizes, and relocation bounds the correct address range.

### Base Addresses

| Platform | Base Address |
|----------|-------------|
| macOS | `0x100000000` (4GB, above __PAGEZERO) |
| Linux | `0x400000` (traditional non-PIE) |
| Windows | `0x140000000` (PE default ImageBase) |

The section merger seeds `LinkLayout::imageBase` from `defaultImageBaseForPlatform(platform)` if not set, and
the relocation applier (for COFF `ADDR32NB`) and PE/Mach-O/ELF executable writers all read the same field so a
future per-link image-base override only has to be plumbed through `NativeLinkerOptions` once.

---

## Relocation Application

**Files:** `codegen/common/linker/RelocApplier.hpp/.cpp`

### The Collision Problem

ELF, Mach-O, and COFF relocation type numbers collide numerically. For example, type `1` means:
- ELF: `R_X86_64_64` (64-bit absolute)
- Mach-O: `X86_64_RELOC_SIGNED` (32-bit PC-relative)
- COFF: `IMAGE_REL_AMD64_ADDR64` (64-bit absolute)

### Solution: RelocAction Dispatch

The applier first classifies each relocation by `(format, arch, type)` into a semantic `RelocAction`, then applies
the action uniformly:

```text
(ObjFileFormat, LinkArch, raw type) → RelocAction → patch bytes
```

This avoids a single `switch` with colliding cases.

### Relocation Formulas

| Action | Formula | Width |
|--------|---------|-------|
| **PCRel32** | `S + A - P` | 32-bit |
| **Abs64** | `S + A` | 64-bit |
| **Abs32** | `S + A` | 32-bit |
| **Branch26** | `(S + A - P) >> 2` masked to 26 bits | instruction field |
| **Page21** | `Page(S+A) - Page(P)` encoded as ADRP immhi:immlo | instruction field |
| **PageOff12** | `(S + A) & 0xFFF` into ADD imm12 | instruction field |
| **LdSt64Off** | `((S + A) & 0xFFF) >> 3` into LDR/STR imm12 | instruction field |
| **CondBr19** | `(S + A - P) >> 2` masked to 19 bits | instruction field |
| **GotPage21** | `Page(GOT(S)+A) - Page(P)` for dynamic symbols, direct page for relaxed locals | instruction field |
| **GotPageOff12** | 8-byte-scaled GOT LDR page offset for dynamic symbols, or relaxed ADD page offset for locals | instruction field |

Where: `S` = symbol address, `A` = addend, `P` = patch site address, `Page(X)` = `X & ~0xFFF`.

Relocation symbol lookup resolves local symbols from the referencing object. For non-local weak/global symbols with
a same-object definition, the global table is consulted first so a strong definition in another object correctly
overrides the weak/common definition. This same rule is used by dead-strip liveness propagation.
Weak undefined symbols without a fallback may resolve to address zero only for absolute data relocations (`Abs64` or
`Abs32`); branch, PC-relative, page, GOT, TLS, and instruction-field relocations must still resolve to a real target.

AArch64 instruction-field relocations validate the opcode before patching: branch26 requires `B`/`BL`, page21 and
GOT page21 require `ADRP`, ADD page-offset relocations require ADD-immediate, load/store page-offset relocations
require an unsigned-offset load/store of the exact relocated width, and branch19 requires `B.cond`/`CBZ`/`CBNZ`.

### Range Checking

- AArch64 B/BL: ±128MB (`imm26` × 4)
- AArch64 B.cond/CBZ/CBNZ: ±1MB (`imm19` × 4)
- x86_64: rel32 can reach ±2GB (sufficient for most programs)
- Invalid relocation symbol indexes are rejected before address resolution.
- Relocations whose target symbol cannot be resolved fail even when the object used an anonymous section symbol;
  unresolved targets are never patched as address zero.
- AArch64 branch targets must be 4-byte aligned.
- AArch64 page-offset load/store relocations validate scaled alignment for the instruction size.
- AArch64 ELF GOT relocation types `R_AARCH64_ADR_GOT_PAGE` and `R_AARCH64_LD64_GOT_LO12_NC` are recognized and
  validate GOT-slot alignment before patching. Local GOT relaxation is only applied to adjacent matching
  ADRP/LDR pairs, and the LDR base register must match the preceding ADRP destination. Mach-O TLVP page/pageoff
  pairs use the same register validation while keeping TLV descriptor-address semantics.
- GOT relocations that use a synthetic GOT slot require that slot to have a resolved output address. A stale
  placeholder symbol is diagnosed instead of being treated as address zero.
- Mach-O TLV descriptor offset fixups must be converted to TLS-template-relative offsets. If the target cannot be
  matched to a TLS template section, relocation application fails instead of leaving an absolute address behind.
- Mach-O 64-bit GOT pointer fixups in data-segment sections, including read-only `__DATA_CONST`/`__AUTH_CONST`,
  record rebase entries when they write nonzero addresses.
- x86_64 ELF `GOTPCRELX`/`REX_GOTPCRELX` local relaxation requires the exact RIP-relative `MOV r*, disp32(%rip)`
  encoding before rewriting it to `LEA`.
- `Abs32` relocations must fit in the unsigned 32-bit range.
- COFF `ADDR32NB`, `SECREL`, and `SECTION` relocations are range-checked before narrowing; negative RVAs are rejected.
- COFF `SECTION`/`SECREL` relocations resolve the target output section from the symbol's input-section identity,
  not by searching final addresses, so legal end-of-section symbols are accepted.
- COFF `SECTION` uses a 2-byte patch width, while `SECREL` uses 4 bytes; both can appear near the end of an input
  chunk as long as their actual field width fits.
- COFF ARM64 `SECREL_LOW12L`, page-offset, and GOT page-offset relocations validate that the instruction at the
  patch site is the expected ADD-immediate or unsigned-offset load/store form before rewriting its immediate field.
- COFF ARM64 `SECREL_LOW12A`/`SECREL_HIGH12A` require ADD-immediate instructions. `PAGEOFFSET_12A` requires an ADD
  instruction, while `PAGEOFFSET_12L` requires an unsigned-offset load/store of the matching scale; the linker
  rejects swapped forms instead of guessing the scale.
- A live alloc input section that still has relocations must appear in the output layout. Missing placement is a hard
  error because otherwise the linker would silently skip fixups for live bytes.
- Each live input section may appear in only one output chunk. Duplicate chunk provenance is rejected before
  relocation patching so the linker never has to choose between two final addresses for the same input section.
- Dynamic symbol bindings requested by symbol resolution are honored directly during absolute relocation
  application, even when no synthetic GOT symbol has been inserted yet. Runtime PC-relative, page, branch, GOT, and
  TLS relocations still require a concrete target or a resolved GOT slot.
- Non-alloc debug sections may carry absolute data relocations for preserved metadata, but runtime PC-relative,
  page, branch, and GOT relocations from non-alloc sections to alloc sections are rejected.
- Symbol address, relocation-place, trampoline, and file-offset arithmetic is checked before narrowing or patching.
  Overflow is a hard link error instead of wrapping through `uint64_t` or packed map keys.

### AArch64 Branch Trampolines

The trampoline pass can redirect both global and local branch targets. Local targets are resolved from the merged
section location map, trampoline reuse is keyed by target identity and addend rather than display name alone so
duplicate local labels from different objects cannot collide, and generated trampoline symbol names are checked
against user and global symbols before insertion. After island insertion shifts later chunks, trampoline target
addresses are resolved again before ADRP/ADD/BR encoding. Trampoline placement and AArch64 page relocations
validate address arithmetic before encoding branch islands. Generated island bytes are recorded as synthetic chunks
so later relocation passes do not confuse linker-owned bytes with input sections.
On macOS, trampoline symbol lookup uses the same underscore fallback rules as normal relocation resolution.

If the original branch's patch site falls outside the output section after island insertion (an upstream
corruption or address-arithmetic bug), the trampoline pass aborts the link with an explicit diagnostic instead of
silently emitting the island while leaving the branch unredirected — the dropped patch would otherwise produce a
binary that traps at run time with no link-time diagnostic.

### String Deduplication

String deduplication only scans allocatable sections marked as C-string sections. Non-alloc debug string tables,
generic read-only data, symbols with explicit sizes that do not match the full NUL-terminated byte range, and
sections containing non-local symbols are left un-compacted so aliases cannot invalidate exported or section-level
references.
Duplicate string groups are processed in sorted byte-content order, so synthetic dedup symbol names are stable across
processes and standard-library hash iteration differences.

### Dead Strip and ICF

Dead stripping keeps EH/unwind metadata roots alive (`.eh_frame`, `.gcc_except_table`, `__compact_unwind`, and
`__eh_frame`). Non-alloc debug sections (`.debug*`, `__DWARF`) are rooted only when
`NativeLinkerOptions::preserveDebugSections` is set. ELF constructor/destructor arrays are live by prefix, including
priority-suffixed `.preinit_array.*`, `.init_array.*`, and `.fini_array.*` inputs. Legacy `.ctors*` and `.dtors*`
sections are rooted as constructor/destructor metadata too.
Dead zero-fill sections are stripped by clearing their logical memory size as well as their data/relocation lists, so
unreachable BSS/common storage cannot survive solely because it had no file-backed bytes.
Weak COFF external fallback definitions are also marked live when referenced. Identical Code Folding includes local
relocation identity in function signatures and skips candidates with extra local symbols in the function section,
preventing folds that would strand non-redirectable local labels. Executable-section relocations that materialize a
function address, such as x86_64 `PCRel32` LEA patterns, mark that function address-taken; local and section-symbol
address-taken references are tracked by object/section/offset as well as by name. Only known direct branch
relocations remain foldable.

### Section Ordering

Within merged output sections, Windows `.CRT$*` and `.tls$*` subsections retain lexicographic order, ELF
`.preinit_array.*`/`.init_array.*`/`.fini_array.*` inputs sort by constructor priority before generic alignment
sorting, and Mach-O `__mod_init_func`/`__mod_term_func` inputs preserve source order.

Section merging and virtual-address assignment diagnose alignment exceptions, image-base/page-size overflow, merged
section byte-size overflow, and alloc-section virtual-address overflow instead of truncating arithmetic. Windows
`.pdata` tables must be an exact multiple of the platform unwind-record size before they are sorted. ELF
`.eh_frame`, `.gcc_except_table*`, and `.note*` metadata keep their original output section names rather than being
folded into generic `.rodata`.

---

## Executable Writers

### ELF Executable Writer

**File:** `codegen/common/linker/ElfExeWriter.hpp/.cpp`

Produces a minimal static ELF executable (`ET_EXEC`).

**Layout:**
```text
ELF Header (64B)
Program Headers:
  PT_LOAD (.text, RE)
  PT_LOAD (.rodata, R)
  PT_LOAD (.data, RW)
  PT_GNU_STACK (non-executable stack)
.text segment data
.rodata segment data
.data segment data
.shstrtab
Section Header Table
```

- Entry point set via `e_entry` field
- Each PT_LOAD segment is page-aligned
- `PT_GNU_STACK` flags = `PF_R | PF_W` (no `PF_X`, non-executable stack)
- TLS sections carry `SHF_TLS`; TLS images emit a `PT_TLS` program header whose file size covers initialized TLS
  data and whose memory size extends through `.tbss`.
- Loadable section headers and `PT_LOAD` memory sizes use logical zero-fill sizes while file sizes include only
  materialized bytes.
- Dynamic string/symbol table offsets, generated section placement, startup-stub placement, and final section-header
  table offsets are checked before narrowing or allocation.
- File permissions set to 755 after writing

### Mach-O Executable Writer

**File:** `codegen/common/linker/MachOExeWriter.hpp/.cpp`

Produces a Mach-O executable (`MH_EXECUTE`).

**Layout:**
```text
mach_header_64 (32B, MH_EXECUTE | MH_PIE)
Load Commands:
  LC_SEGMENT_64 "__PAGEZERO" (vmaddr=0, vmsize=4GB, MUST BE FIRST)
  LC_SEGMENT_64 "__TEXT"     (code + rodata, R-X)
  LC_SEGMENT_64 "__DATA"     (data, RW-)
  LC_MAIN                    (entryoff = entry symbol offset within __TEXT)
  LC_BUILD_VERSION           (platform=macOS, minos=14.0, sdk=15.0)
__TEXT segment data (page-aligned)
__DATA segment data (page-aligned)
```

**Key details:**
- `__PAGEZERO` must be the first load command (vmaddr=0, vmsize=0x100000000)
- `LC_MAIN` specifies the entry offset within `__TEXT` file data. A custom `layout.entryAddr` is honored when set;
  otherwise the writer falls back to `main` / `_main`. The entry address must fall inside a `__TEXT` section.
- No CRT objects needed on macOS (unlike Linux)
- Page alignment: 16KB on arm64, 4KB on x86_64
- `MH_PIE` flag set for ASLR
- Dyld rebase opcode emission sorts and coalesces pointer locations before encoding. Duplicate rebase records would
  otherwise apply the ASLR slide more than once to ObjC and other absolute metadata pointers.
- Mach-O-style internal names such as `__LD,__compact_unwind` are split before writing fixed-width section-name
  fields, so the serialized section name is validated against the actual 16-byte Mach-O field.
- Load-command sizes, section counts, link-edit offsets, code-signature offsets, and section file offsets are
  checked against Mach-O's 32-bit fields. Segment data writes now fail if a section would overlap bytes already
  emitted instead of appending at the wrong location.

### PE Executable Writer

**File:** `codegen/common/linker/PeExeWriter.hpp/.cpp`

Produces a PE32+ executable.

**Layout:**
```text
DOS Header (64B, "MZ")
PE Signature (4B, "PE\0\0")
COFF Header (20B)
Optional Header (240B, PE32+):
  ImageBase = 0x140000000
  SectionAlignment = 0x1000
  FileAlignment = 0x200
  Subsystem = WINDOWS_CUI (console)
Section Headers (.text, .rdata, .data)
Section Data (file-aligned to 0x200)
```

**Key details:**
- DLL characteristics: `DYNAMIC_BASE | NX_COMPAT | HIGH_ENTROPY_VA | TERMINAL_SERVER_AWARE`
- Section data aligned to 0x200 (file) and 0x1000 (memory)
- 16 data directory entries; imports, exceptions, base relocations, resources, IAT, and TLS are populated when used
- TLS directory raw-data bounds exclude `.tbss`; `SizeOfZeroFill` covers the zero-fill tail instead of embedding it
  into the TLS template.
- PE section `VirtualSize` uses each output section's logical memory size, so `.bss`/`.tbss` occupy memory without
  being serialized into the file.
- Import-table sizing is checked before 32-bit RVAs are materialized. When reusing external IAT slots, the slot must
  map to a writable output section before the writer seeds it with the import lookup entry.
- Stack reserve: 1MB, heap reserve: 1MB

---

## Platform-Specific Details

### macOS: Code Signing

macOS Ventura+ requires ad-hoc code signing for arm64 executables to launch, and Rosetta 2 requires
it for translated x86_64 binaries, so the linker signs both. `MachOCodeSign.hpp/.cpp` produces the
ad-hoc signature (`LC_CODE_SIGNATURE` load command with a `CodeDirectory` of SHA-256 page hashes)
entirely in-process, using a self-contained SHA-256 with no external tools — consistent with the
zero-dependency policy, so signing works identically even when cross-building off a non-Apple host.

### Linux: CRT Startup

Zanna programs emit a `main` function. On Linux x86_64, the native linker emits its own loader metadata
(`PT_INTERP`, `PT_DYNAMIC`, `DT_NEEDED`, `.dynsym`, `.rela.dyn`) and still enters at `main`; there is no
external `crt1.o` or host linker dependency in the executable link step.

### Windows: CRT

Windows executables need `mainCRTStartup` or link against the CRT DLLs. The native linker generates
minimal PE executables with the entry point at `main` and emits the required DLL import tables itself.

### Thread-Local Storage

The Zanna runtime uses 13 thread-local variables across 9 `.c` files. The linker preserves TLS sections (`.tdata`,
`.tbss`, `__DATA,__thread_data`) when present in input objects. TLS relocations are handled by the relocation
applier using the appropriate format-specific types. ELF output emits `PT_TLS`; PE output emits the TLS data
directory and reports zero-filled TLS separately from initialized template bytes. TLS offset calculations use the
logical memory size of `.tbss`, not the number of bytes materialized in the object model.

On Mach-O, the section merger splits TLV descriptors (`__thread_vars` — fixed 24-byte `{thunk, key, offset}`
records) from TLS template data (`__thread_data` — the initializer image), and tags the descriptor output with
`OutputSection::tlvDescriptors`. `MachOBindRebase` selects descriptor sections by this flag, not by the legacy
`.tdata` name — `.tdata` is reused on ELF/PE for TLS template bytes, which must never be walked as descriptors.
A descriptor section whose size is not an exact multiple of 24 is rejected with a hard link error; silently
truncating the count would leave a trailing thunk field uninitialized and dyld then aborts on first TLV access.

---

## CLI Flags

| Flag | Effect |
|------|--------|
| `--native-link` | Use native linker (this is the default on AArch64) |
| `--system-link` | Deprecated alias for native linking |
| `--native-asm` | Use native assembler (this is the default) |
| `--system-asm` | Override: use system assembler (`cc -c`) instead |
| `--arch arm64\|x64` | Override native target architecture (`zanna build`) |
| `--fast-link \| --no-fast-link` | Toggle the fast link path; skips string dedup + ICF |
| `--stack-size BYTES` | Set the executable stack size (decimal or `0x` hex; `zanna build`) |

Default behavior: the native assembler and native linker are used by default. `--system-asm` still requests
the host assembler for `.s -> .o`, but `--system-link` no longer routes the final executable through `cc`.
`--fast-link` (implied by `-O0`) skips cross-module string deduplication and Identical Code Folding.

### Environment variables

| Variable | Effect |
|----------|--------|
| `ZANNA_LINKER_STATS` | When set to a value other than `0`, prints per-stage link timings to stderr (`[link-time] <stage> <ms>`) |
| `ZANNA_LIB_PATH` | Additional directory searched for runtime/support archives |
| `ZANNA_BUILD_DIR` / `ZANNA_BUILD_TYPE` | Override the discovered CMake build directory / configuration |

The linker keeps parsed immutable archives in a process-wide cache. Cache keys
include the archive path, file size, and last-write time, so a rebuilt runtime
archive is reparsed on the next link while repeated `build-many` targets avoid
rescanning unchanged archive headers and symbol indexes.

Native codegen executable builds pass their newly serialized relocatable object
directly to the linker as bytes. This removes the temporary object-file
write/read cycle without changing explicit object-only output. The file-path
input remains supported for external objects and compatibility callers.

### Known limitations

- **Conditional branches are not trampolined.** Only unconditional `B`/`BL` (`Branch26`) get veneers;
  an out-of-range AArch64 `B.cond`/`CBZ`/`CBNZ` (±1MB) is a hard link error rather than islanded.
- A link may transparently run `cmake --build <dir> --target <lib>` to produce runtime archives that
  are referenced but not yet built.

---

## Source File Map

### Core Types

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/LinkTypes.hpp` | 153 | OutputSection, InputChunk, LinkLayout, GlobalSymEntry, enums |

### Archive Reader (Phase 10)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/ArchiveReader.hpp` | 64 | Archive, ArchiveMember structures |
| `codegen/common/linker/ArchiveReader.cpp` | 328 | GNU/BSD/COFF archive parsing |

### Object File Readers (Phase 11)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/ObjFileReader.hpp` | 119 | Unified ObjFile/ObjSection/ObjSymbol/ObjReloc |
| `codegen/common/linker/ObjFileReader.cpp` | 87 | Format detection and dispatch |
| `codegen/common/linker/ElfReader.cpp` | 296 | ELF 64-bit .o reader |
| `codegen/common/linker/MachOReader.cpp` | 352 | Mach-O 64-bit .o reader |
| `codegen/common/linker/CoffReader.cpp` | 290 | COFF .obj reader |

### Symbol Resolution (Phase 12)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/SymbolResolver.hpp` | 52 | `resolveSymbols()` interface |
| `codegen/common/linker/SymbolResolver.cpp` | 245 | Iterative archive demand-pull |

### Section Merging (Phase 13)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/SectionMerger.hpp` | 44 | `mergeSections()` interface |
| `codegen/common/linker/SectionMerger.cpp` | 226 | Section classification, merging, VA layout |

### Relocation Application (Phase 14)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/RelocApplier.hpp` | 48 | `applyRelocations()` interface |
| `codegen/common/linker/RelocApplier.cpp` | 405 | Format-dispatched relocation patching |

### Executable Writers (Phases 16–18)

| File | LOC | Purpose |
|------|-----|---------|
| `codegen/common/linker/ElfExeWriter.hpp` | 39 | `writeElfExe()` interface |
| `codegen/common/linker/ElfExeWriter.cpp` | 330 | Static ELF executable output |
| `codegen/common/linker/MachOExeWriter.hpp` | 51 | `writeMachOExe()` interface |
| `codegen/common/linker/MachOExeWriter.cpp` | 351 | Mach-O MH_EXECUTE output |
| `codegen/common/linker/PeExeWriter.hpp` | 42 | `writePeExe()` interface |
| `codegen/common/linker/PeExeWriter.cpp` | 267 | PE32+ executable output |

### Additional Linker Passes

| File | Purpose |
|------|---------|
| `codegen/common/linker/BranchTrampoline.hpp/.cpp` | AArch64 branch trampoline insertion for ±128MB range |
| `codegen/common/linker/DeadStripPass.hpp/.cpp` | Unreferenced section removal via reachability |
| `codegen/common/linker/DynStubGen.hpp/.cpp` | Dynamic symbol stub generation |
| `codegen/common/linker/ICF.hpp/.cpp` | Identical Code Folding |
| `codegen/common/linker/MachOBindRebase.hpp/.cpp` | Mach-O bind/rebase opcode emission |
| `codegen/common/linker/MachOCodeSign.hpp/.cpp` | Mach-O ad-hoc code signing |
| `codegen/common/linker/NameMangling.hpp` | Platform-aware symbol name mangling and Mach-O fallback |
| `codegen/common/linker/StringDedup.hpp/.cpp` | String deduplication for merged sections |
| `codegen/common/linker/AlignUtil.hpp` | Alignment calculation utilities |
| `codegen/common/linker/ExeWriterUtil.hpp` | Shared executable writer utilities |
| `codegen/common/linker/RelocClassify.hpp` | Relocation classification utilities |
| `codegen/common/linker/RelocConstants.hpp` | Relocation type constants |

### Top-Level Orchestrator

| File | Purpose |
|------|---------|
| `codegen/common/linker/NativeLinker.hpp` | `nativeLink()` interface + options struct |
| `codegen/common/linker/NativeLinker.cpp` | Full pipeline orchestration (~1,365 LOC) |

### Total: 41 files, ~9,600 LOC

---

## Design Decisions

### Why Not Use LLD?

Zanna's core principle is zero external dependencies. LLVM's LLD is an excellent linker, but it would introduce
a massive dependency on the LLVM project. The native linker is purpose-built for Zanna's specific needs and is
significantly simpler than a general-purpose linker.

### Why Format-Dispatched Relocations?

A common approach is to normalize all relocations to a unified enum (like `RelocKind` in the assembler). However,
the linker must handle relocations from *other* compilers' object files (the runtime `.a` archives contain objects
built by the system C compiler), whose type numbers we cannot control. The `(format, arch, type) → RelocAction`
dispatch cleanly separates format-specific constants from the patching logic.

### Why Static Linking First?

Dynamic linking (PLT/GOT, dyld stubs, IAT) is significantly more complex than static linking. The implementation
started from static linking and then grew native dynamic-import support per platform: Windows import tables,
Mach-O dyld bind metadata on macOS arm64, and ELF loader metadata on Linux x86_64.

### Why Iterative Archive Resolution?

Archives may have cross-dependencies (e.g., `collections` needs symbols from `arrays`, which needs symbols from
`oop`). A single-pass scan would miss these transitive dependencies. The iterative fixed-point algorithm
keeps re-scanning until no new definitions are found, guaranteeing completeness regardless of archive ordering.
