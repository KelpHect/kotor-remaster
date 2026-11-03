# Sprint 8.5 report - residual inherited-systems closeout

Status: **complete - gate S8.5 met (2026-11-03)**. Sprint 8.5 closes the remaining S5/S6/S7 engine
systems that are not presentation/content work: container loot, companion combat AI, merchant store
gold and identify pricing, and their persistence. The explicit S9+ roadmap is listed at the end.

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- Build: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- Compiler: MSVC 19.51.36231 (C++23), CMake preset `msvc-release`

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 24
Total Test time (real) =  43.31 sec
```

The 23 Sprint 8 targets still pass; `kotor_loot` was added. Python suite remains **143 tests,
0 failures, 7 skips**. See `docs/evidence/s8_5/ctest.log`.

## 2. Container loot

UTP `ItemList` entries now load into the placeable. `usePlaceable`/`E` opens a Loot panel with
Take and Take All. Items resolve through UTI for name/base item/cost/identification; taking moves
them into the player inventory and removes them from the container. Containers with items are
usable even when the template's `Useable` flag is 0.

`danm13` gate:

```text
loot test: container FootLker items=1
loot test: container_used=1
loot test: loot_opened=1
loot test: loot_taken=1
loot test: inventory_gained=1
loot test: PASS
```

`docs/evidence/s8_5/loot.log`.

## 3. Companion combat AI

Non-leader companions now use the player's combat target: they close to melee, attack with their
own sheet and d20 rules, log hits, and can defeat the target. Formation and leader switching from
Sprint 8 remain.

```text
party test: follower distance 8 -> 5.49538
party test: formation_follow=1
party test: companion_attacks=1
party test: leader_selected=1
party test: leader_roundtrip=1
party test: PASS
```

`docs/evidence/s8_5/party.log`.

## 4. Merchant depth

Stores now carry a deterministic gold pool derived from stock value. Buying credits the store and
debits the player; selling requires the store to have enough gold; unidentified items can be
identified for half their cost. Store gold, finite stock and identification persist.

```text
merchant test: store_gold_buy=1
merchant test: identify_item=1
merchant test: identified=1
merchant test: identify_gold=1
merchant test: merchant_gold_roundtrip=1
merchant test: identified_roundtrip=1
merchant test: PASS
```

`docs/evidence/s8_5/merchant.log`.

## 5. Persistence

Save schema now persists store gold and per-item identification in addition to Sprints 6-8 state:
character, companions, party leader, inventory, journal, script globals/locals, feats/powers and
merchant stock deltas. The party and merchant gates exercise the new fields.

## 6. Regression evidence

- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounter volumes.
- VM corpus: **8,666/8,666** default and functional, 0 mismatches, 303,616 ACTION calls.
- GL/Vulkan parity: **0.03/255**.
- Python suite: **143 tests, 7 skips**.
- CTest: **24/24**.
- Install: 15,380 files, 0 session changes.

## 7. Explicit S9+ roadmap

These are not tool-blocked residuals; they are new engine/content systems and are deliberately
kept out of the 8.5 gate:

- Original KotOR `SAVEGAME.sav` load/write compatibility.
- Creature animation/skinning, weapon attachment, cutscene camera playback.
- Audio/EAX, Bink movies, lip sync, the full `.gui` screen set, minigames and localization.
- Mods/Override runtime precedence and the P6 all-area content checklist.
- Delayed functor execution: changes the VM `STORESTATE`/functor contract; a dedicated VM sprint
  keeps the 8,666-script parity stable.
- Merchant reputation haggling: the shipped UTM/GIT store records carry no faction or reputation
  fields, so this needs an explicit engine rule design rather than a data-derived implementation.

## Evidence index

- `docs/evidence/s8_5/ctest.log`
- `docs/evidence/s8_5/loot.log`
- `docs/evidence/s8_5/party.log`
- `docs/evidence/s8_5/merchant.log`
- `docs/evidence/s8_5/encounter-reset.log`
- `docs/evidence/s8_5/rules.log`, `systems.log`, `ui.log`
- `docs/evidence/s8_5/all-modules.log`, `all-modules.json`
- `docs/evidence/s8_5/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s8_5/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s8_5/tools-tests.log`
- `docs/evidence/s8_5/install-sentinel.log`
- Technical note: [docs/recon/46_RESIDUAL_SYSTEMS.md](recon/46_RESIDUAL_SYSTEMS.md)
