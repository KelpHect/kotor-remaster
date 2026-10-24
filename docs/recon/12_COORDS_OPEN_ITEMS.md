# Coordinate, animation, and camera findings (lane 10)

Status: **verified at the shipped-code boundary.** Bearing/door facing, `WOK.position`,
animation controller payloads, GIT/ARE camera-minimap fields, DLG camera models, `CameraAnimation`,
`CameraAngle`, camera mode `+0x38`, and supermodel precedence/binding are decoded. Remaining P1
observations: dialogue-camera object live check, supermodel visual chain, `MapResX` semantics, and
the controller-type catalogue not needed for skeletal pose. MDX is in `docs/recon/15`. Data is
under `artifacts/lanes/10-coords-open/` (untracked); the install was not modified.

## Corpus

106 areas (GIT/LYT/IFO/ARE): 1,546 creatures, 704 doors, 3,138 placeables, 3,970 waypoints,
1,099 cameras. Bearing evidence: 699 doors, 5 chairs, 1,196 PWK placeables. Animation survey:
2,832 MDLs. MDX sample: 634 blocks / 84,707 faces (plus a broader 528-file pass).

## Bearing and door facing (RH verified in code and by data)

* `Bearing` is a signed radian yaw in `[-π, π]`; 206 doors have `Bearing = 0` (205 hooks also 0).
* `Bearing mod π` matches the LYT hook yaw plane for all 699 doors (max `1.55e-4 rad`); the 9
  exact +π cases are front/back-symmetric models.
* **RH proven in engine code:** door loader `0x50a0e0` and placeable loader `0x50a7b0` read
  `Bearing` B, build `q=(cos B/2,0,0,sin B/2)` about +Z (`0x4ac960`), compute
  `forward=(−sin B, cos B, 0)` (`0x5853c0`/`0x5896e0`), and store it at `+0x9c` (`0x4cd170`).
  Save inverts with `atan2(−fx,fy)` (`0x4aa0f0`, `0x586a70`); creatures carry
  `X/Y/ZOrientation` directly (`0x4c8c70`). The PWK use-point oracle independently gives 10
  RH-decisive, 0 LH.
* **Door model selection is UTD `GenericType`**, not `Appearance` (all 358 UTDs have
  `Appearance=0`; 48 reachable `GenericType`s → `genericdoors.2da` → 47 used models, all with
  MDL + 3 DWK states). All used models have identity roots; closed DWKs are X-wide/Y-thin.
  `world = T_GIT · R_z(Bearing) · M_local`.

## `WOK.position` (verified: walkmesh-node translation)

`WOK.position` is exactly the local translation of the room MDL's walkmesh geometry node
(type **545**). The BWM is exported from that node, so it is an export pivot, not a centroid,
LYT origin, or area frame:

* 453 rooms sampled across 103 areas: `WOK.position == node545.pos` **bit-exact, 453/453**; every
  sampled room has exactly one 545 node.
* Rejected across 936 WOKs: centroid 9/936 within 1 cm; local AABB centre 4/936 within 1 mm;
  LYT origin median deviation 141.8 m.
* World placement remains `LYT origin + node chain`; the value is not added. For DWK/PWK it is a
  slide pivot (`dor_lhr01` closed 0, leaves `(-2.785,0.052,0)` = closed X half-extent).

## Animation (payloads verified)

MDL V1.0 layout (offsets relative to file byte 12): animation table at +100 (offset/count),
supermodel +148..179, scale +144. Animation header 44 B; animation node 80 B; controller 16 B:
type +0, unknown +4, rows +6, timeIndex +8, dataIndex +10, columns +12.

* **Type 8 = position** (`columns=3` linear, `columns=19` Bézier stride 9); **type 20 =
  orientation** (`columns=4` XYZW float, `columns=2` packed three 11-bit components, w =
  −sqrt(1−x²−y²−z²)). Array indices are float indices, not byte offsets.
* **Position is additive to the bind local translation; orientation replaces the bind rotation.**
  Clip nodes key a subset of geometry node numbers (p50 33 nodes, 82.8%).
* **No NaN sentinel.** The all-ones packed word `0x7fdffbff` decodes to identity `(0,0,0,w=−1)`;
  0 NaN floats across 120 animated models / 1,619 clips (14,932 identity words).
* Validation: rigid clips (`n_bith`, `c_rancors`) preserve bone lengths to ~1e-15; `dor_lhr01`
  `closed` matches bind at 0.059°; `opening1` leaf X travel −2.020/+3.453 m sums to the 5.473 m
  door width. First-key position offsets are zero for 76–84% of controllers (rest pose).
* Supermodel graph: 375 MDLs name a supermodel, 146 have both; 133 resolve to shipped models.
  116/133 child/super pairs are clip-name disjoint (inherit). 17/133 share names; all 71 shared
  definitions differ.
* **Clip precedence is explicit in code.** Clip lookup `0x4822a0` searches the local array
  (`data+0x58/count+0x5c`) in ascending order, then the linked `+0x64` chain; only the terminal
  model tries `"default"` locally. Playback `0x485bd0` reuses an active same-name instance, then
  searches `+0x64`, then `+0x58`; a miss returns 0 with no state change. Query `0x484d20` checks the
  active list `+0xb0`, then `+0x64`, then `+0x58`; a miss returns duration −1.
* **Binding is by supernode, not raw index.** Node header: type +0, supernode `+2`, node number
  `+4`. Hierarchy clone `0x449200` copies signed16 `[src+2]` to runtime `+0x28`; binder `0x483d80`
  compares that with animation-child `+2` (not the array index). A runtime `+0x28 == −1` skips the
  child; no match scans the animation-child cycle and then skips the runtime child/subtree. A missing
  animation node therefore leaves the prior local state; no bind-pose reset or name fallback at node
  level is observed. Visual confirmation needs a synthetic child/super chain.
* MDX skinning: skinned nodes carry a bone map (57–75 entries, −1 unused) indexing geometry nodes.
  qbone/tbone/aux4 are per-node `qbone_ref_inv` (16 B), `tbone_ref_inv` (12 B), and
  `boneconstantindices` (4 B) arrays (see `docs/recon/15`).

## Cameras and minimap (fields/formulas verified)

GIT `CameraList` (1,099): `CameraID`, `Position`, `Orientation` WXYZ (yaw-only), `Pitch` degrees,
`Height` m, `FieldOfView` degrees.

```
eye     = Position + (0,0,Height)
R_yaw   = yaw from Orientation about +Z
forward = R_yaw * (0, sin(Pitch), -cos(Pitch))   # Pitch=90 horizontal, 0 down, >90 up
right   = R_yaw * (1,0,0)
up      = right x forward
```

Aiming at nearby actors gives median best dot **0.987** (85.9% >0.9) for this convention vs
0.542 for the down-from-horizontal variant. Aspect, near, and far are **absent** from GIT/ARE/DLG;
they are engine/renderer defaults.

ARE `Map` is a nested struct: `MapResX`, `NorthAxis` (0–3), `WorldPt1/2X/Y`, `MapPt1/2X/Y`,
`MapZoom` (1). Per-axis affine:

```
u = MapPt1X + (x - WorldPt1X) * (MapPt2X - MapPt1X) / (WorldPt2X - WorldPt1X)
v = MapPt1Y + (y - WorldPt1Y) * (MapPt2Y - MapPt1Y) / (WorldPt2Y - WorldPt1Y)
```

`MapPt` axis signs are arbitrary per area, so `NorthAxis` is a UI/compass selector, not part of
the affine. 18/106 maps are degenerate (zero extent or `MapResX=0`); `MapResY` is absent;
`MapResX` is a resolution hint (inferred, not used by the affine).

DLG: root `CameraModel` names one of 22 shipped two-node camera MDLs (`camerahook`); clips store
`camerahook` position/orientation with the animation rule above. All 3,036 DLG `CameraID`s fall in
the GIT `CameraID` domain. **`CameraAnimation` is a clip id, not milliseconds:** value =
`1200 + (N−1)` → engine name `cut{N:03d}w` → camera-model clip `CUT{N:03d}W` (`FUN_006288f0` maps
1000–1727 to cut/cutW/cutL/cutWL bands). 208/208 distinct (model,value) pairs resolve to a clip;
the sole outlier is 10098 with no model. All 219 values occur on `CameraAngle=4`.

`CameraAngle` branches: **6** = static GIT camera (3,036/3,036 carry IDs); **4** = camera-model
shot (219 animations); **1/2/3** use `CamHeightOffset`/`TarHeightOffset` into
`FUN_006306e0`→`FUN_006bd260`; **0** falls through the default coefficient path; **5** restores the
prior actor pair and is not a distance preset. `FUN_0059eaa0` parses CameraAngle→+8, CameraID→+0xc
(non-6 forces −1), CamHeightOffset→+0x10, TarHeightOffset→+0x14, CameraAnimation→+0x18, CamVidEffect
→+0x20, CamFieldOfView→+0x1c.

`FUN_006306e0` is an ECX receiver plus **six** 4-byte stack args (`RET 0x18` at `0x631390`), not a
seven-arg C function. `FUN_006bd260` (`RET 0x18` at `0x6bd976`) stores h/t/range at camera
`+0x1c/+0x20/+0x24`. For mode `+0x38==0`, with h,t,r from those fields, Z=(0,0,1), R a +Z yaw of
+30° (or −30° for side 2; ±90° for angle 3), and N a normalize:

```
1: T = A - 0.2 N(d) + (t - 0.04 - 0.2r) Z
   E = T - (0.5 + r) N(R d) + (h + 0.2r) Z
2: D=|B-A|; A'=A-0.1DZ; B'=B-0.05DZ; d=A'-B'; T=B'+0.3d+tZ
   E = T - 0.8 R d - 0.15 N(R d) + h Z
3: D=|B-A|; d=A-B; T=B+0.5d+(t-0.2D)Z
   E = T - 1.5 R d + (h+0.3D) Z
```

Constants are at `0x6bb861–0x6bb90c`; position math at `0x6bc090–0x6bc545`. Coincident anchors set
the close-up flag `+0x34` and replace B by `A+(1.5,1.5,0.1)`.

**Camera mode `+0x38` is construction-time and zero in shipped code.** The dialogue-camera object is
a separate 0x9c-byte class (vtable `0x756ae8`, constructor `0x6bb670`, virtual ID `0x106d` from
`0x6bb740`). Its `+0x38` is written only at construction (`0x6bb6f8`) from controller `+0xc4`; the
camera factory `FUN_006412f0` receives that value (`0x641362`–`0x64136f`), and the dialogue-controller
constructor zeroes `+0xc4` (`FUN_0062fed0` at `0x630244`, `EBX=0` from `0x62feed`). A whole-program
`[reg+0xc4]` write scan (64 writes, stack excluded) finds no other write attributable to that
controller; within the dialogue-camera code the only reads are the three camera-creation reads
(`0x62bf4e`, `0x6320c9`, `0x632127`). Derived conclusion: mode `+0x38` is 0 for every shipped camera
and the nonzero branches are unreachable (legacy/disabled), unless an unexamined alias or an
unrelated class holding the controller pointer writes `+0xc4`. Mode 0 means "height-adjusted or
CAMERAHOOK anchors, standard/close-up construction"; mode nonzero would use raw origins, add heights
inside the evaluator, and use one (not three) angle-3 segment query.

**`+0x34` is a per-update close-up/query flag, not a persistent obstruction input.** Writers:
constructor `0x6bb701`=0, static camera `0x6bb59d`=0, `FUN_006bd260` reset `0x6bd2e2`=0,
coincident-anchor test `0x6bb811`=1, and post-query hit `0x6bd928`=1. Readers are only `0x6bbde1`,
`0x6bc098`, `0x6bc49f` inside `FUN_006bb7b0`; because `FUN_006bd260` resets the field at entry, the
`0x6bd928` value is not read before the next reset. The live meaning is "anchors coincident"
(close-up, range suppressed, and the mode-nonzero evaluator query gate).

**Obstruction segments.** `FUN_006bd260` queries the scene interface (`self+0x10` vtable `+0x104`,
query vtable `+0x38`) after the evaluator: angle 3 uses `0x6bd811`, `0x6bd86d`, `0x6bd8c2` (three
segments) when mode 0; all other procedural angles use `0x6bd911` (one). The mode-nonzero evaluator
adds one coincident-only query at `0x6bbfb4` (unreachable with mode 0). The angle-6 static path
(`FUN_0062c030` -> `FUN_006bb590` at `0x62c23f`) bypasses the evaluator and performs no query; the
CameraAnimation clip path (`FUN_0062bf70` -> `FUN_00641010` at `0x6320a1`/`0x62c00f`) also bypasses it
and reads neither `+0x34` nor `+0x38`.

Do not treat `0x62a970@0x62a9ce` as the camera mode writer: it zeroes controller `+0x38` in the
0xc24-byte dialogue controller, a different object. `FUN_0062aa00` writes that controller's `+0x34`
(1/2/3) and reads its `+0x38`; these are controller state fields, not the camera's.

## Still open

* Camera `+0x38` is statically zero for every shipped construction path. The live 1280×720 Taris
  probe confirmed the dialogue-controller constructor sets controller `+0xc4 = 0` (the source
  value) in-process. No dialogue camera was constructed in that non-dialogue scene, so the camera
  object's field itself was not observed live; a dialogue/cutscene scene is P1 verification. The
  mode-nonzero branches remain unreachable in shipped data.
* Supermodel visual behavior needs a synthetic child/super chain with duplicate, ancestor-only and
  missing clips; the code paths and supernode binding are verified, but visual output is not.
* aux4 is parsed, relocated, copied and freed but has no consumer; the GPU path uses qbone/tbone,
  so no aux4 decode remains (`docs/recon/15`).
* `MapResX` role and minimap texture semantics (inferred).
* Particle/light/animation-node controller types (36/76/80/84/88/100/120/124/132/176/216/224/232/
  236/240/284/380/392) are catalogued but not needed for skeletal pose; type 132 is a FrameEnd cue.
* `m01aa_07b` has no 545 node, so no WOK cross-check.

## Evidence

| dataset | contents |
|---|---|
| `misc2/mdl_anim.py`, `anim_decode.json` | controller layout, pose validation, supermodel/skin |
| `misc2/anim_payload_stats.json` | 120 models, 56,174 controllers, sentinel/column stats |
| `misc2/wok_position_verify.json` | 453/453 node-545 proof, rejected alternatives |
| `misc2/camera_are_dlg.json` | GIT formula, 106 ARE maps, 22 DLG camera models |
| `misc2/camera_aim_verify.json` | pitch convention aim test; DLG CameraID domain |
| `misc3/camanim_rule_test.json` | 1200+(N−1) → cutNNNw rule; 208/208 resolution |
| `misc3/camera_angle_stats.json`, `engine_evidence.json` | CameraAngle branches; line-cited pseudocode |
| `misc3/supermodel_remap.json` | 86/86 differing definitions; 6.9% index alignment |
| `cont/misc/*.json` | earlier Bearing/door/WOK/camera corpus data |
