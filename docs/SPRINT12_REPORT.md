# Sprint 12 report - creature animation and skinning

Status: **complete - gate S12 met (2026-11-03)**. Sprint 11 closed combat,
Force powers, alignment, the effect stack and content-specific abilities.
Sprint 12 closes the bind-pose-only creature gap: shipped MDL clips are parsed,
evaluated and played; skinned meshes deform on the CPU with the verified
`qbone`/`tbone` bind-inverse data; and both renderers refresh entity vertex
buffers from the posed mesh.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 36
Total Test time (real) = 140.37 sec
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

## 5. Rendering

`EntityMesh` retains its `Mdl`, bind triangles and deformed vertices.
`refreshEntityMesh` regroups posed triangles by texture and updates positions,
normals and UVs; topology and texture batches stay stable. OpenGL uses
`glBufferData` with `GL_DYNAMIC_DRAW`; Vulkan maps the existing host-visible
vertex allocation and copies the new vertices. The application refreshes only
when `GameSession::animationRevision()` changes.

Room geometry stays bind-only (0x21 trimesh) so the 235,038-vertex scene
baseline is unchanged. Creature skins now contribute their real textures, so
the scene texture count is **58** instead of the old skinned-mesh omission of
53. A GL capture at frame 10 versus frame 70 differs in **12,937 of 79,200**
3D-view pixels after excluding the HUD.

## 6. Regression

- **CTest: 36/36 passed** (140.37 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0300**, PASS.
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 7. Outside Sprint 12

- Weapon attachment to hand hooks.
- Cutscene camera playback and dialogue camera animation.
- Dangly/saber vertex simulation, particle/light controllers.
- Animation blending/transitions, root motion and inverse kinematics.
- Audio/EAX, Bink movies, lip sync, full `.gui`, minigames, localization,
  mods/Override precedence and the P6 content checklist.

## Evidence index

- `docs/evidence/s12/animation.log`
- `docs/evidence/s12/anim-frame-10.png`, `anim-frame-70.png`, `anim-diff.txt`
- `docs/evidence/s12/ctest.log`
- `docs/evidence/s12/all-modules.log`, `all-modules.json`
- `docs/evidence/s12/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s12/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s12/tools-tests.log`
- `docs/evidence/s12/install-sentinel.log`
- Fixture: `engine/tests/fixtures/anim_script_test.nss/.ncs`
- Technical note: [docs/recon/50_ANIMATION_SKINNING.md](recon/50_ANIMATION_SKINNING.md)
