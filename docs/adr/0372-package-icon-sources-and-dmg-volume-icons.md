---
status: accepted
audience: contributors
last-verified: 2026-09-18
---

# ADR 0372: Package Icon Sources and DMG Volume Icons

## Status

Accepted (2026-09-18). Changes the `package-icon` manifest directive from a
single path to a repeatable one, broadens `macos-dmg-icon`, and replaces the
packager's resampler, ICNS record set, PNG writer filtering, and DMG finishing
sequence. Complements [ADR 0317](0317-application-icon.md), which covers the
runtime `Canvas.SetIcon` side of application icons.

## Context

Packaging Cat 'n' Mouse 1.2.6 exposed two packager defects.

**The DMG volume icon was silently lost.** `macos-dmg-icon` copied the icon to
`.VolumeIcon.icns` in the folder passed to `hdiutil create -srcfolder`, attached
the image where Finder could see it, ran the Finder styling AppleScript, set the
root's custom-icon flag with `SetFile -a C`, signed the app, and compressed the
image. The finished image carried the custom-icon flag but no
`.VolumeIcon.icns`, so Finder drew a generic disk, and the build reported
success because nothing checked for the icon. A scratch replay of the exact
command sequence, checking for the file after every step, found:

- `hdiutil create -srcfolder` keeps `.VolumeIcon.icns`.
- A Finder styling session (`open` … `update without registering applications`
  … `close`) **deletes** `.VolumeIcon.icns` from the volume root, whether or not
  the root's custom-icon flag is already set.
- `codesign --force` on a bundle **fails** ("internal error in Code Signing
  subsystem") whenever the bundle's volume root holds `.VolumeIcon.icns`; the
  root flag alone is harmless.
- A same-named volume that was already mounted (the RW image then mounts as
  `… 1`) is not a cause: Finder addresses the disk by its mount-point name and
  the styling applied correctly.

**Packaged icons were low quality.** Every platform size came from one PNG
through a four-tap bilinear resize: at 1024 → 32 it read 4 of the 1,024 pixels
behind each output pixel, sampled corner-aligned (shifting the art up and left
by half an output pixel), blended straight alpha (fringing transparent edges),
and truncated instead of rounding. The same function built Windows `.ico` and
Linux hicolor sizes. The `.icns` lacked the 16 and 32 pixel non-Retina records
Apple's own icons carry (`ic04`/`ic05`), an author could not supply hand-tuned
small sizes, and the PNG writer's filter-None rows made the Cat 'n' Mouse
`.icns` 3.5 MB although its four source PNGs total 1.9 MB.

## Decision

1. **Icon source sets.** `package-icon` is repeatable, one project-relative
   square PNG per line (`PackageConfig::iconPaths`). Each source must be at
   least 16x16; the largest must be at least 32x32. A repeated path is a
   manifest error (`duplicate package-icon path '<p>'`), as are a non-square
   source (`package-icon '<p>' must be a square PNG (got <w>x<h>)`), a tiny one
   (`… must be at least 16x16 pixels`), and two sources of one size
   (`package-icon sources '<a>' and '<b>' are both <n>x<n>; each size may
   appear once`). `zanna package --dry-run --json` reports `"icons": [...]`.
2. **Size selection.** For every platform size, a source of exactly that size is
   reused byte for byte (a Windows `.ico` entry re-encodes a PNG that is not
   8-bit RGBA non-interlaced); otherwise the smallest larger source is shrunk;
   otherwise the largest source is enlarged, and macOS packages warn
   (`warning: largest package-icon is <n>x<n>; icon slots up to 1024x1024 will
   be upscaled`).
3. **Resampler.** A shrinking axis uses an exact-coverage area filter: in units
   of 1/destination, source pixel *s* spans [*s*·D, (*s*+1)·D) and output pixel
   *x* spans [*x*·S, (*x*+1)·S), so every weight is an integer overlap. Colour
   is the alpha-weighted mean and alpha the plain mean, both rounded. A growing
   axis uses bilinear interpolation of premultiplied samples at pixel centres.
   All arithmetic is integer, so packages are byte-identical on every host;
   Lanczos-style filters were rejected because trigonometric weights and FMA
   contraction differ across hosts.
4. **ICNS records.** `ic04` (16) and `ic05` (32) are `ARGB` records — the
   magic `ARGB` followed by the A, R, G, and B planes, each run-length encoded
   (a control byte of 0x80 or more repeats the next byte control − 125 times; a
   smaller one copies control + 1 literal bytes) with straight alpha, matching
   Apple's own files. PNG records follow: `ic11` (32), `ic12` (64), `ic07`
   (128), `ic13`/`ic08` (256), `ic14`/`ic09` (512), `ic10` (1024).
5. **PNG writer.** Each scanline uses whichever of None, Sub, Up, Average, and
   Paeth gives the smallest sum of absolute signed bytes (ties keep the lower
   type), which is lossless and deterministic.
6. **`macos-dmg-icon`.** Accepts a `.icns` file, framing-checked
   (`macos-dmg-icon '<p>' is not a valid ICNS file: <reason>` for a missing
   magic, a size mismatch, an overrunning entry, or no icon entries), or a
   square `.png`, converted with the rules above; any other extension is
   rejected (`… must be a .icns or .png file`). Without it, an app DMG uses the
   app's own `.icns` as its volume icon whenever the package has a
   `package-icon`. `install-package --macos-dmg-icon` accepts the same formats.
7. **DMG finishing.** Both the app and toolchain DMG builders stage without the
   icon, then:
   - *Phase A* attaches the image where Finder can see it and runs the styling
     AppleScript (still best-effort, so headless builds succeed), waits up to
     five seconds for Finder's `.DS_Store`, and detaches.
   - *Phase B* re-attaches it with `-nobrowse` at a private mount point, signs
     the app there, and only then writes `.VolumeIcon.icns` (type `icns`,
     creator `icnC`) and ORs kHasCustomIcon (0x0400) into the root's
     `com.apple.FinderInfo` through `getxattr`/`setxattr`, replacing `SetFile`
     and its Xcode command-line-tools dependency. Any failure here fails the
     build.
   - The compressed image is re-mounted read-only and must contain
     `.VolumeIcon.icns` with exactly the installed bytes and the root flag.
8. **Toolchain mark.** `defaultZannaToolchainIconImage()` renders the Zanna
   mark natively at 1024x1024 with 4x4 integer supersampling, so every
   toolchain icon size is a downscale.

## Consequences

- App DMGs built from projects with a `package-icon` now show that icon as the
  disk icon; the toolchain DMG's volume icon, generated since it was added, now
  actually survives.
- Every generated icon changes bytes (better filtering), and generated PNGs
  shrink. The Cat 'n' Mouse `.icns` drops from 3.5 MB to the size of its reused
  sources plus two small ARGB records.
- `PackageConfig::iconPath` became `iconPaths`; all in-repo callers moved.
- Tests: `Icon.*` resampler, record-set, ARGB round-trip, exact-size reuse, and
  validation tests; `PNG.EncodeFilteredRoundTrips`; `MacOSVolumeIcon.*`; the
  macOS `MacOSAppDmg.*` and `MacOSToolchainDmgBuilder.*` tests re-mount the
  finished image and check `.VolumeIcon.icns` and the root flag; `package_cli`
  covers the repeatable directive and the new errors.
- The toolchain's platform branding still differs (Windows uses the staged
  badge, macOS and Linux the procedural mark); unifying it is a separate
  decision.
