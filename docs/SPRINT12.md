# Sprint 12 - creature animation, skinning, weapons and cutscene cameras

Status: **complete - gate S12 met (2026-11-03)**. Sprint 11 closed combat,
Force powers, alignment, the effect stack and the content-specific ability
set. Sprint 12 closes the bind-pose-only creature gap: shipped MDL animation
clips are parsed, evaluated and played; skinned meshes are deformed on the CPU
with the verified `qbone`/`tbone` bind-inverse data; equipped weapons follow
the hand hooks; dialogue camera ids play GIT cameras; and both renderers
refresh entity vertex buffers from the posed mesh.

Baseline: Sprint 11 `35/35` CTest, 117/117 modules, VM 8,666/8,666, scene
parity 0.0300, Python 143 tests / 7 skips, install unchanged.

## Gate S12

1. **Animation data** - parse the MDL animation table, per-clip header,
   animation node hierarchy and controller arrays. Cover position (type 8),
   orientation (type 20, including packed quaternions) and scale (type 36).
2. **Pose evaluation** - position controllers are additive to the bind local
   translation, orientation replaces the bind rotation, scale multiplies the
   local translation; clips evaluate deterministically at any time including
   Bezier position segments.
3. **Skinning** - parse skin headers, `node -> bone` maps, `qbone`/`tbone`
   bind-inverse arrays and MDX weights/bone ids; CPU skinning reproduces the
   bind pose and deforms vertices from the animated skeleton.
4. **Playback** - `GameSession` owns per-entity animation name/time/loop state,
   defaults creatures to `pause1`/`pause`/`cpause1`/`default`, advances clips
   in `update`, and exposes `playAnimation`/`stopAnimation` plus name/time/
   revision accessors.
5. **Weapon attachment** - `EquippedRes` resolves through the UTI to
   `baseitems.defaultmodel`; weapon meshes attach to `Rhand_g`/`Lhand_g` and
   follow the animated hand pose.
6. **Cutscene cameras** - GIT `CameraList` cameras load with position,
   orientation, pitch, height and FOV; DLG `CameraID` entries/replies select a
   camera; `GameSession::camera()` returns the cinematic camera while dialogue
   is active.
7. **Script bridge** - `animations.2da` names and flags load; `PlayAnimation`
   and `ActionPlayAnimation` map an animation id to a clip and play it on
   `OBJECT_SELF`.
8. **Rendering** - both GL and Vulkan refresh entity vertex buffers when the
   animation revision changes; room geometry and bind-pose topology stay
   stable.
9. **Regression** - **36/36** CTest, 117/117 modules, VM 8,666/8,666 with
   0 mismatches, scene parity 0.0300, Python 143 tests / 7 skips, install
   sentinel 0 session changes.
10. **Evidence and docs** - report, technical note, fixture and logs.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s12-01-mdl | animation table, clips, controllers, packed quaternions | `engine/src/kotor/Geometry.cpp` |
| s12-02-skin | skin headers, bone maps, `qbone`/`tbone`, MDX weights | `engine/src/kotor/Geometry.cpp` |
| s12-03-playback | session animation state, defaults, update loop | `engine/src/game/GameSession.*` |
| s12-04-script | `animations.2da` and `PlayAnimation` bridge | `engine/src/game/Rules.*`, `GameSession.*` |
| s12-05-attach | equipped weapon models on hand hooks | `engine/src/kotor/Entities.cpp`, `Formats.h` |
| s12-06-camera | GIT cameras and DLG camera playback | `engine/src/kotor/Scene.cpp`, `Tlk.cpp`, `GameSession.*` |
| s12-07-render | entity mesh refresh for GL/Vulkan | `engine/src/render/*`, `Application.cpp` |
| s12-08-evidence | gate, report, recon note, regression logs | `engine/CMakeLists.txt`, `docs/` |

## Outside this sprint

- Dangly/saber vertex simulation and particle/light controllers.
- Animation blending, transitions, root motion and inverse kinematics.
- Camera animation clips (`CameraAnimation`) and dialogue camera effects.
- Audio/EAX, Bink movies, lip sync, full `.gui`, minigames, localization,
  mods/Override runtime precedence and the P6 content checklist.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --animation-test `
  --animation-script "tests\fixtures\anim_script_test.ncs" --null --headless
```
