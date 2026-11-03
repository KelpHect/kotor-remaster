# 42 - Playable C++ slice: data, renderer, gameplay, gate

Status: Sprint 5 complete (2026-11-03). This note records how the C++ engine turns a real KotOR
install into the playable P4 slice, and the coordinate/format traps that were closed while doing
it. It is a companion to [SPRINT5_REPORT.md](../SPRINT5_REPORT.md).

## 1. Runtime file map

- `engine/src/kotor/Archive.cpp` - KEY/BIF/RIM/ERF readers, SHA-256, resref normalization.
- `engine/src/kotor/Gff.cpp` - GFF V3.2 fields, locstrings, lists, struct slots.
- `engine/src/kotor/Scene.cpp` - IFO/ARE/GIT/LYT module load, room geometry, textures, entity GIT
  lists, trigger geometry.
- `engine/src/kotor/Entities.cpp` - UTC/UTD/UTP/UTT templates, appearance/genericdoors/placeables
  2DA resolution, MDL/MDX entity meshes.
- `engine/src/kotor/Geometry.cpp` - LYT, WOK, MDL/MDX node hierarchy and world triangles.
- `engine/src/kotor/Texture.cpp` - TPC/TGA decode to RGBA.
- `engine/src/kotor/TwoDa.cpp`, `Tlk.cpp` - 2DA V2.b and TLK V3.0; DLG parse in `Tlk.cpp`.
- `engine/src/render/GlScene.*` - GL 4.5 scene pass and entity meshes.
- `engine/src/render/VulkanScene.*` - Vulkan 1.4 dynamic-rendering scene pass and entity meshes.
- `engine/src/render/Bmp.cpp` - shared 24-bit BMP writer.
- `engine/src/game/GameSession.*` - player, collision, dialogue, combat, triggers, save/load.
- `engine/src/app/Application.cpp` - flags, HUD/dialogue UI, demo and gate modes.

## 2. Data path

The install is opened read-only. `Install` verifies `swkotor.exe` against the supported 1.0.3.0
SHA-256, loads `chitin.key`, binds `modules/<stem>.rim` and `_s.rim`, then adds `patch.erf` and
the texture packs. Lookup order is module archives, patch, texture packs, then KEY/BIF.

Module load:

1. IFO `Mod_Area_list` -> area name.
2. ARE/GIT/LYT for the area.
3. LYT room list; each WOK gives collision facets, each MDL/MDX gives room triangles.
4. GIT `Door List`, `Placeable List`, `Creature List`, `WaypointList`, `TriggerList`, etc.
5. UTC/UTD/UTP/UTT templates provide names, conversations, factions, HP/stats, and model refs.
6. `appearance.2da` resolves creature `modela`/`race` and texture; `genericdoors.2da` resolves
   door models from `GenericType`; `placeables.2da` resolves placeable models from `Appearance`.
7. MDL/MDX entity triangles are grouped by texture and kept as per-entity meshes; the renderer
   applies the GIT position/bearing.
8. Trigger GIT polygons are triangulated at load time.
9. `dialog.tlk` is read from the install root and used for entity names and DLG strrefs.

KotOR 1 uses magic `ARE `/`GIT `/`IFO ` + `V3.2`; the generic `GFF ` check was wrong for every
shipped module and is now a version check at offset 4.

## 3. Coordinates and transforms

- Right-handed Z-up, 1 unit = 1 m.
- IFO `Mod_Entry_X/Y/Z` and `Mod_Entry_Dir_X/Y` define the spawn and heading.
- GIT doors/placeables use `X/Y/Z` + `Bearing`; creatures use `XPosition/YPosition/ZPosition` +
  `XOrientation/YOrientation`, converted with `atan2(y, x)`.
- The camera eye is player feet + 1.55 m; forward is `(cos(pitch)cos(yaw), cos(pitch)sin(yaw),
  sin(pitch))`.
- Room MDL vertices are transformed by the node hierarchy and the LYT room origin. Entity MDL
  vertices stay in model space; the renderer composes `translate(position) * rotateZ(bearing)`.
- ARE light/fog colors are packed `UInt32` BGR, not `Vector3`; zero sun values use Classic defaults.
- The walkmesh is world-space; floor height is the highest upward facet under the player, and wall
  blocking tests the player's radius against non-floor facets.

## 4. Renderer

Both backends use the same scene vertex layout (`position`, `normal`, `uv`) and the same texture
list. The room and each entity mesh are separate GPU meshes; entity meshes are skipped when the
creature is dead.

OpenGL 4.5:

- Core profile context; scene VAO/VBO/IBO created at upload.
- One shader with `uModel` and `uViewProjection`; room uses identity, entities use their GIT
  transform.
- `glReadPixels` + shared BMP writer for captures.

Vulkan 1.4:

- Dynamic rendering with a D32 depth attachment; staging buffers for entity/room vertices and
  textures.
- Push constants contain `model`, `viewProjection`, ambient/diffuse/fog, and fog near/far.
- The projection Y row is flipped before the view multiply because the 3D pass uses GL's Y-up
  convention while Vulkan's viewport is Y-down; ImGui keeps its own Vulkan projection.
- The swapchain prefers `VK_FORMAT_B8G8R8A8_UNORM` for Classic parity. An sRGB swapchain would
  encode already-sRGB texels a second time; the UNORM path matches GL.
- Screenshot readback converts BGRA to RGBA and reverses rows for BMP.

Parity is checked by `engine/scripts/check_parity.py`, a verification-only Pillow script. It
captures both backends at the same camera and compares the scene region, excluding the HUD FPS
text. Current result: 0.03/255 mean absolute channel difference.

## 5. Gameplay

- **Dialogue**: DLG entries/replies are parsed with `RepliesList`/`EntriesList`, text is resolved
  through the TLK, `Script`/`Quest` fields are event-logged, and the UI offers numbered replies.
  Starting entries are selected by the first spoken line; full `Active` condition evaluation is
  P5 work.
- **Combat**: hostiles are faction 4 (Sith) in the Endar Spire slice. The player and NPCs use a
  d20 + attack bonus vs defense, damage ranges, HP, and death. NPCs close to 1.5 m and attack on a
  2.5 s timer; the player attacks every 1.8 s in range.
- **Triggers**: GIT trigger geometry is triangulated; a point-in-triangle XY test fires
  `ScriptOnEnter`/`ScriptOnExit` once per transition.
- **Placeables**: UTP `Useable` is loaded; the interaction path records `use <tag>` within 2.5 m
  (the Endar Spire footlocker is the gate case). There is no inventory/loot UI yet.
- **Doors**: `LinkedToModule`/`LinkedTo` cause a module reload; `LinkedTo` positions the player at
  the target door. The transition copies the strings before reloading the scene.
- **Save/load**: `kotor.engine.player/1` stores module, position, yaw, pitch, and HP. This is an
  engine-local snapshot, not original KotOR save compatibility.
- **UI**: HUD (module, position, HP, target, counts, events, hints), dialogue panel with reply
  buttons, and a combat log. No full `.gui` screen set.

## 6. Gate harness

`--playthrough` is deterministic and headless:

1. Trask dialogue (walks; script-spawn fallback when the direct path is blocked).
2. `end_trig02` enter event (trigger centroid).
3. `end_locker01` footlocker use through the placeable path.
4. First hostile fight to death.
5. First linked door transition (`end_m01aa -> end_m01ab`).
6. Save/load round-trip in the new module.
7. Advance simulated time to 1800 s.

`--landmark-test` checks known GIT/IFO positions and headings. `--scene-assert` asserts the exact
loader inventory. `--dialogue-test`, `--combat-test`, `--scene-stats`, `--walk-test`, and
`--door-test` are the other headless gates. CTest runs all 13.

## 7. Gotchas closed

- Empty locstrings with only a strref must be resolved through `dialog.tlk`.
- The synthetic WOK count table ends at 0x60; data must start after it.
- GL scene upload must create its VAO after the shader/program is ready.
- Vulkan depth must exist before the first dynamic-rendering frame.
- Shader SPIR-V must be copied at build time, not only at configure time.
- Do not pass a reference into `scene_` (e.g. `linkedTo`) into a function that replaces `scene_`.
- Vulkan/GL parity needs the UNORM swapchain, projection-Y flip, and BGRA readback conversion.
- The Mdl parser ignores animation/scale controllers; static bind pose is the P4 scope.

## 8. Open work

- Creature animation, skinning, weapons, and equipment visuals.
- NCS execution and full DLG condition/action evaluation.
- Full combat rounds, feats, Force powers, party, inventory, and death presentation.
- Original save compatibility and the complete GUI/TLK/audio stack.
- All 117 areas, mods/Override precedence in the runtime, and P5/P6 parity.
