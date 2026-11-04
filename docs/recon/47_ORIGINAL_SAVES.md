# Recon 47 - original KotOR save read/write

Status: Sprint 9 evidence note. Target install: `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.
All save samples used here are read-only fixtures from
`artifacts/lanes/06-dynamic/profile/saves/`.

## 1. Container layout

A save slot is a folder named `NNNNNN - NAME`:

```text
SAVEGAME.sav      ERF tagged MOD V1.0
PARTYTABLE.res    GFF V3.2, file type "PT  "
GLOBALVARS.res    GFF V3.2, file type "GVT "
savenfo.res       GFF V3.2, file type "NFO "
Screen.tga        256x256 thumbnail (optional for autosaves)
pifo.ifo          autosave-only player info
```

`SAVEGAME.sav` holds:

- `INVENTORY` (GFF, often stored with the retail type-0 quirk),
- `REPUTE` (FAC),
- `AVAILNPC*` (UTC),
- one nested `MOD V1.0` archive per visited module.

Each nested archive holds the area `GIT` (2023), area `ARE` (2012) and `Module`
`IFO` (2014). A real Taris Lower City save contains seven nested module
archives (`tar_m02aa` through `tar_m03aa`); the `savenfo.res` `LASTMODULE`
selects the current one.

## 2. Native reader

`engine/src/kotor/SaveGame.cpp` parses the outer ERF from bytes, selects the
nested module matching `savenfo.res` `LASTMODULE`, and then maps:

- **IFO** - `Mod_Entry_Area`, `Mod_Entry_X/Y/Z`, `Mod_Entry_Dir_X/Y`;
- **PC** - `Mod_PlayerList[0]`: name locstrings, STR/DEX/CON/INT/WIS/CHA,
  current/max HP, XP, class and level from `ClassList`, feats from `FeatList`;
- **inventory** - outer `INVENTORY.ItemList` plus the PC `Equip_ItemList`;
  resref falls back from `InventoryRes`/`InventoryResRef`/`TemplateResref` to
  `Tag`, and name comes from `LocalizedName`;
- **party** - `PT_MEMBERS` member ids mapped to the shipped companion order
  (`0 PC, 1 Carth, 2 Bastila, ... 9 T3-M4`); member id 0 is implicit and is not
  added as a companion;
- **journal** - `JNL_Entries` plot/state pairs;
- **globals** - `CatNumber`/`ValNumber`, `CatBoolean`/`ValBoolean` (MSB-first),
  `CatString`/`ValString`, and `CatLocation`/`ValLocation` (six floats per
  entry); location globals stay in `GameSession::locationGlobals_` so they round
  trip without entering the NCS integer/string global map;
- **objects** - `Creature List`, `Door List`, `Placeable List`, `TriggerList`,
  `StoreList`, `Encounter List`, `WaypointList`, `SoundList`: tag, position,
  bearing, current/max HP, dead/open/locked flags and placeable `ItemList`.

`GameSession::loadOriginalSave` loads the module from the install, applies the
player/party/inventory/globals/journal, overlays saved object state by tag, then
re-synchronizes the script world.

Measured loads:

| Save | Module | Gold | Inventory | Journal | Runtime globals | Locations | Companions |
|---|---|---:|---:|---:|---:|---:|---:|
| `000000 - QUICKSAVE` | `tar_m02af` | 51 | 15 | 4 | 1,180 | 5 | 0 |
| `000006 - TARISLC` | `tar_m03aa` | 731 | 16 | 9 | 1,180 | 5 | 1 |

## 3. Native writer

`engine/src/kotor/GffWrite.cpp` provides a mutable GFF V3.2 tree, a canonical
writer (struct/field/label/fdata/fidx/lidx, 4-byte fdata alignment) and an
ERF/MOD writer (160-byte header, 24-byte keys, 8-byte resource rows, packed
payloads). `engine/src/kotor/SaveGame.cpp` builds:

- **IFO** - copies the install module IFO, sets `Mod_IsSaveGame=1`,
  `Mod_IsNWMFile=0`, the current entry point and a fresh `Mod_PlayerList`;
- **ARE/GIT** - copies the install templates. Each GIT instance is flattened by
  merging missing fields from its UTC/UTD/UTP/UTT/UTM/UTE template, then the
  live position, bearing, HP, dead/open/locked and placeable loot are written;
  `UseTemplates` is removed so the saved GIT is read as flattened state;
- **PT** - gold, XP pool, played seconds, members and leaders, the 9 available
  NPC flags, empty Pazaak/message lists and the journal;
- **GVT** - the four category name lists and the four value blobs, including
  location values;
- **NFO** - area name, last module, played time, save name, hints and portrait;
- **INVENTORY** - unequipped items; equipped items are written into the PC
  `Equip_ItemList`;
- **REPUTE** - the FAC payload is parsed into the faction matrix and rebuilt on
  save (21 factions / 405 non-100 relations for the shipped table, including
  player relations and `FactionParentID`). A fresh save writes the full matrix
  instead of an empty scaffold; adjustments made in play survive reload.
- **visited modules** - the current module is rebuilt from live state; the other
  nested `<module>.sav` ERFs and `AVAILNPC*`/`PC` outer resources are preserved
  verbatim so revisiting a previously saved module keeps its retail state.

## 4. Validation

`tools/resources/kotor_save.py` is the independent Python reader/validator and
is not imported by the engine.

```text
docs/evidence/s9/validate-roundtrip.log   validation: PASS (no issues)
docs/evidence/s9/validate-create.log      validation: PASS (no issues)
docs/evidence/s9/validate-party.log       validation: PASS (0 errors, 3 inherited
                                          Mod_Tag warnings from preserved visited modules)
```

The multi-module Taris Lower City source has four `Mod_Tag` warnings. The writer
preserves the six non-current module ERFs verbatim, so three of the four remain;
the current module and all sidecars validate cleanly.

The round-trip slot preserves the source module, area, area name, PC name,
gold, inventory count (12 carried + 3 equipped), journal count, 1,185 global
entries (369 number / 809 boolean / 2 string / 5 location) and party. The fresh
`end_m01aa` slot opens from scratch with one inventory item, one journal entry,
one number global and no warnings.

Source hashes are identical before and after the gate
(`docs/evidence/s9/source-before.json` / `source-after.json`,
`SOURCE_UNCHANGED=1`).

## 5. Residual work

- Faction reputation (`REPUTE`) is simulated from `repute.2da` and the FAC
  payload (`GetReputation`/`AdjustReputation`, save round-trip). Remaining
  residual: custom faction parenting is stored but not inherited for relation
  fallback.
- The writer updates the current module from live state and preserves the other
  visited modules as their original saved ERFs; it does not re-simulate those
  modules or update their state.
- Saved effects, action queues, combat round data, per-object locals and
  non-GIT graph state are not restored; the GIT list state above is.
- Retail-engine acceptance of a slot written by this C++ writer was not re-run
  in Sprint 9. The format is independently validated by `kotor_save.py` (0/0)
  and the prior retail acceptance of the same container/sidecar layout is
  recorded in `docs/recon/21_SAVE_BYTEPARITY.md`.
