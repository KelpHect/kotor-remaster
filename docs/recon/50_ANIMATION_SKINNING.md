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
array index. A missing animation node leaves the bind local transform.

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
`animationLoop` and `revision`. Creatures default to the first available of
`pause1`, `pause`, `cpause1`, `default`. `updateAnimations` advances and loops
the clip, recomputes the posed triangles and calls `refreshEntityMesh`.

`animations.2da` supplies the id-to-name map plus `stationary` and `looping`
flags. `PlayAnimation` / `ActionPlayAnimation` (ids 300/40) resolve the clip
and play it on `OBJECT_SELF`.

## 5. Rendering refresh

`EntityMesh` retains its `Mdl`, bind triangles and deformed vertices.
`refreshEntityMesh` regroups the posed triangles by texture and updates
positions/normals/UVs; topology and texture batches are stable. GL issues a
dynamic `glBufferData` for each changed entity mesh; Vulkan maps the existing
host-visible vertex allocation and copies the new vertices. The application
refreshes only when `GameSession::animationRevision()` changes.

## 6. Validation

- `n_bith`: 10 clips, `pause1` 3.0 s, `walk` 1.0 s, 4 skinned nodes, bind
  error < 1e-3, posed vertices move and remain finite.
- `c_bantha`: 3 skinned nodes, bind error < 1e-2, posed `cwalk` vertices
  finite.
- Endar Spire: `n_sithappren` plays `pause1`; 8,355 vertices change after
  0.96 s; `animations.2da` row 6 is `pause1` and loops.
- Script fixture `anim_script_test.ncs` calls `PlayAnimation(6,1,0)` on an
  NPC and the NPC's clip becomes `pause1`.

## 7. Limits

Animation blending/transitions, root motion, inverse kinematics, weapon
attachment, cutscene cameras and dangly/saber simulation are outside Sprint 12.
They consume this pose evaluator but are separate systems.
