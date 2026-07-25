# Profiles, saves, and recovery

XENOSCAPE has three player-selected expedition slots and a separate global
settings store. The current development campaign schema is version 4.

Each profile summary reports area, checkpoint, playtime, completion percentage,
difficulty, maximum HP, ability count, upgrade tiers, banked salvage, and a
compact local last-write stamp. Continue selects the most recently written valid
profile. Migrated profiles without a timestamp fall back to farthest area,
playtime, and slot number. Empty Continue and Load actions show explicit
nonblocking messages. Deletion and overwrite require confirmation.

## Autosave points

Progress is saved at save stations, permanent ability acquisition, boss defeat,
permanent Workbench purchase, regional entry, postgame-result commit, and a
clean return to title or application exit. Simulation modes first save a
campaign snapshot; their world, pickups, deaths, and interactions are discarded
on return. A Time Trial commits only an improved regional time/rank.

## Persistent campaign data

The snapshot includes abilities, max HP, banked salvage, keys, lore, bestiary,
upgrades, difficulty, map rooms, switches, doors, shrines, teleporters,
checkpoint coordinates, tutorials, regional best times/ranks, and New Game+
state. Temporary enemies, projectiles, power-ups, and room combat state are
rebuilt after death or reload.

## Corrupt or failed saves

Every populated schema-v4 slot stores a bounded checksum over its core fields,
world masks, rooms, best times, difficulty, tutorials, and last-write stamp.
This detects a syntactically valid but partial or altered snapshot as well as a
malformed store. Damaged data is copied to a `.corrupt.bak` file when the
platform permits, then only the invalid slot is retired. The game reports:

> Save data was unreadable. A backup was preserved and defaults were restored.

A failed write does not bank unbanked salvage and reports the storage-permission
message without blocking play. Save probes use isolated SaveData namespaces and
never alter production profiles.

`SaveData.Save()` writes the complete JSON payload through a same-directory
temporary file, flushes it, and atomically replaces the live save. A failed
write therefore leaves the prior valid payload in place.

Schema-1/2/3 profiles migrate additively. In particular, a legacy profile that
contains only Wall Jump receives the restored baseline Double Jump bit before
it is offered to Continue; the migrated snapshot is immediately rewritten with
schema 4 and a full-slot checksum. The isolated progression probe covers round
trip, migration, deliberate checksum tampering, malformed JSON backup, and
selective recovery.
