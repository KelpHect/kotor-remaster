# Coordinate open items (lane 10)

Status: **continued and largely resolved.** The original lane produced the corpus datasets; a
continuation resolved the Bearing sign (RH), door model selection/facing, `WOK.position`, and the
animation/supermodel header survey. MDX block layout is handled in a separate continuation and
animation controller payloads remain open. Data lives under `artifacts/lanes/10-coords-open/`
(untracked); nothing from the install was modified.

## Corpus

106 areas scanned (GIT/LYT/IFO/ARE plus archive identity): 1,546 creatures, 704 doors,
3,138 placeables, 3,970 waypoints, 1,099 cameras. Bearing evidence covers 699 doors and 5 chairs;
use-point evidence covers 312 placeables; the stricter oracle covers 1,196 placeables with a PWK.

## Bearing and door facing (RH verified in engine code and by data oracle)

* `Bearing` is a signed radian yaw in `[-π, π]`; 206 doors have `Bearing = 0` (205 of their hooks
  are also 0). There is no separate zero sentinel.
* `Bearing mod π` matches the LYT hook yaw plane for all 699 doors (max deviation
  `1.55e-4 rad`). Among 345 doors with a well-conditioned hook angle, 341 have `Bearing ≈ θ` and
  4 have `Bearing ≈ θ ± π`; the 9 exact +π cases are front/back-symmetric door models.
* **Handedness is RH.** For 1,196 placeables with a PWK, the use point was rotated by `R_z(+B)`
  and by the mirrored yaw and tested against every area floor triangle. Ten cases are decisive
  for RH and **zero for LH**. Strongest: `m28aa` `plc_lockerlg`, `B = −π/2`; the RH use point is
  0.727 m inside a floor triangle, while the LH point is 0.484 m outside every floor (through the
  wall). This independently verifies `R_z(+Bearing)` under the invariant that a use point must lie
  on walkable floor.
* **Engine code proof.** Door loader `0x50a0e0` and placeable loader `0x50a7b0` read GIT `Bearing`
  B (radians), build `q = (cos B/2, 0, 0, sin B/2)` about +Z at `0x4ac960`, compute
  `forward = (−sin B, cos B, 0)` at `0x5853c0` / `0x5896e0`, and store it at object `+0x9c` via
  `0x4cd170`. The save path inverts with `atan2(−fx, fy)` (`0x4aa0f0`, `0x586a70`), and creatures
  carry `X/Y/ZOrientation` directly (`0x4c8c70`). Status: verified from instructions, not only from
  the oracle. Details: `artifacts/lanes/p0-classify/cont2/bearing_code_proof.md`.
* **Door model selection is UTD `GenericType`, not `Appearance`.** The 358 shipped door UTDs all
  have `Appearance = 0`; they carry 48 distinct `GenericType` values that index
  `genericdoors.2da`. All 48 reachable rows resolve to shipped `DOR_*` models (47 are actually
  used), each with an MDL and three DWK states. Row 0 maps to `T_DOOR01`, which is absent from
  the install, so an `Appearance`-based lookup would be wrong.
* **The model contributes no yaw or translation.** All 47 used door models have identity root
  nodes, and all 47 closed-state DWKs are X-wide/Y-thin. The instance transform is therefore:

  ```
  world_door = T_GIT(X,Y,Z) · R_z(Bearing) · M_local(model/DWK)
  ```

## `WOK.position` (verified unused for room placement; pivot for DWK/PWK)

Across 936 room WOKs, `position` matches the local AABB centre only 4/936 within 1 mm
(116/936 within 1 cm), the vertex centroid 9/936, and is zero in 7/936; the median max-axis
deviation is 9.58 m (p90 132 m, max 424.5 m). It is not a centroid, LYT origin, or consistent
local frame. Room WOK vertices are already world-space and the verified door chain has no
`position` term, so it is **unused for placement**.

For sub-walkmeshes it is a pivot: 246/278 DWK/PWK resources have a non-zero `position`; a
two-leaf door (`dor_lhr01`) has closed `position=(0,0,0)` and both opening leaves at
`(-2.785, 0.052, 0)`, exactly the closed-state X half-extent. Why room WOKs use inconsistent
authoring frames remains open.

## Animation / supermodel (header survey verified; override rule inferred)

2,832 MDLs parse with zero errors: 375 name a supermodel, 1,027 have animations, 146 have both.
All 2,832 root nodes are identity bind poses, so the instance origin is never displaced by bind
pose or animation scale. Supermodel chains reach depth 7; 133/146 both-models resolve to a shipped
supermodel (13 name missing models).

Worked example: `m01aa_c01_char01` (242 nodes, 18 clips, scale 1.06) → `S_Male02` (164 nodes,
166 clips) → `S_Male01` (82 nodes, 12 clips) → `NULL`. The child's 18 clip names are disjoint from
`S_Male02`; it inherits the parent's set and adds its own. Of 133 resolved child/super pairs, 17
share at least one clip name (nearest definition overrides) and 116 are disjoint (inherit).

Clips key a subset of geometry node indices; controllers are present (type 20 position, type 8
orientation). The structural contract is: bind pose supplies every local transform; a clip
replaces the keyed nodes' local transforms; untouched nodes keep bind pose; world transforms
compose parent→child as `T·R`. Controller keyframe payloads (including the `NaN,NaN,-0` first
key) and MDX-backed tracks are not decoded, so this rule is inferred, not frame-proven.

## Camera (verified field roles, outliers recorded)

1,099 GIT cameras across 106 areas:

* `Position` behaves as a ground point: 1,096/1,099 are within 1 mm of a WOK floor, 1,097 within
  5 mm; all 1,099 are over a floor within 0.5 m.
* `Height` is an offset: `eye = Position.z + Height` is the natural model. Median height 1.55 m,
  range −1.95 to 22.9 m. Two cameras have negative height (`m38ab` -1.95, `m45ac` -0.05).
* `Pitch` is non-negative degrees (range 10.4–144.7; 90 is the mode), and `FOV` is degrees (55 is the
  mode, 745/1,099; others 35/45/60/65/75).
* 141 cameras carry a ceiling value; 3 (`m26ad` 5/6/7) place the eye above that ceiling, and 2
  (`m13aa` 28/30) place `Position.z` above the floor. These are special-case cameras; do not clamp
  height or snap position generally.

## ARE `NorthAxis` (resolved: absent)

`NorthAxis` is absent (`None`) in all 106 shipped ARE files. The minimap orientation must be derived
from the ARE `Map` data or another field, not `NorthAxis`. `CameraStyle` is present (0 for `m01aa`).

## Still open

* **MDX vertex/face blocks** — a separate continuation is decoding the layout and payloads.
* **Animation controller payloads** — type 20/8 field semantics, the first-key sentinel, and
  MDX-backed tracks are undecoded; the override rule is structural only.
* **Room `WOK.position` authoring frame** — pivot semantics are verified for DWK/PWK; why room
  values differ by area is not.
* **None for Bearing sign** — `R_z(+B)` is proven in engine code (`0x50a0e0`/`0x50a7b0` →
  `0x4ac960` → `0x5853c0`/`0x5896e0`) and by the PWK oracle. Keep the sign as one versioned
  constant in the new engine.
* **DLG/GIT camera transforms and the minimap transform** — not covered here.

## Evidence

| dataset | contents |
|---|---|
| `bearing.json`, `pwk_vote.json` | 699 door `B/θ/Δ/Δ mod π`; 312 use-point votes |
| `cont/misc/bearing_oracle.json` | 1,196 PWK use-point RH/LH floor tests; 10 RH-decisive |
| `cont/misc/door_appearance.json` | UTD `GenericType` → `genericdoors` → model/DWK mapping |
| `cont/misc/wok_position.json` | 936 room WOKs vs centroid/AABB; DWK pivot examples |
| `cont/misc/anims_survey.json` | 2,832 MDLs, supermodel chains, clip-name collisions |
| `cameras.json`, `areas.json` | 1,099 cameras; 106 areas with GIT/LYT/IFO/ARE |
