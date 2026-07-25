---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0175: Give Windows Setup a Native Zanna Games Experience

## Status

Accepted (2026-07-24)

## Context

ADR 0103 made the Windows toolchain installer native, self-contained,
transactional, and accessible. Its primary interactive pages nevertheless used
Windows Task Dialogs. Those pages inherited a light, generic application
installer appearance and could not express the dark visual language shared by
zannagames.com and Zanna Studio.

Setup is a developer's first executable Zanna experience. It needs to
communicate the from-scratch toolchain, source-to-native pipeline, and Zanna
Games identity without adding an installer framework, a web runtime, font
download, or image-decoding dependency. Branding cannot weaken the existing
keyboard, screen-reader, high-contrast, small-work-area, DPI, cancellation, or
rollback contracts.

## Decision

The primary Windows setup journey uses a repository-owned native Win32 shell
for Welcome, maintenance choice, transaction confirmation and license
acceptance, progress, and successful completion. The existing component and
integration picker uses the same visual system. Error prompts, Restart Manager
interactions, and update-result dialogs may remain focused native Windows
dialogs when a custom page would not improve the decision.

The canonical visual system follows the public Zanna Games dark palette:

- background `#0c1214`, raised background `#10181b`, and surface `#131c20`;
- primary text `#dde7e4`, muted text `#9ab0ab`, and borders `#24343a`;
- Zanna green `#8cc63f`, steel `#a9b8bd`, and teal `#2bc8c4`; and
- the green-to-steel-to-teal compile rail as the persistent setup progress
  signature.

The shell renders the Z mark, circuit field, rails, borders, and action cards
with GDI vector primitives at the active DPI. It does not decode or embed the
large raster brand references under `misc/images`; those files remain visual
inspiration. Body text uses Segoe UI. Code-like labels and headings prefer
Cascadia Mono and deterministically fall back to the Windows-provided Consolas
family.

Every action remains a real child `BUTTON`, `EDIT`, `STATIC`, checkbox, or radio
control. Owner drawing changes presentation only: the complete action title and
description remain the native accessible name, tab order remains native, Enter
and Space activate focused controls, focus is visibly outlined, and dialog
navigation continues through `IsDialogMessage`. License acceptance is enforced
by the selected transaction action rather than by color or pointer state.

Pages cap themselves to the current work area and expose bounded native
scrolling when their logical content does not fit. Progress work remains on a
joined worker thread, logger messages cross the UI boundary through owned
posted strings, and cancellation remains cooperative and rollback-safe.

When Windows high-contrast mode is active, branded RGB values are replaced by
system window, text, highlight, and disabled-text colors. Windows 10 controls
whose visual styles ignore `WM_CTLCOLOR` in dark mode use classic native
rendering so labels cannot become dark-on-dark. No visual effect is allowed to
be the only representation of state.

## Consequences

- Setup is recognizably part of Zanna Games before it changes the machine.
- Website, Studio, and installer colors and typography now share one explicit
  palette instead of drifting through approximate dark themes.
- The installer gains two small Windows-only presentation modules and links
  only additional Windows system libraries (`dwmapi` and `gdi32`).
- Primary setup pages are more code than Task Dialog configuration, so palette,
  contrast, DPI, scrolling, focus, high-contrast, and cancellation behavior
  require direct validation.
- Raster wallpaper and logo files do not enlarge the bootstrap or introduce a
  decoder attack surface.

## Alternatives Considered

- **Keep Task Dialogs and add only an icon.** Rejected because the layout and
  light surface still read as a generic installer.
- **Host HTML/CSS from the website.** Rejected because a browser runtime adds a
  large dependency and weakens offline, accessibility, determinism, and attack
  surface contracts.
- **Embed the 1024- and 1920-pixel brand PNGs.** Rejected because fixed raster
  artwork is heavier, less crisp across 100–300 percent DPI, and unnecessary
  for the geometric Zanna mark.
- **Build a bespoke graphics widget toolkit for setup.** Rejected because
  native child controls already provide the accessibility and keyboard
  semantics setup needs.

## Compatibility

Command-line switches, package metadata, lifecycle exit codes, component
selection, transaction semantics, and unattended modes do not change. Quiet
mode creates no UI. Passive mode uses the branded progress surface. This ADR
supersedes only the Task Dialog and system-light presentation choice in the
“User experience and integration” section of ADR 0103; all of ADR 0103's
installer architecture and accessibility requirements remain active.

