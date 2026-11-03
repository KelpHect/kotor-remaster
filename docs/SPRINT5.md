# Sprint 5 - playable KotOR P4 vertical slice

Status: **complete - gate S5 met (2026-11-03)**. The engine's own C++23 runtime loads a user-owned
KotOR 1.0.3.0 install directly, renders `end_m01aa` on Vulkan 1.4 and OpenGL 4.5, and runs a
deterministic 30-minute scripted slice through dialogue, a trigger, combat, a door transition and
save/load. The evidence and exact commands are in [SPRINT5_REPORT.md](SPRINT5_REPORT.md).

Goal: put a real KotOR module into the engine as a playable P4 vertical slice, keep Python out of
the product path, and fix faults found in our own tools while doing it.

## Gate S5

1. **Direct C++ data path** - KEY/BIF/RIM/ERF, GFF V3.2, LYT/WOK/MDL/MDX, TPC/TGA, 2DA, TLK,
   DLG, and UTC/UTD/UTP/UTT templates load from the user's read-only install. No Python and no
   pre-baked scene package are used by the runtime.
2. **Playable area** - `end_m01aa` loads 15 rooms, 235,038 vertices/indices, 53 textures, 1,365
   collision facets, 218 GIT entities, 101 entity meshes, and 13 trigger volumes with zero model
   warnings.
3. **Player and interactions** - movement, mouse look, collision, doors, module transitions,
   placeables (basic use/open), NPCs, dialogue, basic combat, triggers, save/load, and a minimal
   HUD/dialogue UI.
4. **Rendering** - the Classic scene renders on Vulkan 1.4 and OpenGL 4.5. Both backends draw the
   same room, doors, placeables, and creatures with the same camera; the scene-region mean absolute
   difference is 0.03/255.
5. **G4 script** - a deterministic 1800 s playthrough reproduces the expected events: real Trask
   dialogue, trigger `end_trig02`, a Sith apprentice fight to death, the
   `end_m01aa -> end_m01ab` door transition, and a save/load round-trip.
6. **Placement and map validation** - landmark positions and headings match the verified GIT
   oracle; the retail map validator reports 0 hard failures on `end_m01aa`, `end_m01ab`,
   `tar_m02af`, and `tar_m02aa`.
7. **Tests and evidence** - 13/13 CTest targets pass; commands, logs, screenshots, and the parity
   report are recorded in [SPRINT5_REPORT.md](SPRINT5_REPORT.md).

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s5-01-data | C++ archive/GFF/2DA/TLK/DLG/template/MDL loaders | `engine/src/kotor/` |
| s5-02-render | GL 4.5 and Vulkan 1.4 Classic scene pass, textures, screenshots | `engine/src/render/` |
| s5-03-gameplay | player, collision, doors, triggers, dialogue, combat, save/load, UI | `engine/src/game/`, `engine/src/app/` |
| s5-04-gate | 1800 s scripted gate, landmark/parity checks, CTest | `engine/tests/`, `engine/scripts/check_parity.py` |
| s5-05-report | exit report, technical note, evidence index | `docs/SPRINT5_REPORT.md`, `docs/recon/42_PLAYABLE_SLICE.md` |

## Non-goals (P5/P6)

- Full P5 systems: feats/Force powers, party, inventory, merchants, XP/levels, journal, cutscene
  cameras, encounters, minigames, audio, movies, and every `.gui` screen.
- Original KotOR save compatibility. Sprint 5 uses an engine-local player snapshot.
- Creature animation/skinning. Entity models are rendered in their static bind pose.
- NCS script execution and full dialogue condition evaluation. Dialogue scripts/quests are
  recorded as events; starting entries are selected by the first spoken line.
- All 117 areas. Sprint 5 validates the Endar Spire pair and the Taris apartment pair.

## Running the slice

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

OpenGL capture:

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --gl --width 960 --height 540 `
  --frames 3 --screenshot artifacts\engine\s5-gl.bmp
```

Vulkan capture:

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --vulkan --width 960 --height 540 `
  --frames 3 --screenshot artifacts\engine\s5-vulkan.bmp
```

Parity check (verification only, Pillow required):

```powershell
python engine\scripts\check_parity.py
```

Scripted gate:

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --playthrough --null --headless
```
