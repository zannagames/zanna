# Ashfall combat-first overhaul

## Outcome

Ashfall is now structured as a fast, combat-led FPS campaign. All nine required
missions are won by fighting, surviving an assault, or moving through an exit
that combat has already secured. No campaign level requires clue following,
console hunting, or collectible hunting. The implementation keeps deterministic
headless coverage and the software-renderer fallback while using the stronger
public 3D features on capable window backends.

The overhaul was implemented entirely inside `examples/games/ashfall`. It was
validated with the installed Zanna compiler and Ashfall's direct probe programs;
Zanna itself was not rebuilt and its ctest suite was not run.

## Implemented recommendations

The source review produced the following action items. All are implemented.

### Combat and weapons

1. **Make every shot originate from the real camera aim.** Hitscan, ricochet,
   beam, projectile, and blast paths now share coherent world-space origins,
   endpoints, normals, and source bearings.
2. **Preserve locational damage end to end.** Body, head, weak-point, and armor
   regions drive multipliers and distinct hitmarker colors.
3. **Make armor readable instead of arbitrary.** Armor regions reduce damage;
   exposed weak points reward precision; boss health displays use scaled maximum
   health rather than an unscaled definition value.
4. **Make the Vanguard shield directional.** Its shield protects a 150-degree
   forward arc; flanking and rear fire bypass it.
5. **Give the rail weapon real penetration.** Multi-hit raycasts can damage up to
   the authored penetration count, stop at hard world geometry, and collapse
   overlapping body/head/weak-point colliders into one logical victim.
6. **Make the Shard Caster actually ricochet.** It reflects from the world normal,
   traces a second segment, and produces feedback along both legs.
7. **Make the Rivet Driver's damage-over-time persistent.** Timed per-enemy DoT
   state now ticks independently after impact rather than collapsing into one
   immediate damage row.
8. **Finish the Helix Lance heat loop.** Beam charge, heat accumulation, lockout,
   cooldown, and the HUD heat/overheat state now agree.
9. **Give projectiles continuous collision.** Fast player and enemy rounds use
   swept/raycast movement so thin geometry is not skipped.
10. **Keep explosions honest.** Blast damage uses world occlusion and distance
    falloff, deduplicates overlapping hit regions, threatens the player as well
    as enemies, and feeds the same damage/event pipeline as direct fire.
11. **Make weapon upgrades change live stats.** Salvage tiers now affect damage,
    spread, and recoil in the active controller rather than only changing meta
    data.
12. **Use focused mission loadouts.** Interactive missions grant three authored
    weapons suited to the encounter instead of dumping all ten guns on the
    player at once. Direct number keys and the mouse wheel make switching clear.

### Player movement and feel

13. **Make crouching physically real.** `Character3D.TrySetHeight` changes the
    capsule while preserving the feet and safely refuses to stand under cover.
14. **Make sliding a complete state.** Sprint-to-crouch creates a timed slide with
    matching collision height, friction, camera roll, view height, and recovery.
15. **Correct movement feedback.** Footsteps accumulate actual horizontal travel,
    stance changes affect hearing radius, and camera landing/sprint/ADS layers
    blend rather than snapping.
16. **Use per-weapon ADS optics.** The camera consumes each weapon definition's
    authored ADS FOV; the marksman rail has meaningfully stronger zoom than a
    shotgun or SMG.

### Enemies, navigation, and encounters

17. **Replace direct-line enemy movement.** Ground enemies use baked
    `NavMesh3D`/`NavAgent3D` paths with local avoidance on interactive backends.
18. **Keep a deterministic fallback.** A world sweep and separation steering
    path remains active when navigation is unavailable in headless/software
    contexts.
19. **Stop enemies stacking in one point.** Avoidance radii, squad separation,
    cover hints, flank pockets, and attack-token limits spread the formation.
20. **Give ranged enemies real projectiles.** A fixed world-space enemy projectile
    pool handles travel, collision, lifetime, player damage, and rendering.
21. **Separate traits that were accidentally coupled.** Flying, boss, cloak,
    shield, turret, and kamikaze behavior are independent; SHRIKE units remain
    airborne without inheriting boss-only behavior.
22. **Make awareness useful but not tedious.** Sight, line of sight, hearing, and
    difficulty scaling feed combat acquisition; missions never ask the player to
    wait through a stealth puzzle.
23. **Use a combat director.** Melee and ranged token caps prevent unfair
    simultaneous attacks while whole incoming formations are deferred whenever
    their overlap would exceed the active combatant cap.
24. **Recover stuck encounters.** Timed overlap, low-threat reinforcement, and
    straggler recovery keep a missed enemy from stalling an entire mission.
25. **Make bosses mechanically distinct.** WARDEN uses plates and pressure,
    SHRIKE Prime controls airspace with movement and volleys, and HELIX changes
    phases, movement, reinforcements, and chamber lighting.

### Campaign and level design

26. **Remove required puzzle objectives.** Required `ACTIVATE` and `COLLECT`
    objectives were eliminated from all nine missions. The level probe rejects
    their return.
27. **Turn every mission into an encounter sequence.** Levels now contain three
    to five authored combat waves with ingress routes, first-move waypoints, and
    escalating compositions.
28. **Prevent endpoint rushing.** Exits unlock only after the required combat
    objective and all authored waves are resolved.
29. **Expose encounter state.** The HUD always reports wave index and remaining
    hostiles, while combat objectives point at a live threat rather than an
    unexplained clue location.
30. **Author readable lanes.** Route strips, landmark silhouettes, floor arrows,
    cover hints, flank pockets, and encounter beat markers make the next fight
    legible at a glance.
31. **Add useful high ground.** Habitat Arcology's rear mezzanine is now a
    collision-backed gallery with two stairways and cover; Storm Terraces has an
    elevated resupply lane. Step rises stay within the player controller limit.
32. **Add combat-reactive props.** Pooled explosive barrels are registered damage
    targets, detonate into the shared occluded blast pipeline, and chain to nearby
    barrels.
33. **Reward movement through the arena.** Health, armor, ammo, salvage, and
    elevated rewards are positioned around tactical routes rather than clue
    trails; nearby pickups visibly bob, rotate, and magnetize to the player.
34. **Give each level a strong identity.** Per-level sky, fog, ambient color,
    accent hue, practical lights, atmosphere, terrain/water, hero prop, cover
    language, and enemy roster replace a repeated gray-box mood.

### Visuals, feedback, and public 3D API use

35. **Lift the crushed presentation.** ACES exposure and color grading were
    recalibrated for readable silhouettes, with restrained bloom and a much
    softer vignette.
36. **Use depth cues on capable GPUs.** Balanced/Cinematic modes add guarded SSAO;
    Cinematic adds capability-gated SSR.
37. **Use the sky as real light.** Capability-gated image-based lighting gives
    PBR surfaces diffuse and specular environment response, with lower intensity
    in dark interiors.
38. **Scale performance safely.** Performance mode requests 85% render scale via
    `TrySetRenderScale`; unsupported backends stay at native resolution without a
    trap.
39. **Cull and light dense scenes.** Frustum and conservative occlusion culling
    are enabled; the try-form of clustered lighting raises the practical-light
    budget where supported while preserving fixed-forward fallback.
40. **Retain cross-platform fallbacks.** GPU-only paths are guarded by
    `BackendSupports`; direct/fill lighting, FXAA, and software-safe tonemapping
    remain the baseline.
41. **Make weapons visible in motion.** Per-weapon emissive muzzle flashes,
    tracers to the real hit endpoint, impact sparks, scorch decals, blast
    particles with capability-gated soft intersections, and short pooled light
    pulses reinforce every shot. The flashlight is a retained moving spot light.
42. **Differentiate projectiles and pickups.** Player projectile types and pickup
    classes use distinct meshes, shapes, emissive colors, motion, and impact
    events instead of one shared placeholder sphere.
43. **Use the existing batching APIs.** Repeated structural props remain cached
    meshes drawn through `InstanceBatch3D`; hero and animated props remain
    retained entities.
44. **Use the environment APIs already in Zanna.** Terrain LOD, layered splat
    materials, animated water, height fog, lens flares, decals, and ambient
    particle volumes all contribute without a new dependency.

### Audio, HUD, accessibility, and progression

45. **Replace silent event bookkeeping with audible feedback.** Every weapon has
    a distinct procedural shot, with enemy shots, impacts, explosions, footsteps,
    hurt, kill, pickup, melee, checkpoint, objective, victory, and defeat cues.
46. **Make audio spatial.** The listener follows the camera; attenuation, physics
    occlusion, a voice budget, reverb routing, and a world reverb zone use the
    public 3D audio API.
47. **Make music react to the fight.** Procedural ambient, combat, and boss loops
    crossfade from real combat intensity and boss state; victory and defeat cues
    latch only for their menus and reset on the next scene.
48. **Replace stealth-centric HUD language.** Detection was replaced by explicit
    hostiles/wave telemetry; objectives, boss bars, prompts, salvage, weapon heat,
    capacity-aware low ammo, kill feed, hitmarkers, and directional damage all
    coexist without overlap.
49. **Support real display sizes.** HUD anchors use the current canvas dimensions,
    including window resize, fullscreen, and render-scale presentation.
50. **Add accessibility controls.** FOV, sensitivity, invert Y, shake from 0–150%,
    flash effects from 0–100%, and an outlined high-contrast reticle are persisted
    and applied live.
51. **Make difficulty affect the whole fight.** Story/Soldier/Veteran scale enemy
    health, awareness, melee/ranged token pressure, and score expectations rather
    than only changing one damage number.
52. **Make progression durable and usable.** A visible Armory on title and
    completion screens buys upgrades with salvage. Profile JSON stores campaign
    unlocks, medals, salvage, upgrade tiers, and run totals; level select respects
    progression.
53. **Make Continue and Retry truthful.** Typed checkpoint JSON restores level,
    wave, player transform/defenses, weapon ownership/ammo, grenades, RNG streams,
    objectives, score, and shot statistics. Completion cannot recreate a cleared
    checkpoint, and Retry preserves the newly recorded death/run totals.
54. **Fix score/medal accounting.** Per-level par times, run restoration, accuracy,
    totals, and medal mapping now agree with the completion screen and profile.

## Mission-by-mission combat structure

| Mission | Combat structure | Readability and tactical identity |
|---|---|---|
| L1 Crashsite Canyon | Four-wave counterattack, then a secured exit | Wreck arch landmark, canyon flanks, explosive barrel clusters |
| L2 Relay Outpost | 40-second automatic upload defense, then cleanup | Bulkhead lanes, relay sightline, overlapping garrison pressure |
| L3 Ashveil Crossing | Four formations staged along the bridge, then cross | Strong forward axis, ridge cover, anti-rush combat gate |
| L4 Habitat Arcology | Four-wave shielded garrison push | Column grid, flank lanes, playable rear mezzanine and high reward |
| L5 Hydroponics Caverns | Four-wave Wraith/Stalker ambush | Biolume pool, close cover, explicit flank counterplay |
| L6 Storm Terraces | Five overlapping assaults over 60 seconds, then cleanup | 360-degree holdout, storm strobes, elevated resupply terrace |
| L7 The Foundry | WARDEN Goliath plus staged support waves | Armor-breaking boss, crucible cover, furnace hazards |
| L8 Spire Ascent | SHRIKE Prime aerial battle plus air/ground support | Circular beacon movement, long sightlines, anti-air loadout |
| L9 Helix Core | Multi-phase HELIX Avatar finale with reinforcements | Concentric arena, phase-colored core, shifting pressure lanes |

The Redoubt hub keeps its workbench visit optional; purchases are available
directly in the title/completion Armory. The hub is not part of campaign
completion and therefore does not reintroduce puzzle gating.

## Public 3D API assessment

The overhaul reviewed the live runtime inventory and the repository's 3D API
documentation before selecting features. The following public surfaces are now
used where they improve play or presentation:

- `Character3D.TrySetHeight`, slope/step resolution, and fixed-step movement;
- `Physics3DWorld.Raycast`, `RaycastAll`, and `SweepSphere` for combat and safe
  fallback steering;
- `World3D.BakeNavMesh` and `NavAgent3D` target/avoidance controls;
- `Canvas3D.BackendSupports`, `TrySetRenderScale`, frustum culling, and occlusion
  culling;
- skybox-driven IBL, capability-gated cascaded shadows, bounded shadow distance,
  try-enabled clustered lighting, height fog, retained moving spot lights,
  practical/pulse lights, and lens flares;
- `PostFX3D` SSAO, SSR, ACES tonemap, bloom, color grade, vignette, and FXAA;
- `InstanceBatch3D`, retained animated entities, terrain LOD/layers, `Water3D`,
  capability-gated `Particles3D.SetSoftness`, and `Decal3D`;
- `World3D.Audio`, camera listener following, attenuation, physics occlusion,
  reverb zones/routing, procedural synthesis, and `MusicGen`.

TAA, motion blur, and depth of field were deliberately not enabled: this is a
fast mouse-look shooter, and temporal trails or focus blur reduce target and
projectile clarity. GPU-only effects are never assumed; capability checks keep
Metal, D3D11, OpenGL, software, and headless behavior within the project's
cross-platform contract.

## Validation contract

Ashfall has 13 direct probes covering core determinism, movement, performance
budgets, weapons/damage/audio mapping, enemy behavior/navigation/bosses, all
levels and hazards, manifest round-trip, economy/difficulty/scoring, the render
pipeline, complete campaign progression, menu/settings/checkpoints, optional
assets, and bounded smoke execution.

The important regression assertions include:

- no required `ACTIVATE` or `COLLECT` objective in campaign levels;
- at least three waves, twelve combatants, cover/flank hints, landmarks, and
  encounter beats per required mission;
- endpoint rushing cannot complete a combat-gated mission;
- L4/L6 raised combat routes and rewards remain authored;
- explosive barrels detonate through the damage pipeline;
- penetration stops at walls and overlapping target regions cannot multiply rail
  or blast damage;
- player, barrel, sapper, and grenade blasts share falloff/self-damage behavior;
- front/rear Vanguard shield behavior differs correctly;
- enemy projectiles resolve player-versus-wall impact order correctly and pooled
  AI—including overlapping holdout formations—stays within director caps;
- arena geometry/projectiles unload cleanly before campaign/title scenes;
- all nine missions reach completion in one deterministic campaign session;
- checkpoint state, completion cleanup, death totals, Armory/profile progression,
  and accessibility settings round-trip through JSON;
- all source type-checks cleanly as one project.
