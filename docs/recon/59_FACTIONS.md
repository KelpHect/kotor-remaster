# Recon 59 - faction reputation from repute.2da and REPUTE

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Shipped matrix

`repute.2da` is a square **21 x 21** table (V2.b). Its layout is unusual:

- the row label is the numeric faction id (`0`..`20`);
- column 0 (`label`) holds the faction **name**, not a relation;
- row 0 holds the player's relations to factions 1..20;
- cell `[faction][column]` (column >= 1) is that faction's standing toward the
  faction with the same numeric id.

Names: `Player`, `Hostile_1`, `Friendly_1`, `Hostile_2`, `Friendly_2`,
`Neutral`, `Insane`, `ptat_Tuskan`, `glb_xor`, `Surrender_1`, `Surrender_2`,
`Predator`, `Prey`, `Trap`, `Endar_Spire`, `Rancor`, `Gizka_1`, `Gizka_2`,
`Czerka`, `Zone_Controller`, `Sacrifice`.

The baseline matrix is symmetric for player relations (column 0 is mirrored
from row 0); faction-to-faction cells keep their shipped directional values.
Examples used by the gate:

| pair | value |
|---|---:|
| Player / Hostile_1 | 0 |
| Player / Friendly_1 | 100 |
| Player / Neutral | 50 |
| Player / Endar_Spire | 0 |
| Hostile_1 / Hostile_1 | 100 |
| Czerka / Sacrifice | 0 |
| Zone_Controller / Czerka | 100 |

Standing 0 is hostile, 50 neutral, 100 friendly; `hostile()` is `< 50`.

## Save FAC payload

A retail `REPUTE` (`FAC ` GFF) carries:

- `FactionList`: 21 structs with `FactionName`, `FactionParentID` (-1) and
  `FactionGlobal` (1);
- `RepList`: 405 structs with `FactionID1`, `FactionID2`, `FactionRep`.

405 = 441 - 36: every pair whose value is **100** is omitted, and a missing
pair means 100. The Taris save matches `repute.2da` except for
`Player / ptat_Tuskan`, which it stores as 50 after play.

`FactionSystem::saveFac` writes the same shape: all factions, then every
non-100 cell in ascending `(FactionID1, FactionID2)` order. Loading a save
resets the matrix to 100 and applies `RepList`; an empty FAC (the synthetic
fresh-save scaffold) keeps the `repute.2da` defaults.

## Runtime

`engine/src/game/Factions.*` provides `reputation`, `setReputation`,
`adjustReputation` (clamped 0..100), `hostile`, `loadFac` and `saveFac`.
`GameSession` owns one instance:

- loads `repute.2da` at startup;
- `GetReputation` (action 208) maps both objects to faction ids and reads the
  matrix; `AdjustReputation` (209) writes one direction and refreshes hostility;
- `GetFactionAverageReputation` (186) averages party members' standings;
- script objects carry their entity's `faction` attribute;
- a loaded save replaces the matrix from `REPUTE`, and `saveOriginalSave`
  rebuilds `REPUTE` from the live matrix.

Hostility for GIT creatures is now derived from `repute.2da` instead of the
old hardcoded `FactionID == 4`. Encounter spawns keep an explicit
`hostileOverride` because those templates are placed as attackers. On the
Endar Spire slice the first hostile is now a `SithSoldier` (faction 1), and
faction-4 Republic soldiers are friendly, as the table says.

## Gate

`--faction-test --faction-script <ncs>` asserts the matrix names/values,
hostility, GIT classification, the `GetReputation`/`AdjustReputation` script
bridge (`0 -> 25` on Player/Hostile_1), a save/parse/reload round-trip of a
`Player/ptat_Tuskan` adjustment, and 21 restored faction names. Evidence:
`docs/evidence/s13/faction.log`.
