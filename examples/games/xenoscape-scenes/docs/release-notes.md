# XENOSCAPE 0.3.0 development-preview stabilization report

XENOSCAPE is not version 1.0. The former 1.0 and commercial-release claims were
incorrect and have been retracted from project metadata and documentation. This
report records the implemented playability and polish work in development
version 0.3.0; it does not replace cross-platform release certification.

## Twenty-five implemented upgrades

1. **Truthful versioning.** Project metadata, README, package description, asset
   language, and this report identify a 0.3.0 development preview. A future 1.0
   requires the platform matrix in `testing.md`.
2. **Baseline Double Jump.** New games, Player defaults, AbilityManager, empty
   slots, and migrated saves start with Wall Jump plus Double Jump. Existing
   geometry can no longer be made impossible by withholding its required move.
3. **Physics-aware traversal proof.** Level validation now floods actual
   standable tile cells using campaign-entry jump, double-jump, wall-jump,
   horizontal-control, fall, destructible, and gate constraints.
4. **Gold-slice playthrough gate.** A focused probe proves Crash Site and Fungal
   Caverns, both physical endpoints, the Spore Mother Dash reward, and the route
   it unlocks before the full campaign gate runs.
5. **Overlay coordination.** One coordinator arbitrates modal, top-banner, and
   lower-third lanes. Maps, lore, abilities, transitions, boss warnings, room
   titles, achievements, tutorials, messages, and ARIA no longer stack freely.
6. **Bounded guidance.** Tutorial cards expire after seven seconds, accept Back
   dismissal, remain profile-acknowledged only after demonstrated actions, and
   can be replayed. Ability cards expire after 1.8 seconds or explicit input.
7. **Single ability authority.** AbilityManager owns grants, persistence mask,
   and acquisition presentation. Duplicate transition popups were removed and
   re-grants remain idempotent.
8. **Readable HUD hierarchy.** The always-on strip is limited to health and
   arcade counters; salvage, lore, abilities, minimap, objective, boss, and room
   information occupy bounded secondary surfaces controlled by HUD Detail.
9. **Text and modal repair.** Lore, archive, bestiary, tutorials, and radio use
   bounded wrapping/scaling. Every reversed `TextScaled` color/scale call was
   corrected, eliminating giant or malformed profile, pause, debrief, ability,
   lore, boss-pointer, hub, and damage-number text.
10. **Authored rooms.** All ten regions use eight named, nonuniform room
    boundaries instead of deriving identity from equal percentages. Landmarks,
    tutorials, mastery beats, set pieces, lore, and secrets reference room data.
11. **Traversable geometry.** The validator exposed and drove repairs to a
    sealed Crystal exit, fused Thermal and Overgrowth walls, unsupported Sky and
    Corrupted checkpoints, and a blocked Corrupted cloud route.
12. **Physical world graph.** Every graph edge now has authored endpoint rooms
    and an in-level gateway carrying its edge id, destination, and ability gate.
    Arrival spawns at the reciprocal endpoint rather than an abstract map jump.
13. **Ability curriculum.** The baseline teaches Wall/Double Jump; Spore Mother
    grants Dash; Crystal Wyrm or the nonlinear Lake shrine grants Charge Shot;
    Magma grants Ground Pound; Overgrowth grants Grapple. Graph gates and probes
    use that same order.
14. **Interaction discoverability.** The nearest actionable authored record gets
    a world-space focus ring and semantic live-binding prompt, including named
    region destinations and locked-route feedback.
15. **Recovery tools.** Pause now offers Return to Checkpoint. It restores the
    latest safe state without recording a death or dropping salvage, while the
    ordinary death marker loop remains available by difficulty.
16. **Combat feel and correctness.** Stomps require real top-half downward
    contact, nonlethal hits produce damage numbers, charged hits add feedback,
    immune targets use a distinct spark, and boss phases retain tells, hit stop,
    particles, rumble, and projectile clearing.
17. **Regional bosses.** The ten areas now culminate in Beacon Warden, Spore
    Mother, Crystal Wyrm, Relay Custodian, Tide Leviathan, Magma Core, Verdant
    Colossus, Cryo Custodian, Null Harvester, and the Architect. New regional
    climaxes have unique stats, symbols, effects, objectives, warnings, and
    phase-volley signatures.
18. **Progression hierarchy.** Salvage is named consistently as the only
    spendable campaign currency; coins and score are explicitly arcade/ranking
    counters. Objectives defer to active boss danger and the HUD can be reduced.
19. **Camera framing.** Existing velocity look-ahead and vertical intent now
    yield to a bounded boss-arena focus window, released on defeat, recovery, or
    level load so offscreen threats and post-fight lock-in are avoided.
20. **Coherent visual direction.** Title and ARIA art were replaced with original
    crisp pixel-art assets using the navy/cyan/magenta/amber game language.
    Regional boss cores, VFX families, and accessibility-safe panels reinforce
    that direction while deterministic code-native fallbacks remain available.
21. **Audio and narrative pacing.** The redundant level-name splash was removed;
    the opening is skippable and bounded; ARIA remains movement-safe and
    subtitle-backed; radio activity ducks music/ambience instead of competing
    with it; every regional boss has a short specific warning.
22. **Accessibility and input parity.** Twenty persistent settings now include
    Minimal/Standard/Full HUD and Off/Standard/Strong aim assist. Large text and
    contrast cover maps, profiles, lore, tutorials, abilities, and radio. Dash,
    Grapple, Interact, Map, navigation, and dismissal have named keyboard and
    gamepad actions with live prompts.
23. **Save hardening.** Schema 4 computes a full-slot checksum, stages complete
    snapshots, preserves corrupt backups, selectively retires damaged slots,
    migrates old baseline abilities, and has round-trip, legacy, tamper, and
    malformed-store probes.
24. **Release-readiness gates.** New playthrough, UI-flow, 30/60/120-Hz cadence,
    3,600-frame soak, expanded large-text render, ten-level reachability, full
    campaign, deterministic performance, package, save, settings, mechanics,
    world, and meta probes are registered and runnable directly.
25. **Reproducible packaging.** `zanna.project` packages the runtime assets and
    tuning under version 0.3.0. Tarball dry-run, host compilation, inventory,
    extraction, and `--zanna-package-smoke` all have documented commands and
    were exercised outside the source tree.

## Verification record

On 2026-07-10, the project directory type-checked cleanly and every direct
Xenoscape probe reported `RESULT: ok`. All ten levels passed critical
reachability. A macOS arm64 tarball was created and its extracted executable
reported `RESULT: package_smoke_ok`. No Zanna rebuild or Zanna CTest run was
performed, honoring the shared-worktree constraint.

## Remaining release authority

The code and project gates are substantially more complete, but 0.3.0 remains a
development preview. A 1.0 declaration still requires hands-on keyboard/gamepad
exploratory playthroughs and clean package/install evidence on the intended
macOS, Windows, and Linux matrix. Documentation must not infer those results
from one host or from deterministic probes.
