# Sprint 5 report - playable KotOR P4 vertical slice

Status: **complete - gate S5 met (2026-11-03)**. This report records the commands, outputs, and
artifacts for the C++ runtime slice. The slice is intentionally scoped to the P4 gate; P5/P6
systems and full-game coverage remain open and are listed under *Limitations*.

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- Build: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- Compiler: MSVC 19.51.36231 (C++23), CMake presets `msvc-release`
- GPU: AMD Radeon RX 9070 XT
- Vulkan: instance 1.4.0, device 1.4.359, `VK_FORMAT_B8G8R8A8_UNORM`
- OpenGL: 4.5.0 Core Profile, `26.10.1.261008`
- Shaders: `engine/shaders/scene.vert` / `scene.frag`, compiled to SPIR-V with Khronos
  `glslang` 16.6.0 at `C:\dev-overflow\glslang\bin\glslang.exe`

Build and tests:

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

Result: **13/13 tests passed, 0 failed** (1.91 s on the final run; timing varies). Captured in
`engine/artifacts/engine/s5-evidence/ctest.log`.

## 1. Module load and scene inventory

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --scene-stats --null --headless
```

```text
KotOR module end_m01aa area m01aa from E:/SteamLibrary/steamapps/common/swkotor
  build sha256: 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6
  rooms: 15 vertices: 235038 indices: 235038 batches: 34
  textures: 53 collision vertices: 850 facets: 1365
  entities: 218 (door 15, placeable 60, creature 26, waypoint 58)
  entry: 15.4248, 20.1215, -1.275
  entity meshes: 101 triggers: 13 warnings: 0
  entity mesh 0 end_door05 verts=564 local=-3.07263,-0.121998,-0.047403 .. 3.08736,0.123205,3.00955
  entity mesh 1 end_door06 verts=606 local=-3.08736,-0.141861,-1.04532 .. 3.07263,0.121999,3.07527
  entity mesh 2 end_door06 verts=606 local=-3.08736,-0.141861,-1.04532 .. 3.07263,0.121999,3.07527
  creature mesh Carth verts=3276 local=-0.606384,-0.151653,-0.00207151 .. 0.610334,0.167699,1.54884
  bounds: -170.51, -32.6442, -204.652 .. 238.793, 471.187, 291.201
```

The loader reads the archive, GFF, LYT/WOK/MDL/MDX, TPC/TGA, 2DA, TLK, DLG and template data
directly in C++. Entity models, template stats, trigger polygons, and textures are resolved from
the install; no Python viewer or cached scene package is used.

## 2. Rendering and backend parity

Captures:

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --gl --width 960 --height 540 `
  --frames 3 --screenshot artifacts\engine\parity-gl.bmp
.\build\msvc-release\kotor_engine.exe --module end_m01aa --vulkan --width 960 --height 540 `
  --frames 3 --screenshot artifacts\engine\parity-vulkan.bmp
python engine\scripts\check_parity.py
```

The parity checker (verification only; not part of the runtime) compares the scene region and
excludes the HUD because its FPS text changes between runs.

```text
scene_region=(499, 118, 940, 520)
mean_abs_channel_diff=0.0300
rms_channel_diff=1.8755
threshold=1.0000
verdict=PASS
```

Artefacts: `engine/artifacts/engine/parity/parity-gl.png`,
`engine/artifacts/engine/parity/parity-vk.png`, `parity-diff.png`,
`engine/artifacts/engine/parity/parity-report.txt`.

The Vulkan path flips the projection Y row before the view multiply, prefers a UNORM swapchain for
Classic parity, and converts BGRA readback to RGBA for the BMP writer. This makes the two backends
produce the same frame rather than the earlier double-sRGB / vertically-inverted result.

## 3. Dialogue

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --dialogue-test --null --headless
```

The C++ runtime finds `end_trask`, reads its UTC `Conversation = end_trask01`, parses the real
71,743-byte DLG, resolves strrefs through `dialog.tlk`, and follows one branch:

```text
dialogue test: end_trask conversation end_trask01
  Trask: You can move the mouse to the edges of the screen to rotate the camera. ...
  Trask: To move towards the footlocker, hold down the right mouse button and face the camera ...
  Trask: You can left-click on an object to select it. ...
    1. Okay.
    2. Can you repeat that?
dialogue test: lines=3
```

The in-game UI shows the speaker, the entry text, and numbered reply buttons; keys `1`-`9` choose a
reply and `Esc` closes the conversation. The DLG parser also reads `RepliesList`, `EntriesList`,
`ActionParam1..4`, `Script`, and `Quest` fields.

## 4. Basic combat

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --combat-test --null --headless
```

The runtime loads the hostile UTC (`Appearance_Type`, `FactionID = 4`, HP, AC, Dex), places the
player in range, resolves d20 attack rolls, applies damage, and marks the creature dead:

```text
combat test: end_darkjedi01 dead=1 hp=-3/8 player=94/100 steps=110
```

The target and player HP are shown in the HUD, and the recent events list records rolls, hits,
and the kill. Dead entities are skipped by both renderers.

## 5. Triggers, placeables, doors, and save/load

- `end_trig02` has a four-point GIT polygon. `updateTriggers()` tests the player's XY position
  against the triangulated polygon and fires the enter event once.
- `end_locker01` has UTP `Useable = 1`; `usePlaceable()` records `use end_locker01` when the player
  is within 2.5 m. The gate asserts the event.
- `testTransition` selects a linked door, copies `LinkedToModule`/`LinkedTo` before reloading (the
  earlier reference into the replaced scene was a use-after-free), and loads the target module.
- Save/load writes and restores the module, position, yaw, pitch, and player HP.

The combined playthrough output includes:

```text
playthrough: trigger end_trig02 fired=1
playthrough: placeable end_locker01 used=1 logged=1
playthrough: combat target end_darkjedi01 dead=1 playerHp=94/100 steps=110
playthrough: transition end_m01aa -> end_m01ab ok=1
playthrough: save/load module=end_m01ab same=1
playthrough: simulated 1800.03 s, events=18
playthrough: PASS
```

## 6. Deterministic 30-minute gate

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --playthrough --null --headless
```

The gate runs a fixed set of events and then advances simulated time to 1800 s:

- walks or script-spawns to Trask, opens the real dialogue, follows a branch;
- enters the `end_trig02` trigger;
- uses the `end_locker01` footlocker through the placeable interaction path;
- fights the first Sith apprentice to death;
- takes the first linked door to `end_m01ab`;
- saves and reloads the player snapshot in the new module;
- advances to 1800.03 s of simulated play.

Result: **`playthrough: PASS`**. The "30-minute" requirement is measured in deterministic simulated
time so the gate is fast and repeatable; it is not a wall-clock 30-minute capture.

## 7. Landmark and heading gate

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --landmark-test --null --headless
```

```text
landmark end_trask pos=(22.8837, 20.6108, -1.275) bearing=-2.74887 pos_ok=1 bearing_ok=1
landmark end_door05 pos=(46.5749, 50.8134, -1.31576) bearing=-0 pos_ok=1 bearing_ok=1
landmark end_locker01 pos=(10.7959, 21.8703, -1.105) bearing=3.14145 pos_ok=1 bearing_ok=1
landmark end_door10 link=end_m01ab/from_01a ok=1
landmark entry pos=(15.4248, 20.1215, -1.275) ok=1
landmark test: PASS
```

These values are the verified GIT/IFO values (RH Z-up metres, radian headings). The test checks
position tolerance 0.01 m and angular tolerance 0.001 rad.

## 8. Map validation

```powershell
python tools/mapcompile/compile.py --game-dir E:/SteamLibrary/steamapps/common/swkotor `
  --module end_m01aa --module end_m01ab --module tar_m02af --module tar_m02aa `
  --out artifacts/lanes/s5-mapvalidate --profile retail
```

```text
[  1/4] end_m01aa      area=m01aa      triaged      hard=0 triage=33 codes=T_ANCHOR_OFF_WALKMESH,T_DOOR_NO_HOOK,T_ROOM_NO_WOK
[  2/4] end_m01ab      area=m01ab      triaged      hard=0 triage=9  codes=T_ANCHOR_OFF_WALKMESH,T_DOOR_NO_HOOK
[  3/4] tar_m02af      area=m02af      triaged      hard=0 triage=2  codes=T_DOOR_NO_HOOK,T_ROOM_NO_WOK
[  4/4] tar_m02aa      area=m02aa      triaged      hard=0 triage=10 codes=T_ANCHOR_OFF_WALKMESH,T_DOOR_NO_HOOK,T_ROOM_NO_WOK
clean=0 triaged=4 hard_failed=0 of 4 modules in 3.97s
```

The retail profile applies the documented shipped-data triage classes (static doors, door-linked
PTH components, multi-level walkmeshes, lightmap wrap UVs, placeholder cameras). There are zero
unexplained hard failures.

## 9. Tests

`ctest --preset msvc-release --output-on-failure`:

| Test | Result |
|---|---|
| `kotor_formats` | passed |
| `smoke_null_headless` | passed |
| `smoke_imgui_selftest` | passed |
| `kotor_scene_stats` | passed |
| `kotor_scene_assert` | passed |
| `kotor_door_transition` | passed |
| `kotor_taris_transition` | passed |
| `kotor_walk_collision` | passed |
| `kotor_dialogue` | passed |
| `kotor_combat` | passed |
| `kotor_playthrough` | passed |
| `kotor_landmark` | passed |
| `kotor_save_load` | passed |

`kotor_formats` includes synthetic 2DA V2.b and TLK V3.0 cases in addition to the SHA-256,
resref-normalization, LYT, and WOK cases (16 checks total).

## 10. Faults fixed in our own tools

1. **GFF magic** - KotOR files use `ARE `/`GIT `/`IFO ` + `V3.2`, not the generic `GFF ` magic.
   The stricter check rejected every module; the parser now validates the version at offset 4.
2. **GL scene upload** - the VAO was created before the scene buffers and the upload path bailed
   out with "scene renderer is not initialized"; the VAO is now created in `upload()`.
3. **Vulkan depth** - the depth image was only created on resize, so the first frame had no depth
   attachment; the backend now prepares it at initialization.
4. **Screenshot orientation/format** - unified `Bmp.cpp`; GL and Vulkan captures are both written
   top-down; Vulkan readback converts BGRA to RGBA and flips projection Y before the view multiply.
5. **Stale shaders** - CMake copied `shaders/` only at configure time, so a rebuilt executable
   loaded an old push-constant layout; the copy is now a `POST_BUILD` step that clears the
   destination first.
6. **Door transition use-after-free** - `testTransition` passed `entity.linkedTo` by reference into
   `loadModule`, which replaced the scene and destroyed the string mid-call; both strings are now
   copied first.
7. **WOK unit fixture** - the synthetic test wrote its count table over the vertex data, so the
   parser read zero counts and indexed an empty face list; the fixture now starts data at 96.
8. **ARE colors** - ambient/diffuse/fog are packed `UInt32` BGR values, not `Vector3`; zero sun
   colors fall back to the Classic defaults.
9. **TLK locstrings** - DLG locstrings with only a strref (no inline substring) were shown as
   empty; the parser now resolves the strref through `dialog.tlk` first.
10. **Vulkan/GL parity** - the Vulkan path initially used an sRGB swapchain (double-encoding the
    already-sRGB texels), a Y-down viewport with a GL projection, and BGRA readback into an RGBA
    BMP. The UNORM swapchain, projection-Y flip, and channel conversion bring the scene diff to
    0.03/255.
11. **Placeable use** - UTP `Useable` was not read; the loader now maps it and the interaction path
    records `use <tag>` (the Endar Spire footlocker is the gate case).

## 11. Limitations and open work

- Entity models are static bind pose; no animation, skinning, or weapons.
- Dialogue scripts/quests are recorded as events; no NCS execution or full condition evaluation.
- Combat is a basic d20 attack/damage loop; no feats, Force powers, rounds, or death animation.
- Save format is engine-local (`kotor.engine.player/1`), not original KotOR save compatibility.
- GUI is the HUD, dialogue panel, and combat log; not the full `.gui` screen set.
- Placeables support a basic `Useable` event only; there is no inventory/loot UI.
- The 1800 s gate is deterministic simulated time, not a wall-clock session.
- Four modules are validated (`end_m01aa`, `end_m01ab`, `tar_m02af`, `tar_m02aa`), not all 117.
- `end_m01aa`'s first chosen dialogue branch is the controls tutorial because full `Active`
  condition scripts are P5 work.

## Evidence index

Committed copies of the evidence below are under `docs/evidence/s5/`; the working paths are the
same files. The scene captures and logs are the final run used in this report.

- Scene stats: `engine/artifacts/engine/s5-evidence/scene-stats.log`
- Dialogue: `engine/artifacts/engine/s5-evidence/dialogue.log`
- Combat: `engine/artifacts/engine/s5-evidence/combat.log`
- Playthrough: `engine/artifacts/engine/s5-evidence/playthrough.log`
- Landmark: `engine/artifacts/engine/s5-evidence/landmark.log`
- CTest: `engine/artifacts/engine/s5-evidence/ctest.log`
- Parity report: `engine/artifacts/engine/parity/parity-report.txt`
- Parity captures: `engine/artifacts/engine/parity/parity-gl.png`,
  `engine/artifacts/engine/parity/parity-vk.png`, `parity-diff.png`
- Scene assert: `engine/artifacts/engine/s5-evidence/scene-assert.log`
- UI captures: `engine/artifacts/engine/demo-dialogue4.png`,
  `engine/artifacts/engine/demo-combat4.png`
- Map validation: `artifacts/lanes/s5-mapvalidate.log`,
  `artifacts/lanes/s5-mapvalidate/summary.json`
- Technical note: [docs/recon/42_PLAYABLE_SLICE.md](recon/42_PLAYABLE_SLICE.md)
