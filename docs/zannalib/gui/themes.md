---
status: active
audience: public
last-verified: 2026-08-17
---

# GUI Themes and Palettes

`Zanna.GUI.Theme` selects the active app's appearance. `Zanna.GUI.ThemePalette`
is an independently mutable logical palette that can be validated, cloned, and
installed without exposing the lower C theme structure.

The exhaustive generated signatures are in the
[Theme](../../generated/runtime/gui.md#zanna-gui-theme) and
[ThemePalette](../../generated/runtime/gui.md#zanna-gui-themepalette)
references.

## Modes

Theme modes have stable integer values:

| Value | Mode | Behavior |
|---:|---|---|
| `0` | Dark | Deterministic built-in dark palette; the default |
| `1` | Light | Deterministic built-in light palette |
| `2` | System | Follows the host application's light/dark preference |
| `3` | Custom | Uses the last palette accepted by `SetPalette` |

`FollowSystem()` is equivalent to `SetMode(2)`. System mode samples the host
immediately and checks it again as GUI frames run. `GetMode()` continues to
return `2`; it does not collapse System into the currently resolved light or
dark appearance.

The compatibility methods `SetDark()`, `SetLight()`, and `GetName()` remain
available. `GetName()` reports the selected mode as `dark`, `light`, `system`,
or `custom`.

Modes and custom palettes are app-scoped. Select the intended app with
`App.MakeCurrent()` before changing its theme when a process owns multiple GUI
apps.

## Palette lifecycle

`ThemePalette.New()` and `FromDark()` start from the built-in dark palette;
`FromLight()` starts from light. `Clone()` returns independent storage.

`Theme.SetPalette(palette)` performs these operations atomically:

1. validates token values, font liveness, and required text contrast;
2. clones the logical palette into the active app;
3. builds a separate effective copy at `windowScale * uiScale`;
4. composes high-contrast and reduced-motion preferences;
5. invalidates theme-dependent layout and painting; and
6. selects Custom mode and increments the theme revision.

The supplied palette can be changed or released after a successful call.
Those later edits do not mutate the installed app. `GetPalette()` similarly
returns an independent logical snapshot, before DPI or accessibility
transforms. `ResetCustom()` discards the app-owned custom copy and switches an
active Custom mode safely back to Dark.

`SetPalette` returns false for a missing active app, invalid handle, failed
validation, stale font role, or allocation failure. The old installed palette
and mode remain intact on failure.

## Validation contract

Color and metric setters return false only when the token name is unknown (or
contains an embedded NUL). A recognized but invalid value returns true and
preserves the previous field. `Validate()` then reports the first invalid token
as:

```text
GUI theme token <name> has an invalid value
```

A valid setter for the same token repairs that pending error. This split lets a
configuration loader distinguish a misspelled schema key from a known key with
bad data.

Validation checks:

- colors are integers from `0x000000` through `0xFFFFFF`;
- all floating-point metrics are finite and within their documented range;
- integer, byte, and Boolean metrics have integral values;
- font roles are null or live `Zanna.GUI.Font` handles; managed role handles are retained by the
  palette and installed app themes use presentation-generation retirement;
- `fgPrimary` on `bgPrimary` and `fgSecondary` on `bgSecondary` each meet a
  WCAG contrast ratio of at least 4.5:1.

`Validate()` returns `Zanna.Result.Ok(1)` on success or `Err(String)` on
failure. Applying a palette performs the same validation even if the caller
does not invoke `Validate` first.

## Color tokens

Colors use packed 24-bit `0xRRGGBB` integers.

| Group | Tokens |
|---|---|
| Surfaces | `bgPrimary`, `bgSecondary`, `bgTertiary`, `bgHover`, `bgActive`, `bgSelected`, `bgDisabled` |
| Text | `fgPrimary`, `fgSecondary`, `fgTertiary`, `fgDisabled`, `fgPlaceholder`, `fgLink` |
| Accents | `accentPrimary`, `accentSecondary`, `accentDanger`, `accentWarning`, `accentSuccess`, `accentInfo` |
| Borders | `borderPrimary`, `borderSecondary`, `borderFocus` |
| Syntax | `syntaxKeyword`, `syntaxType`, `syntaxFunction`, `syntaxVariable`, `syntaxString`, `syntaxNumber`, `syntaxComment`, `syntaxOperator`, `syntaxError` |
| Effects | `elevationShadowColor`, `focusGlowColor` |

## Metric tokens

Spatial metrics are logical units. They are scaled once when installed. Motion
durations are milliseconds and are not DPI-scaled. Alpha values range from 0
through 255.

| Group | Tokens and ranges |
|---|---|
| Typography | `fontSizeSmall`, `fontSizeNormal`, `fontSizeLarge`, `fontSizeHeading` (`> 0`, up to 1024); `lineHeight` (`0.5`–`4.0`) |
| Spacing | `spacingExtraSmall`, `spacingSmall`, `spacingMedium`, `spacingLarge`, `spacingExtraLarge` (non-negative) |
| Button | `buttonHeight` (`> 0`), `buttonPaddingHorizontal`, `buttonRadius`, `buttonBorderWidth` (non-negative) |
| Input | `inputHeight` (`> 0`), `inputPaddingHorizontal`, `inputRadius`, `inputBorderWidth` (non-negative) |
| Scrollbar | `scrollbarWidth` (`> 0`), `scrollbarMinThumbSize`, `scrollbarRadius` (non-negative) |
| Radius scale | `radiusNone`, `radiusSmall`, `radiusMedium`, `radiusLarge`, `radiusExtraLarge`, `radiusPill` (non-negative) |
| Elevation 0–3 | `elevationLevelNBlur` (non-negative), `elevationLevelNX`, `elevationLevelNY` (signed integer), `elevationLevelNAlpha` (`0`–`255`) |
| Gradient | `gradientEnabled` (`0` or `1`), `gradientStrength` (`0.0`–`1.0`) |
| Focus | `focusGlowWidth` (non-negative), `focusGlowAlpha` (`0`–`255`) |
| Motion | `motionEnabled` (`0` or `1`), `motionHoverMs`, `motionPressMs`, `motionFocusMs` (`0`–`60000`) |

`SetMotionEnabled(Boolean)` is the typed convenience for `motionEnabled`.

Direct C-field-style aliases are also accepted for metric migration and
tooling: `typographySizeSmall`, `typographySizeNormal`,
`typographySizeLarge`, `typographySizeHeading`, `typographyLineHeight`,
`spacingXs`, `spacingSm`, `spacingMd`, `spacingLg`, `spacingXl`,
`buttonPaddingH`, `buttonBorderRadius`, `inputPaddingH`,
`inputBorderRadius`, `scrollbarBorderRadius`, `radiusSm`, `radiusMd`,
`radiusLg`, `radiusXl`, and `elevationLevel0Dx`/`Dy` through
`elevationLevel3Dx`/`Dy`. Getters return the same field through either name;
validation diagnostics use the canonical names in the table.

## Font roles

`SetFontRoles(regular, bold, mono)` changes all three roles atomically. A null
argument clears that role. Every non-null value must be a live GUI font, or no
role changes and `Validate()` identifies the first invalid role as
`fontRegular`, `fontBold`, or `fontMono`.

A palette borrows these font handles while it is only a palette value. Keep the
fonts live until applying or discarding it. Once installed, app font retirement
protects a referenced role until the app no longer uses it. Regular and bold
roles are intended for application chrome; mono is intended for editors and
fixed-grid text.

## Accessibility composition

High contrast is an app presentation preference layered over the selected
palette. It replaces surface, text, state, border, and syntax colors with the
deterministic high-contrast set, disables subtle gradients, and preserves the
custom logical metrics. Reduced motion disables the installed copy's motion
without changing the stored palette. Turning either preference off rebuilds
from the same logical custom base, so original values are not lost.

## Observing changes

`GetRevision()` is monotonic and non-consuming, so multiple observers can keep
their own last-seen values. `WasChanged()` is a compatibility edge: it returns
true once after an installed theme changes, then false until the next mode,
palette, scale, accessibility, or followed System appearance change.

## Example

```zia
bind Zanna.GUI.Theme as Theme;
bind Zanna.GUI.ThemePalette as ThemePalette;

var palette = ThemePalette.FromDark();
palette.SetColor("accentPrimary", 0x5B8CFF);
palette.SetColor("borderFocus", 0x8FB0FF);
palette.SetMetric("buttonHeight", 32.0);
palette.SetMetric("radiusMedium", 8.0);
palette.SetMetric("focusGlowWidth", 2.5);
palette.SetMotionEnabled(true);

var checked = palette.Validate();
if checked.IsOk() {
    if !Theme.SetPalette(palette) {
        Zanna.Terminal.Say("The custom theme could not be installed");
    }
}

// A separate observer can compare revisions without consuming WasChanged.
var seenTheme = Theme.GetRevision();
// ... run frames ...
if Theme.GetRevision() != seenTheme {
    seenTheme = Theme.GetRevision();
}
```

See also [GUI Core & Application](core.md),
[Accessibility](application.md), and the
[GUI modernization ADR](../../adr/0107-gui-theme-accessibility-input-and-render-policy.md).
