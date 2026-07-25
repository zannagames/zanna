# Accessibility and difficulty

All settings are application-wide, persist independently of campaign slots,
and apply live unless a transition is explicitly documented.

| Setting | Choices / behavior |
|---|---|
| Master, Music, SFX, Ambience, ARIA | Independent 0-100 mixer values |
| Fullscreen | Windowed / fullscreen |
| UI Scale | 100%, 125%, 150% |
| High Contrast | Opaque UI backing and bright player outline |
| Large Text | Adds one bounded bitmap-font scale step |
| Reduced Motion | Removes title drift/bounce, camera intent sweeps, trails, and shake |
| Reduced Flashes | Clamps full-screen flash alpha to 64 |
| Screen Shake | 0-100%; Reduced Motion always suppresses it |
| Rumble | Cross-platform controller vibration; safe no-op where unsupported |
| Charge Input | Hold or toggle |
| Navigation Hints | Off, Contextual, Always |
| Damage Assist | 25%, 50%, 75%, 100% incoming damage |
| Game Speed | 75%, 90%, 100% gameplay simulation; menus stay full speed |
| Subtitles | Shows or hides nonblocking ARIA radio captions |
| HUD Detail | Minimal, Standard, Full; minimap appears only in Full |
| Aim Assist | Off, Standard, Strong; expands projectile target tolerance without changing damage |

Linux currently treats controller vibration as a no-op through Zanna's platform
adapter. This does not affect input or progression.

## Difficulty profiles

Explorer uses 50% incoming damage, 85% enemy HP, 75% aggression, and no salvage
loss. Standard uses 100/100/100 and 10% recoverable loss. Veteran uses 150%
incoming damage, 125% enemy HP/aggression, and 20% recoverable loss.

Difficulty can be lowered at a save station immediately. An increase is queued
until the next regional transition so enemies do not change health mid-fight.
No achievement requires Veteran.

All menu rows use the same semantic actions as gameplay, including gamepad
navigation. World-map cards, lore/archive text, tutorial cards, ability notices,
radio captions, and profile screens apply bounded large-text/high-contrast
layouts. Modal, top-banner, and lower-third ownership is arbitrated centrally so
accessibility text cannot stack underneath another automatic popup.
