# Sprint 7 report - P5 systems parity II: stores, encounters, companions and the systems UI

Status: **complete - gate S7 met (2026-11-03)**. The engine now loads the shipped merchant and
encounter templates, prices and trades real stock, spawns deterministic encounter creatures with
meshes and scripts, gives companions real sheets and the 80% XP share, and exposes
character/inventory/party/journal/merchant panels with real captures. All numbers below are from
the final run; evidence is in [docs/evidence/s7](../evidence/s7/).

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- Build: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- Compiler: MSVC 19.51.36231 (C++23), CMake preset `msvc-release`
- GPU: AMD Radeon RX 9070 XT
- Build command: `cmd /c "engine\scripts\build.bat msvc-release"`
- Test command: `ctest --preset msvc-release --output-on-failure`

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 21
Total Test time (real) =  10.62 sec
```

The 17 Sprint 5/6 targets still pass; four targets were added: `kotor_merchant`,
`kotor_encounter`, `kotor_party` and `kotor_ui`. The VM corpus target still reports 0 mismatches.
See `docs/evidence/s7/ctest.log`.

Python research suite: **143 tests, 0 failures, 7 skips** (`docs/evidence/s7/tools-tests.log`).

## 2. Stores and merchants

`tar_m02ac` loads **7 stores**. `keblastore` exposes **41 stock entries**, markup 100, markdown 40
and buy/sell flag 3. UTI item templates provide base item, cost and localized name; UTM provides
the stock list and pricing.

```text
merchant test: seven_stores=1
merchant test: store keblastore stock=41 markup=100 markdown=40 flag=3
merchant test: stock_41=1
merchant test: buy_and_sell=1
merchant test: stock g_w_vbroshort01 cost=100 buy=100 remaining=1
merchant test: buy_price_math=1
merchant test: buy_item=1
merchant test: buy_gold=1
merchant test: buy_inventory=1
merchant test: buy_stock_decrement=1
merchant test: stock_roundtrip=1
merchant test: gold_roundtrip=1
merchant test: inventory_roundtrip=1
merchant test: sell_price_math=1
merchant test: sell_item=1
merchant test: sell_gold=1
merchant test: sell_inventory=1
merchant test: PASS
```

Buying debits `cost * MarkUp / 100` and decrements finite stock; selling credits
`cost * MarkDown / 100` and removes the item. The merchant panel lists all seven stores, opens a
stock list with prices and Buy buttons, and lists player inventory with Sell buttons. Capture:
`docs/evidence/s7/merchant-vk.png`.

## 3. Encounters

`tar_m03aa` loads **4 encounter volumes** and spawns **8 creatures** from the real UTC templates at
the GIT spawn points. Spawned creatures have meshes, `ScriptSpawn` hooks and hostile/combat state.

```text
encounter test: volumes=4 states=4
encounter test: volume_count=1
encounter test: spawned=8
encounter test: creatures_spawned=1
encounter test: first tar03_vulkaptenc hp=18 hostile=1 mesh=1
encounter test: spawned_hostile=1
encounter test: spawned_mesh=1
encounter test: spawned_hp=1
encounter test: PASS
```

The scene stats before spawn show 118 entities / 4 encounter entities; after spawn 126 entities /
27 creatures, with 344 hooks on 46 entities (`docs/evidence/s7/scene-m03aa.log`). UTE
`MaxCreatures`, `RecCreatures`, `ResetTime`, `Respawns`, `SpawnOption` and `CreatureList`
(`ResRef`, `Appearance`, `CR`, `SingleSpawn`) load into each volume.

## 4. Party companions

Adding `Carth` creates a real companion sheet from his entity/UTC stats. A 1,000 XP award gives
the PC the full 1,000 and the companion the documented 80% share (800). The companion survives
save/load, can be selected and leaves cleanly.

```text
party test: join=1
party test: companion_sheet=1
party test: Carth name=Carth hp=500/500 level=1
party test: companion_name=1
party test: companion_hp=1
party test: pc_full_xp=1
party test: companion xp=800
party test: companion_80_percent=1
party test: selected=1
party test: save_written=1
party test: save_loaded=1
party test: companion_roundtrip=1
party test: party_roundtrip=1
party test: companion_xp_roundtrip=1
party test: leave=1
party test: companion_removed=1
party test: PASS
```

## 5. Systems UI

The new `Systems` window has character, inventory, party, journal and merchant panels. In the real
loop, `C`, `I`, `P`, `J` and `M` toggle them; `E` near a store opens that merchant. The headless
state gate passes every panel and toggle:

```text
ui test: starts_closed=1
ui test: character_open=1
ui test: inventory_open=1
ui test: toggle_closes=1
ui test: party_name=1
ui test: journal_name=1
ui test: merchant_name=1
ui test: none_name=1
ui test: PASS
```

Real Vulkan captures:

- `docs/evidence/s7/merchant-vk.png` - `keblastore`, 41 stock entries, prices and Buy buttons
- `docs/evidence/s7/inventory-vk.png` - 19 items, costs, stacks and Equip buttons
- `docs/evidence/s7/party-vk.png` - Carth companion sheet, HP and Select button

## 6. Persistence and regressions

Save schema `kotor.engine.player/3` adds companion sheets and merchant stock deltas. The merchant
and party tests round-trip gold, inventory, finite stock and companion XP. The Sprint 6 systems
test still round-trips character, journal, party, inventory, globals/locals and module.

```text
systems test: xp_roundtrip=1
systems test: level_roundtrip=1
systems test: inventory_roundtrip=1
systems test: party_roundtrip=1
systems test: module_roundtrip=1
systems test: local_roundtrip=1
systems test: PASS
```

VM corpus parity is unchanged: **8,666/8,666** default and functional, 0 mismatches, 303,616
ACTION calls (`docs/evidence/s7/vm-corpus.log`). GL/Vulkan scene parity is still
**0.03/255** (`docs/evidence/s7/parity.log`).

The install sentinel was taken before the final gate and compared after the 21-test run and the
Python suite:

```text
{"files": 15380, "manifest_drift": 2, "pass": true,
 "session": {"changed": [], "changed_count": 0, "extra": [], "extra_count": 0,
             "missing": [], "missing_count": 0, "unchanged": true}}
```

The two manifest drifts are the same pre-sprint `swkotor.ini` / `steam_autocloud.vdf` writes from
the earlier retail runs. Sprint 7 changed nothing in the install.

## 7. Faults fixed

1. **Merchant stock round-trip by resref** - duplicate item resrefs in a store (for example two
   `g_w_vbroshort01` rows) made the loader apply the last duplicate's count to every row. Stock
   deltas are now keyed and restored by stock index.
2. **Store template resref** - GIT `StoreList` uses `ResRef`, not `TemplateResRef`; stores loaded
   with an empty template and no stock. The GIT reader now falls back to `ResRef`.
3. **Store/encounter hooks** - `OnOpenStore`, `OnEntered` and `OnExhausted` were not in the hook
   label list; they are now collected and `OnEntered` runs after an encounter spawns.
4. **Encounter hostility** - spawn templates use a faction that is not the Endar Spire Sith
   faction; spawned encounter creatures are explicitly hostile so the encounter is a combat
   system.
5. **Zero/duplicate merchant stock persistence** - addressed by the indexed delta above; the
   merchant test now saves after a finite purchase and verifies the exact remaining count.
6. **`check_parity.py` relative `--out`** was already fixed in Sprint 6; Sprint 7 re-ran it at
   0.03/255.

## 8. Limitations and Sprint 8

Sprint 8 closes the encounter reset/respawn timers, companion formation/leader switching, feat and
Force-power selection, and the full 117-module loader sweep; see `docs/SPRINT8_REPORT.md`. The
following are the remaining future systems.

- Original KotOR SAVEGAME.sav compatibility is still open; the engine slot is local.
- Encounter reset/respawn timers are parsed but not simulated; encounters spawn once at module
  entry.
- Merchant identify/reputation haggling and store gold limits are not modeled; UTM markup/down and
  buy/sell flags are.
- Cutscene cameras, movie/audio, animation/skinning, minigames, the full `.gui` set and all 117
  areas remain Sprint 8.
- Companion AI/formation and party selection of the active leader are not modeled; companion
  sheets, XP share, roster and selection are.

## Evidence index

Committed under [docs/evidence/s7](../evidence/s7/):

- `ctest.log` - 21/21 final CTest run
- `merchant.log`, `encounter.log`, `party.log`, `ui.log`, `systems.log`
- `scene-m02ac.log`, `scene-m03aa.log` - store/encounter pre-spawn counts
- `vm-corpus.log`, `vm-parity.json` - 8,666/8,666, 0 mismatches
- `parity.log`, `parity-gl.png`, `parity-vk.png` - 0.03/255 scene parity
- `merchant-vk.png`, `inventory-vk.png`, `party-vk.png` - real panel captures
- `tools-tests.log` - 143 Python tests, 7 skips
- `install-sentinel.log` - 15,380 files unchanged
- Technique note: [docs/recon/44_SYSTEMS_UI.md](recon/44_SYSTEMS_UI.md)
