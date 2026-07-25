# Developer architecture

`game.zia` owns the application state machine and long-lived systems. Persistent
authority is deliberately narrow:

- `AbilityManager` owns the six-bit ability mask.
- `WorldState` owns stable interaction and room-discovery masks.
- `SaveManager` owns schema-v4 full-slot checksums, validation, migration, three
  slots, last-write ordering, and corruption backup.
- `WorldGraph` owns ten nodes, twelve bidirectional gated edges, nonuniform
  authored room boundaries, endpoint rooms, and room names.
- `MetaProgression` owns best regional results and New Game+ state.
- `GameSettings` owns global normalized settings.

`Level` builds the active regional tilemap, spawn tables, interaction records,
content anchors, dynamic destructible/crumble state, and regional mechanics.
`EnemyManager`, `ProjectilePool`, `PickupManager`, and `ParticleSystem` use fixed
pools or retained lists. `Renderer`, `GameCamera`, `HUD`, and `MenuSystem` own
presentation; gameplay never depends on authored bitmap availability.

`UiCoordinator` resolves three presentation lanes every frame: one modal, one
top banner, and one lower-third. Explicit maps/lore/ability panels suppress
automatic cards; boss danger outranks room/achievement flavor; actionable
tutorials outrank radio. Tutorials and acquisition cards also own bounded expiry,
dismissal, and replay state rather than relying on draw order.

Simulation uses integer centipixels and millisecond timers. The main Canvas
clamps frame delta. Game Speed scales live gameplay only; hit stop and rumble use
the unscaled frame delta. Collision is axis-separated with a six-pixel upward
corner correction and an 80 ms wall-stick grace. Camera framing has bounded
velocity look-ahead, delayed vertical intent, and a temporary boss-arena focus
window that is released on defeat, recovery, or level load.

`LevelValidator` loads the production builders and constructs a movement-aware
stand-cell graph using the ability mask guaranteed on first entry. It proves the
exit, checkpoint, boss, and every named-room center reachable, in addition to
checking physical world-gateway topology and authored content contracts.

The runtime JSON contains development-default toggles for world graph, authored
art, and adaptive music. Procedural art/audio are permanent fallbacks and are
tested directly, not deleted legacy paths.
