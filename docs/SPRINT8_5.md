# Sprint 8.5 - residual inherited-systems closeout

Status: **complete - gate S8.5 met (2026-11-03)**. Sprint 8 closed the tool-reachable loader,
encounter-reset, formation and feat-selection work. Sprint 8.5 closes the remaining inherited
systems from Sprints 5-7 that are not presentation/content systems: container loot, companion
combat AI, merchant store gold and identify pricing, and their persistence. The remaining S9+
roadmap is listed at the end and is deliberately not mixed into this gate.

Baseline: Sprint 8 `23/23` CTest, 117/117 module sweep, 0 warnings.

## Gate S8.5

1. **Container loot** - UTP `ItemList` entries load into each placeable. Using a container opens a
   loot panel; individual Take and Take All move UTI-resolved items into the player inventory and
   remove them from the container. `--loot-test` proves a real `danm13` container.
2. **Companion combat AI** - a non-leader companion with a player combat target closes to melee,
   attacks with its own sheet and d20 rules, logs hits, and can defeat the target. The party gate
   proves the companion hit and the existing formation/leader tests still pass.
3. **Merchant depth** - stores carry a deterministic gold pool derived from stock value, buying
   credits the store and debits the player, selling requires enough store gold, and unidentified
   items can be identified for half their cost. The merchant gate checks all three.
4. **Persistence** - store gold, finite stock, item identification, companions, party leader, feats
   and powers round-trip through the engine save slot.
5. **Regression** - **24/24** CTest targets pass, the 117-module sweep is still 0 failures /
   0 warnings, VM corpus parity is 8,666/8,666 with 0 mismatches, GL/Vulkan parity is 0.03/255,
   Python tools are 143 tests with 7 skips, and the install sentinel has 0 session changes.
6. **Evidence and docs** - report, technical note and logs are committed.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s8.5-01-loot | UTP ItemList, loot state/panel, loot gate | `engine/src/kotor/`, `engine/src/game/`, `engine/src/app/` |
| s8.5-02-party-ai | companion melee assist and gate | `engine/src/game/GameSession.*` |
| s8.5-03-merchant | store gold, identify price, persistence | `engine/src/game/GameSession.*`, `Rules.h` |
| s8.5-04-gate | CTest, 117 sweep, VM/parity/install evidence | `engine/CMakeLists.txt`, `docs/evidence/s8_5/` |
| s8.5-05-report | report and technical note | `docs/SPRINT8_5_REPORT.md`, `docs/recon/46_RESIDUAL_SYSTEMS.md` |

## Explicitly S9+ (not this sprint)

- Original KotOR `SAVEGAME.sav` load/write compatibility.
- Creature animation/skinning, weapon attachment and cutscene camera playback.
- Audio/EAX, Bink movies, lip sync, the full `.gui` screen set, minigames and localization.
- Mods/Override runtime precedence and the P6 all-area content checklist.
- Delayed functor execution. This changes the VM functor/`STORESTATE` semantics and is kept out of
  8.5 so the 8,666-script parity remains stable; it is a dedicated VM sprint item.
- Merchant reputation haggling. The shipped UTM has no store faction/gold field and the GIT store
  record has no faction, so a reputation rule would be invented rather than data-derived; this is
  an engine-rule design item for S9.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

```powershell
.\build\msvc-release\kotor_engine.exe --module danm13 --loot-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --party-test --null --headless
.\build\msvc-release\kotor_engine.exe --module tar_m02ac --merchant-test --null --headless
```
