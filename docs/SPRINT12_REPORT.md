# Sprint 12 report - creature animation, skinning, weapons and cutscene cameras

Status: **complete - gate S12 met (2026-11-03)**. Sprint 11 closed combat,
Force powers, alignment, the effect stack and content-specific abilities.
Sprint 12 closes the bind-pose-only creature gap: shipped MDL clips are parsed,
evaluated and played; skinned meshes deform on the CPU with the verified
`qbone`/`tbone` bind-inverse data; equipped weapons follow hand hooks;
dialogue camera ids play GIT cameras; and both renderers refresh entity vertex
buffers from the posed mesh.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 36
Total Test time (real) = 143.42 sec
```

The new target is `kotor_animation`; all Sprint 11 targets still pass.
Python tools remain **143 tests, 0 failures, 7 skips**.

## 2. MDL animation data

- Model header animation table at `+88/+92`; 136-byte clip header; clip name,
  duration, transition and animation root.
- 80-byte animation nodes with supernode binding, children and controller
  arrays.
- Controller types:
  - **8 position** - linear (3 columns) or Bezier (19 columns), additive to
    the bind local translation.
  - **20 orientation** - 4-float WXYZ or packed 11/11/10-bit quaternion;
    shortest-arc slerp.
  - **36 scale** - one or three components.
- Missing animation nodes leave the bind local transform; supernodes bind
  runtime nodes to animation nodes.
- Local clips take precedence; supermodel clips are merged recursively with
  cycle protection and name-level override.

## 3. Skinning

Skin headers are read from `node + 80 + 332` (K1):

- MDX weights offset and bone-id offset;
- node-to-bone map;
- per-node `qbone` (WXYZ) and `tbone` (Vec3) bind-inverse arrays;
- MDX 4-weight/4-bone vertex records.

Verified bind formula:

```text
meshOrigin = bind world translation of the skinned mesh node
local      = R(qbone[bone]) * (p - meshOrigin) + tbone[bone]
world      = world[bone] * local
```

At bind this reproduces the MDX positions:

- `n_bith` max error **< 0.001**;
- `c_bantha` max error **< 0.01**.

During playback the bind world transforms are replaced by the animated pose.

## 4. Playback and script bridge

- `GameSession::playAnimation`, `stopAnimation`, `animationName`,
  `animationTime` and `animationRevision`.
- Per-entity default clip: `pause1`, `pause`, `cpause1`, then `default`.
- `updateAnimations` advances and loops clips, then refreshes the mesh.
- `animations.2da` loads names plus `stationary`/`looping` flags.
- `PlayAnimation` and `ActionPlayAnimation` resolve the id and play on
  `OBJECT_SELF`.

The compiled fixture `engine/tests/fixtures/anim_script_test.ncs` validates the
bridge: an NPC runs `PlayAnimation(6,1,0)` and its clip becomes `pause1`.

## 5. Weapon attachment

`Equip_ItemList[].EquippedRes` resolves through the UTI to the base item; the
base item's `defaultmodel` becomes the rendered weapon model. The first
weapon is attached to `Rhand_g` and a second to `Lhand_g`. Weapon meshes keep
their own MDL and textures, and each animation update applies the parent
creature's hand pose to the weapon's bind vertices before the renderer applies
the entity transform.

The Endar Spire scene now has **120 entity meshes** (101 creature/placeable
meshes plus 19 weapon meshes) and **65 textures**. The animation gate checks
`weapon_attachment`, `weapon_hand_hook`, `weapon_models`, `weapon_vertices`,
`weapon_parent` and `weapon_follows_animation`.

## 6. Cutscene cameras

GIT `CameraList` now loads `CameraID`, `Position`, `Orientation` (WXYZ),
`Pitch`, `Height` and `FieldOfView`. DLG entries/replies load `CameraID`,
`CameraAnimation`, `CamVidEffect`, `CamHeightOffset` and `TarHeightOffset`.

`GameSession::cinematicCamera` applies the verified camera formula:

```text
eye     = Position + (0, 0, Height)
forward = R(orientation) * (0, sin(Pitch), -cos(Pitch))
right   = R(orientation) * (1, 0, 0)
up      = right x forward
```

DLG entry/reply `CameraID` selects a static GIT camera. `CameraAnimation`
values use the verified mapping `value = 1200 + (N-1)` -> `CUT{N:03d}W`;
`m01aa_c01` resolves `m01aa_c01_cam` and its 18 clips. The camera model's
`camerahook` node is evaluated with the same MDL pose evaluator; forward is
local +Y and up is local +Z. `GameSession::camera()` prefers an active
animated clip, then falls back to a static `CameraID`, then first-person.

While dialogue is active, a non-negative `CameraID` overrides the first-person
camera. The gate checks `camera_catalog`, `camera_math`, `camera_height`,
`camera_forward`, `dialogue_camera_apply`, `camera_model`,
`camera_animation_field`, `camera_clip_load`, `camera_clip_pose`,
`camera_clip_advance` and `dlg_camera_field`.

## 7. Rendering

`EntityMesh` retains its `Mdl`, bind triangles and deformed vertices.
`refreshEntityMesh` regroups posed triangles by texture and updates positions,
normals and UVs; topology and texture batches stay stable. OpenGL uses
`glBufferData` with `GL_DYNAMIC_DRAW`; Vulkan maps the existing host-visible
vertex allocation and copies the new vertices. The application refreshes only
when `GameSession::animationRevision()` changes.

Room geometry stays bind-only (0x21 trimesh) so the 235,038-vertex scene
baseline is unchanged. Creature skins and weapons contribute their real
textures. A GL capture at frame 10 versus frame 70 differs in **12,937 of
79,200** 3D-view pixels after excluding the HUD.

## 8. Regression

- **CTest: 36/36 passed** (143.42 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0300**, PASS.
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 9. Outside Sprint 12

- Dangly/saber vertex simulation and particle/light controllers.
- Animation blending/transitions, root motion and inverse kinematics.
- Camera effects (`CamVidEffect`) and height-offset camera presets.
- Audio/EAX, Bink movies, lip sync, full `.gui`, minigames, localization,
  mods/Override precedence and the P6 content checklist.

## Evidence index

- `docs/evidence/s12/animation.log`
- `docs/evidence/s12/anim-frame-10.png`, `anim-frame-70.png`, `anim-diff.txt`
- `docs/evidence/s12/weapon-attach.png` (GL hand-hook capture)
- `docs/evidence/s12/ctest.log`
- `docs/evidence/s12/all-modules.log`, `all-modules.json`
- `docs/evidence/s12/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s12/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s12/tools-tests.log`
- `docs/evidence/s12/install-sentinel.log`
- Fixture: `engine/tests/fixtures/anim_script_test.nss/.ncs`
- Technical note: [docs/recon/50_ANIMATION_SKINNING.md](recon/50_ANIMATION_SKINNING.md)
