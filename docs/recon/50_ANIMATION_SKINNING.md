# Recon 50 - MDL animation and skinning

Status: Sprint 12 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## 1. MDL animation layout

Offsets are relative to file byte 12 (the MDL data section). The model header
is 196 bytes:

- animation table offset at `+88`, count at `+92`;
- supermodel at `+136`, model scale at `+132`;
- name-offset table at `+184`.

Each animation entry points to a 136-byte `AnimationHeader`:

- geometry header 80 bytes; clip name at `+8`;
- duration `+80`, transition `+84`;
- animation root offset `+40`;
- events offset/count `+120`/`+124`.

Animation nodes use the same 80-byte node header as model nodes:
type `+0`, supernode `+2`, node id `+4`, parent `+12`, position `+16`,
orientation WXYZ `+28`, children `+44/+48`, controllers `+56/+60`,
controller data `+68/+72`.

## 2. Controllers

Each controller is 16 bytes:

| offset | field |
|---:|---|
| +0 | type u32 |
| +6 | row count u16 |
| +8 | key-array index u16 (float index) |
| +10 | data-array index u16 (float index) |
| +12 | column count u8 (bit 4 = Bezier) |

Arrays are located at `node.controllerData + index * 4`. Controller types:

- **8 position**: columns 3 linear, or 19 (`0x13`) = 3 values with in/out
  tangents; position is additive to the bind local translation.
- **20 orientation**: columns 4 float WXYZ, or columns 2 = one packed u32 per
  row. Packed layout is 11/11/10 bits for x/y/z in `[-1,1]`, with
  `w = sqrt(1 - x^2 - y^2 - z^2)`; q and -q are equivalent.
- **36 scale**: one or three components; multiplies the local translation.

Key times are floats and clamp to the first/last key. Linear components use
lerp; orientation uses shortest-arc slerp. Position Bezier uses the stored
value/out-tangent/in-tangent quadruple.

Animation nodes bind to runtime nodes by **supernode** (`u16` at `+2`), not by
array index. A missing animation node leaves the bind local transform. Local
clips take precedence; `Mdl::mergeAnimations` appends supermodel clips whose
names are not already present (recursive, cycle-limited).

## 3. Skinning

A skin mesh is node type `0x61` (mesh bit `0x20` plus skin bit `0x40`). After
the 80-byte node header and 332-byte K1 trimesh header, the 100-byte skin
header holds:

| offset | field |
|---:|---|
| +12 | MDX weights offset |
| +16 | MDX bone ids offset |
| +20/+24 | node-to-bone map offset/count |
| +28/+32 | qbone offset/count |
| +40/+44 | tbone offset/count |

`qbone` is a WXYZ quaternion and `tbone` a Vec3 per node; the MDX vertex has
four float weights and four float bone ids. The verified bind formula is:

```text
meshOrigin = bind world translation of the skinned mesh node
local      = R(qbone[bone]) * (p - meshOrigin) + tbone[bone]
world      = bindWorld[bone] * local
```

At the bind pose this reproduces the MDX position (max error < 1e-5 on
`n_bith` and `c_bantha`). During playback `bindWorld` is replaced by the
animated node transform.

## 4. Playback and bridge

`GameSession` stores per-`EntityMesh` `animation`, `animationTime`,
`animationLoop`, previous/current blend state and `revision`. Creatures default
to the first available of `pause1`, `pause`, `cpause1`, `default`.
`updateAnimations` advances and loops the clip, recomputes the posed triangles
and calls `refreshEntityMesh`.

Clip changes use the target clip's `transition` time. `Mdl::blendedTriangles`
evaluates both poses, lerps node translations and linearly blends rotation
matrices, then re-orthonormalizes each basis before skinning. The blend ends
exactly at the target pose. `Mdl::rootMotion` returns the root-node world
translation; sampled shipped clips are root-in-place and use only controller
types 8/20/36/132, so no IK controller data exists to apply.

`animations.2da` supplies the id-to-name map plus `stationary` and `looping`
flags. `PlayAnimation` / `ActionPlayAnimation` (ids 300/40) resolve the clip
and play it on `OBJECT_SELF`.

## 5. Weapon attachment

An `Equip_ItemList` entry's struct type is its slot mask: 0x10 the right hand, 0x20 the left. The
UTI gives `BaseItem` and `ModelVariation`, and the model is `<itemclass>_<variation>` (retail
`005b30e0`); `defaultmodel` is only the variation 001. Retail `00610f40` and `0069a6b0` hang the
items on the `rhand` and `lhand` hooks, not the `Rhand_g` and `Lhand_g` bones, and scale them by
appearance `weaponscale`. The hook sits about 9 cm from the bone and turned about 84 degrees from
it: an item on the bone is off the grip and points the wrong way. Weapon meshes are separate
`EntityMesh` records with the creature as `attachModel`; each refresh takes the hook's pose from
the body's clip and its blend.

### Clips by what the hands hold

Retail `00613710` reduces the hands to a class: 8 empty; right hand alone by `weaponwield` 1 to 1,
2 to 2, 3 to 3, 4 to 5, 5 to 7, 6 to 9; with a left hand, left `weaponwield` 2 to 4 and 4 to 6,
otherwise 0. Retail `0069f650` then picks the row:

| Class | Holds | Ready | Walk, run with weapons out | Attack |
|---|---|---|---|---|
| 1 | baton | `g1r1` | `walk`, `run` | `g1a1`, `g1a2` |
| 2 | one melee | `g2r1` | `walkSS`, `runSS` | `c2a1`..`c2a5` |
| 3 | two-handed | `g3r1` | `walkST`, `runST` | `c3a1`..`c3a5` |
| 4 | two melee | `g4r1` | `walkDS`, `runds` | `c4a1`..`c4a5` |
| 5 | pistol | `g5r1` | `walk`, `run` | `b5a1` |
| 6 | two pistols | `g6r1` | `walk`, `run` | `b6a1` |
| 7 | rifle | `g7r1` | `walkRF`, `runRF` (always) | `b7a1` |
| 8 | nothing | `g8r1` | `walk`, `run` | `g8a1`, `g8a2` |
| 9 | repeater | `g7r1` | `walkRF`, `runRF` (always) | `b9a1` |

Models without these rows take the other model type's: `00614110` gives `b0a1` for ranged without
a feat (`b0a2`..`b0a4` by feat), `00613da0` gives `g0a1` plus the attack index for melee (table row
0x114 onward). `0069f650` gives `die` and `dead` (rows 0x50, 0x51), or `cdie` and `cdead` (0x112,
0x113). A war droid (`c_drdwar`) has only the second kind: `cdie` is its explosion and `cdead` the
wreck. Retail frames at `tar_m02aa` show the wrecks and a soldier's body still there about a minute
after they died, so a killed creature's body stays; how long it stays beyond that isn't measured.

A creature's bearing is the way it faces (`GetFacing`, the GIT orientation vector), and its model
faces +Y, so the model turns by the bearing less a quarter turn. Doors and placeables turn by
`Bearing` itself. Ours shot from retail's measured camera at `tar_m02aa` drew every creature a
quarter turn off until `Entity::drawBearing` applied this; with it, the PC's `g2r1` pose and the
droids' facing line up with the retail frames.

Attacks are `00613da0` (melee) and `00614110` (ranged, the row without a feat). `00647130` runs a
powered item's `powerup`, `powered`, `powerdown` and `off` clips as the weapons come out and go
away. Clips whose `animations.2da` `hideequippeditems` is 1 hide the items.

Inferred: "weapons out" is taken to be combat mode, from `006182a0` setting the flags as combat
mode changes. Standing ready while in combat, the fallback attack rows for creatures, and cycling
the attack index are this engine's reading. An item stays visible when the weapons are away.

## 6. Cutscene cameras

GIT `CameraList` entries store `CameraID`, `Position`, `Orientation` WXYZ,
`Pitch` (degrees), `Height` and `FieldOfView`. The verified formula is:

```text
eye     = Position + (0,0,Height)
forward = R(orientation) * (0,sin(Pitch),-cos(Pitch))
right   = R(orientation) * (1,0,0)
up      = right x forward
```

The loader reads the GFF floats as W X Y Z (X and Y are 0 in all 1,099 shipped cameras); reading
them X Y Z W rolls every camera about X and put the speaker in view for 8 of 1,744 resolvable
static-camera dialogue lines, against 1,443 (median 8.9 degrees off axis) with W first.

A camera-model `camerahook` (`CameraAngle` 4, clips `CUTnnnW`) is a modeller's camera: it looks along
local -Z with local +Y up, the same as the minigame camera hooks. Over the 236 shipped clips of the
24 `*_cam` models, -Z is level (|z| < 0.5) in 91% and +Y is within 26 degrees of world up in 90%;
the former forward (local +Y) was level in 5%. Who the animated cutscenes frame is not checked
against actor positions: the staged characters are placed by script, not by the GIT.

DLG `CameraAngle` 1/2/3 uses `CamHeightOffset`/`TarHeightOffset` and the
verified speaker/target framing formulas. `camera()` prefers the animated
clip, then a preset, then a static `CameraID`, then first-person.

Retail `006bb7b0` (called from `006bd260`) computes the preset eyes from the CAMERAHOOK node of each
creature, with FOV 55 pushed at `0x6bd2fc`. Hooked live on `end_trask01`, the eyes for angles 1, 2 and
3 land within 6 mm of `presetCamera`. Lines carrying `CameraAngle` 0 are not one shot: paired frames
show the close-up, the over-the-shoulder and the two-shot on different lines, and the choice is made
before `006306e0` (it arrives through the queue at `this+0x124`). The choice is a fixed table at `+0xa8`
walked one row per line from a seed set at `0059f220` (rows 1,3,1,3,2,2,1,3,1,2,1,2,1,3,2,3,1,1,1);
`game/DialogueShots.h` follows it, checked on `end_trask01` and `tar02_postraid`. The follow camera comes from `camerastyle.2da`
by ARE `CameraStyle` (retail reads at `00508c50`): the eye is `distance` behind the creature and
`height` above its camerahook, looking `90 - pitch` degrees below level, measured on four styles. A static
camera's projection fovy is its GIT `FieldOfView`: read on retail at 50, 55 and 45 in `tar_m02aa`, and at
55 on cameras 5 and 9 of `end_m01aa`.
`CamVidEffect` is -1/0 only in the 1,135-DLG corpus (45,396 / 39, 0
non-default), so non-default effect semantics are evidence-based out of scope.

A textless DLG entry that pauses the conversation holds: `k_ptar_shoot` on the third entry of
`tar02_preraid` calls `ActionPauseConversation` and, 1.08 s later, `SetDialogPlaceableCamera(5)`, then
resumes after a delay. On retail the entries of that conversation land on cameras 1, 3, 4, 5, 6, 2, 8 at
0.8, 6.2, 15.8, 17.0, 21.3, 30.0 and 32.2 s, with the eyes equal to ours to 3 decimals. `GetObjectByTag`
lowers its tag (`005e6080`), and `k_ptar_sithtalk` starts the conversation on the trigger's target
placeable `trooper1_invis`.

Each line turns its speaker and listener toward each other (`006306e0`). Logged live on `tar02_preraid`:
creatures with `appearance.2da` `headtrack` 0 face the other exactly; the PC (`headtrack` 1) turns only as
needed to keep the other within `head_arc_h`, and its head follows the other's `camerahook` on `hturn_g`,
clamped to `head_arc_h` and `head_arc_v`, within 10 m, blend time 0.25 (`006146e0`, `00487190`, tick
`00481dc0`). `SetLockOrientationInDialog` skips the creature, `SetLockHeadFollowInDialog` makes it face
fully. Starting a death clip plays `appearance.2da` `DeathVFX` (`0063eeb2`), 4034 on the war droid.

That effect (`v_drdexpld_01`) throws solid pieces: emitters with a `chunk` model and no sprite. Ours draws each
particle as that model, tumbling, in the emitter's start colour. Inferred from frames of the retail fight; retail's
chunk path was not read.

## 7. Dangly and saber meshes

Dangly nodes (`type & 0x100`) carry a 28-byte header after the trimesh:
constraint offset/count, `displacement`, `tightness`, `period`, and an unused
danglyverts pointer. Per-vertex constraints are floats in model order. The
runtime applies a deterministic damped settle:

```text
settle = 1 - exp(-max(tightness, 0.1) * t) * cos(max(period, 0.1) * t)
z    -= displacement * constraint * clamp(settle, 0, 1)
```

Saber nodes (`type & 0x800`) carry a SaberHeader after the trimesh with
vertex/UV/normal offsets. The 22 shipped saber MDLs have stride 0 trimesh
headers but valid 352-vertex / 24-face SaberHeader geometry. They contain no
constraints or springs, so they render as static blade geometry; dynamic saber
physics is re-scoped with this shipped-data evidence.

Light nodes (`type & 0x02`) and emitter nodes (`type & 0x04`) evaluate every
controller family through `Mdl::fxNodeParameters(ordinal, animation, time)`.
Each `FxParameter` keeps the controller's native width and is sampled by
clamped linear interpolation between keys. The model covers:

- light: 76 colour, 88 radius, 96 shadow radius, 100 vertical displacement,
  132 alpha, 140 multiplier;
- emitter: 80/84/216/220-228 alpha and percentage curves, 88 birth rate,
  92 bounce, 96 combine time, 100 drag, 104 fps, 108/112 frame bounds,
  116 gravity, 120 life expectancy, 124 mass, 128/132 Bezier, 136 rotation,
  140 random velocity, 144/148/152/156/232/236 sizes, 160 spread,
  164 threshold, 168 velocity, 172/176 xy size, 180 blur length, 184-200
  lightning, 240 random birth rate, 252 target size, 284 colour mid,
  380/392 colour end/start.

`fxTriangles` consumes the model: light nodes emit additive quads using
colour/radius/shadow/multiplier/vertical displacement; emitter nodes emit
alpha quads with birth-rate particle count, lifetime phase, alpha/colour/size
curves, velocity/gravity/drag/mass motion, spread/random-velocity offsets,
Bezier/lightning offsets, blur stretching and threshold culling. Room lights
are appended to the room mesh; entity effects are separate refreshed
`EntityMesh` records. Particle positions are a deterministic function of
time, not random state.

FX triangles carry `BlendMode::{Opaque, Alpha, Additive}` through
`TextureBatch`. Lights are additive (`SRC_ALPHA, ONE`), emitters are alpha
(`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`), and static geometry stays opaque. OpenGL
sets `glBlendFunc` per batch; Vulkan binds base/alpha/additive pipelines.

## 8. Rendering refresh

`EntityMesh` retains its `Mdl`, bind triangles and deformed vertices.
`refreshEntityMesh` regroups the posed triangles by texture and updates
positions/normals/UVs; topology and texture batches are stable. GL issues a
dynamic `glBufferData` for each changed entity mesh; Vulkan maps the existing
host-visible vertex allocation and copies the new vertices. The application
refreshes only when `GameSession::animationRevision()` changes. Both renderers
report the live blend inventory (24 alpha, 17 additive batches on Endar
Spire); Vulkan creates and binds its alpha/additive pipelines successfully.

## 9. Validation

- `n_bith`: 10 clips, `pause1` 3.0 s, `walk` 1.0 s, 4 skinned nodes, bind
  error < 1e-3, posed vertices move and remain finite.
- `c_bantha`: 3 skinned nodes, bind error < 1e-2, posed `cwalk` vertices
  finite.
- Endar Spire: `n_sithappren` plays `pause1`; 8,355 vertices change after
  0.96 s; `animations.2da` row 6 is `pause1` and loops.
- Script fixture `anim_script_test.ncs` calls `PlayAnimation(6,1,0)` on an
  NPC and the NPC's clip becomes `pause1`.
- Endar Spire now has **142 entity meshes**, **84 textures**, **237,594**
  vertices and **49 batches**; the weapon gate checks hand hooks, models,
  finite vertices, parent linkage and motion while the creature animates.
- GIT `CameraList` loads; `cinematicCamera` matches the stored eye height and
  unit forward; `dialogue_camera_apply` proves `camera()` uses an active
  dialogue camera; `end_trask01` contains a non-negative `CameraID`.
- `m01aa_c01` resolves `m01aa_c01_cam` with 18 `CUT###W` clips; `CUT001W`
  poses `camerahook`, is finite and unit-forward, and advances between frames.
- `c_rakghoul` has a dangly node whose constraints move vertices and remain
  finite; `w_lghtsbr_001` loads static SaberHeader geometry with no dangly or
  skin nodes.
- `CameraAngle` 1/2/3 presets compute finite, unit-forward cameras.
- Corpus scan of 1,135 DLGs: 45,396 `CamVidEffect=-1`, 39 `CamVidEffect=0`,
  0 non-default.
- `fx_flame01` exposes 33 controller types across 3 FX nodes; the parameter
  model returns exactly that set, all finite and deterministic at equal sample
  times. The 80/84/132/144/380/392 families are asserted individually, and
  room lights (`m01aa_01a`) expose 76/88/140 with stable parameter values.
  FX triangles are finite and animated. Non-default `CamVidEffect` is 0 across
  the 1,135-DLG corpus.
- `blendedTriangles` produces finite cross-fade vertices; `rootMotion` is
  unchanged between sampled frames (root-in-place); `hasNonPoseControllers`
  is false for the sampled pose clips (no IK controllers).

## 10. Limits

Exotic light/emitter controller families beyond the implemented billboard path,
non-default `CamVidEffect` semantics and audio/cutscenes are outside Sprint 12.
