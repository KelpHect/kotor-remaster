# Coordinate open items (lane 10)

Status: **data collected, findings derived here.** Lane 10 reached its turn limit before writing its
own report; the JSON datasets it produced are complete enough to resolve the Bearing sign, camera
height model, and ARE `NorthAxis` question. MDX blocks and animation/supermodel semantics remain
open. Data lives under `artifacts/lanes/10-coords-open/` (untracked); nothing from the install was
modified.

## Corpus

106 areas scanned (GIT/LYT/IFO/ARE plus archive identity): 1,546 creatures, 704 doors,
3,138 placeables, 3,970 waypoints, 1,099 cameras. Bearing evidence covers 699 doors and 5 chairs;
use-point evidence covers 312 placeables.

## Bearing (verified convention, inferred handedness)

* `Bearing` is a scalar radian yaw in `[-π, π]`; 206 doors have `Bearing = 0` (205 of their hooks are
  also 0). There is no separate zero sentinel.
* `Bearing mod π` matches the LYT hook yaw plane for all 699 doors (max deviation
  `1.55e-4 rad`). Bearing and the hook use the same yaw sign: among 345 doors with a
  well-conditioned hook angle (`0.35 < |θ| < 2.79`), 341 have `Bearing ≈ θ` and 4 have
  `Bearing ≈ θ ± π`.
* Handedness remains **inferred**, not proven by an asymmetric oracle. Two independent votes:
  * Placeable use points (312): right-handed `forward = (−sin B, cos B)` places the use point inside
    a floor for 304/312; left-handed does 285/312. The decisive asymmetric cases are 20 RH-only vs
    1 LH-only (284 both, 7 neither); the RH/LH dot-product score favors RH 125 to 75.
  * Five `plc_chair2` instances against known walls: 3 favor RH, 2 favor LH (weak).
* Practical contract: use GIT `Bearing` for the instance, not the hook facing; treat the model as
  RH, +Y-forward with +X width, and keep a sign switch as a single versioned constant if a runtime
  oracle later disproves it.

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

* **MDX vertex/face block layout** — rendering only; not decoded.
* **Animation/supermodel bind-pose overrides** — lane 10's animation survey did not finish; node
  transforms used so far are the static bind pose.
* **Door appearance → DWK/model final facing** — appearance ID mapping and its interaction with
  Bearing are not measured.
* **`WOK.position`** — a centroid/pivot hint; still unused and unverified.
* **Bearing handedness** — RH is inferred from two votes; an asymmetric runtime oracle would close
  it (for example, a creature/placeable whose use point or animation faces a known target).

## Evidence

| dataset | contents |
|---|---|
| `bearing.json` | 699 door `B/θ/Δ/Δ mod π`; 5 chair RH/LH scores |
| `pwk_vote.json` | 312 use-point RH/LH dot products and floor votes |
| `cameras.json` | 1,099 cameras with floor/ceiling, `dz`, height, pitch, FOV, quaternion |
| `areas.json` | 106 areas: GIT object lists, LYT rooms, IFO entry, ARE fields |
