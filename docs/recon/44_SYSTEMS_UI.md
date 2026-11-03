# Recon 44 - stores, encounters, companions and the systems UI

Status: Sprint 7 evidence note. The target is the user's read-only KotOR 1.0.3.0 install
(`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`). All data is loaded in C++;
Python remains an oracle/verifier.

## 1. Store data

A GIT `StoreList` entry names its template in `ResRef` (not `TemplateResRef`), so the GIT reader
uses `TemplateResRef` first and falls back to `ResRef`. The template is a UTM (`2051`):

- `Tag`, `LocName`, `MarkUp`, `MarkDown`, `BuySellFlag`;
- `ItemList` structs with `InventoryRes` and `Infinite`.

Items are UTI (`2025`) templates with `BaseItem`, `Cost`, `StackSize`, `Identified`, `Tag` and
`LocalizedName`. The engine caches UTI templates by resref.

`tar_m02ac` has **7 stores**. `keblastore` has **41 stock entries** and MarkUp/MarkDown 100/40. The
merchant model keeps a stock entry per row (duplicate resrefs stay distinct), a finite remaining
count, and the store's open/closed state. Prices are:

```
buy  = max(1, item.cost * MarkUp / 100)
sell = max(1, item.cost * MarkDown / 100)
```

Buying requires `BuySellFlag & 1`, enough gold and stock; selling requires `BuySellFlag & 2` and a
matching inventory item. The merchant gate (`docs/evidence/s7/merchant.log`) proves the exact price
math, one finite purchase, one sale, and a save/load round-trip of the remaining count.

## 2. Encounter data

A GIT `Encounter List` entry has `TemplateResRef`, world position, a `Geometry` polygon and a
`SpawnPointList`. Geometry points are relative to the encounter position; spawn-point `X/Y/Z` are
world coordinates with `Orientation`.

The template is a UTE (`2040`) with `Active`, `Difficulty`, `MaxCreatures`, `RecCreatures`,
`ResetTime`, `Respawns`, `SpawnOption` and `CreatureList`. Each creature row has `ResRef` (UTC),
`Appearance`, `CR` and `SingleSpawn`.

`tar_m03aa` has **4 encounter volumes**. At module entry the spawner uses
`count = RecCreatures > 0 ? RecCreatures : MaxCreatures`, caps at `MaxCreatures`, cycles the
creature list and spawn points, and calls `spawnCreature` for each. Spawning:

- parses the UTC, resolves appearance/model/texture through `appearance.2da`;
- builds the same entity mesh path as the static loader;
- collects scripts, HP/AC/attack/damage, and adds a script-world object;
- runs the creature's `ScriptSpawn` and the encounter's `OnEntered`.

Measured: 8 creatures, first `tar03_vulkaptenc` hp 18, hostile, mesh present
(`docs/evidence/s7/encounter.log`). The scene grows from 118 to 126 entities.

## 3. Companions

`addPartyMember(tag)` creates a `CompanionState` with a `CharacterSheet` from the rules table and
the entity's HP/name. The roster and sheets are separate from the PC sheet. `giveXp` awards the PC
the full amount and each companion `amount * 80 / 100`, recomputing level/HP from `exptable`.
`removePartyMember` removes both the tag and the sheet. The party panel lists the roster, HP, XP
and a Select button.

Measured: Carth joins with a 500 HP sheet; +1,000 XP gives the PC 1,000 and Carth 800; the sheet,
XP and roster survive save/load (`docs/evidence/s7/party.log`).

## 4. UI

`GameSession` carries `UiPanel { None, Character, Inventory, Party, Journal, Merchant }`.
`drawSystemsUi` renders:

- Character: name/class/level, XP/HP/gold, attributes, AC/attack, skill and feat lists.
- Inventory: gold, item count, name/resref, cost, stack, equipped marker and Equip.
- Party: companion sheets, HP/XP, Select.
- Journal: plot id to state.
- Merchant: all stores with Open, the active merchant's stock with Buy, player inventory with Sell.

Keyboard toggles `C/I/P/J/M` are handled in the SDL key branch; `E` within 3 m of a Store entity
opens it. The headless `--ui-test` covers every state and the real captures are
`docs/evidence/s7/merchant-vk.png`, `inventory-vk.png`, `party-vk.png`.

## 5. Save schema 3

`kotor.engine.player/3` adds:

- `companion=<tag>|<name>|<class>|<level>|<xp>|<hp>|<maxHp>` for every companion;
- `merchant=<storeResref>|<stockIndex>|<remaining>` for every finite stock row.

Stock is indexed, not keyed by item resref, because a store can list the same item resref more
than once. Loading rebuilds the module and merchants first, then re-applies companions and stock
deltas. Schema 2 remains readable.

## 6. Open items

- Encounter reset/respawn timers are parsed but not simulated; encounters spawn once on module
  entry.
- Store gold, identify prices and reputation haggling are not modeled.
- Companion AI, formation and active-leader switching are not modeled.
- Original SAVEGAME.sav compatibility, cutscene cameras, audio/movies, animation/skinning,
  minigames, the full `.gui` set and the remaining areas are Sprint 8.
