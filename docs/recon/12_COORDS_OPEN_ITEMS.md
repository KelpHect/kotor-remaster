# Coordinate, animation, and camera findings (lane 10)

Status: **largely verified.** Bearing/door facing, `WOK.position`, animation controller payloads,
GIT/ARE camera-minimap fields, and DLG camera models are decoded. Remaining: DLG
`CameraAnimation`/`CameraAngle` composition and supermodel override/name-remap semantics (a
targeted continuation is running). MDX is in `docs/recon/15`. Data is under
`artifacts/lanes/10-coords-open/` (untracked); the install was not modified.

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
  **116/133 child/super pairs are clip-name disjoint (inherit).** 17/133 share names; all 71
  shared definitions differ, so nearest (child) override matters. The order and inherited-clip
  name-vs-index remap are still being closed (engine proof/runtime not yet observed).
* MDX skinning: skinned nodes carry a bone map (57–75 entries, −1 unused) indexing geometry nodes;
  weights/IDs are read from MDX at the skin header offsets (`mesh+332+12/+16`). Animation keys
  address the same node numbers.

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
the GIT `CameraID` domain. `CameraAngle` (0–6) and `CameraAnimation` (ms-scale cue) roles remain
being closed.

## Still open

* DLG `CameraAnimation` clip/time selection and `CameraAngle` composition (targeted continuation).
* Supermodel nearest-override order and inherited-clip name-vs-index remap (engine/runtime proof).
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
| `cont/misc/*.json` | earlier Bearing/door/WOK/camera corpus data |
