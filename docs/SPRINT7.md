# Sprint 7 - P5 systems parity II: stores, encounters, companions and the systems UI

Status: **complete - gate S7 met (2026-11-03)**. Sprint 6 established the native script runtime,
dialogue conditions, d20 rules and a first party/inventory/journal model. Sprint 7 turns those
models into playable systems: merchant stock and pricing, encounter volumes with spawned creatures,
companion sheets and XP sharing, and the character/inventory/party/journal/store panels. Python
remains a verification oracle only. The exit report is [SPRINT7_REPORT.md](SPRINT7_REPORT.md).

Baseline: Sprint 6 / gate S6 (`docs/SPRINT6_REPORT.md`): 8,666/8,666 default and functional corpus
parity, 17/17 CTest, real `end_m01aa` script hooks, dialogue entry 54, d20 rounds, and engine-slot
save/load.

## Gate S7

1. **Merchant model** - UTM store templates and UTI item templates load from the install. The seven
   real stores in `tar_m02ac` expose their stock (including `keblastore`'s 41 entries), markup and
   markdown percentages, and buy/sell flags. Buying checks gold and stock; selling checks the
   player inventory; both update gold, inventory and stock deterministically. `--merchant-test`
   proves the counts, price math and one buy + one sell.
2. **Encounter model** - GIT `Encounter List` geometry and `SpawnPointList`, plus UTE
   `MaxCreatures`/`RecCreatures`/`ResetTime` and `CreatureList`, load into the scene. `tar_m03aa`
   exposes its four encounter volumes and spawns deterministic creatures from the real UTC
   templates at module entry, with meshes, script hooks and combat stats. `--encounter-test` proves
   the volume count, spawn counts and a hostile target.
3. **Party companions** - companions have real sheets derived from their UTC/entity stats, join and
   leave the roster, can be selected, and receive the documented 80% XP share while the PC gets
   the full award. The party panel exposes the roster. `--party-test` proves all of it on the
   shipped Carth/Trask roster.
4. **Systems UI** - ImGui panels for character, inventory/equipment, party, journal and merchant
   stock extend the playable HUD. Keyboard toggles (C/I/P/J/M) work in the real loop; the panel
   state is asserted headlessly, and a real Vulkan capture shows the merchant, inventory and party
   panels.
5. **Persistence** - the engine save slot round-trips the character, companions, party roster,
   inventory/equipment, journal, gold, merchant stock deltas and script globals/locals. A load
   rebuilds the module and restores the merchant/party state. `--systems-test` covers it.
6. **Regression** - the Sprint 5/6 gates stay green: 17 existing CTest targets plus the new
   merchant/encounter/party/UI targets, 8,666/8,666 VM corpus parity, GL/Vulkan scene parity, and a
   read-only install sentinel.
7. **Evidence and tools** - report, recon note, logs and captures are committed; faults found in our
   own tools while doing the work are fixed and covered by tests.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s7-01-data | UTI/UTM/UTE parsing, store/encounter scene records, spawn helper | `engine/src/kotor/`, `engine/src/game/` |
| s7-02-merchant | merchant stock, pricing, buy/sell, merchant gate | `engine/src/game/`, `engine/src/app/` |
| s7-03-encounter | geometry/spawn points, deterministic spawn, combat/scripts | `engine/src/game/`, `engine/src/kotor/` |
| s7-04-party | companion sheets, roster, selection, XP split, party gate | `engine/src/game/` |
| s7-05-ui | character/inventory/party/journal/merchant panels, toggles, capture | `engine/src/app/` |
| s7-06-save | save schema v3 and merchant/party round-trip | `engine/src/game/` |
| s7-07-gate | CTest, corpus/parity regression, install sentinel | `engine/CMakeLists.txt`, `engine/scripts/` |
| s7-08-report | report, technical note, evidence index | `docs/SPRINT7_REPORT.md`, `docs/recon/44_SYSTEMS_UI.md` |

## Non-goals (Sprint 8)

- Original KotOR SAVEGAME.sav compatibility. Sprint 7 still uses the engine slot.
- Cutscene cameras, movie/Bink playback, audio/EAX, lip sync and the full `.gui` screen set.
- Creature animation/skinning, weapon attachment and dialogue camera framing.
- Minigames (pazaak, swoop, turret), all 117 areas, full localization and mods.
- Full merchant haggling/identify/reputation rules beyond the UTM markup/down and buy/sell flags.

## Shared rules

- **Install:** read-only at `E:\SteamLibrary\steamapps\common\swkotor`; hash sentinels before and
  after the gate. No original save or install write.
- **No Python in the runtime:** UTI/UTM/UTE and all gameplay data load in C++; Python verifies.
- **Evidence:** every claim cites a log/test/capture path. Corpus claims cite the parity JSON.
- **Stop rule:** three identical failed attempts on one item -> document and stop that item.

## Running the sprint gates

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

Individual proofs:

```powershell
.\build\msvc-release\kotor_engine.exe --module tar_m02ac --merchant-test --null --headless
.\build\msvc-release\kotor_engine.exe --module tar_m03aa --encounter-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --party-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --ui-test --null --headless
.\build\msvc-release\kotor_engine.exe --module tar_m02ac --panel merchant --vulkan --frames 3 `
  --screenshot artifacts\engine\s7-merchant.png
```
