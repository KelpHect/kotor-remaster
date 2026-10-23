# m01aa geometry preview (lane 09)

Status: **complete**. The first area (Endar Spire, `m01aa`) parses from LYT, WOK, MDL, PTH, IFO
and GIT and renders to a dependency-free top-down SVG. The preview is a validation instrument, not
an engine renderer.

## Method

`tools/geometry/probe.py` reads a flat area directory (`KOTOR_AREA_DIR`):

* `m01aa.ifo`, `m01aa.git`, `m01aa.pth` with the project GFF parser;
* `m01aa.lyt` room origins and door hooks;
* `<room>.wok` BWM V1.0 floor/walkmesh records;
* `<room>.mdl` node hierarchy and static transforms.

`tools/geometry/preview.py` draws world XY (+Y up), using WOK floor triangles and boundary edges,
with room labels at floor centroids, LYT origin crosses, door hooks, GIT doors/creatures/placeables/
waypoints, PTH nodes and links, and the IFO entry point/direction. It also cross-checks its own
output against `probe.py` (45 required values) and writes `stats.json` and `summary.md`.

Generated SVGs and stats contain game-derived geometry and stay under `artifacts/` (gitignored).
The tools take only a local path and perform no engine, Ghidra or network access.

## Verified counts and checks

| item | value |
|---|---:|
| LYT rooms | 15 |
| rooms with WOK floor | 14 |
| LYT door hooks | 15 |
| WOK vertices / triangles | 850 / 1,365 |
| upward-facing floor triangles drawn | 858 |
| GIT creatures / doors / waypoints / placeables | 26 / 15 / 58 / 60 |
| PTH points / directed links / undirected edges | 77 / 164 / 82 |
| PTH components | 1 |
| shared WOK vertex positions | 44 |

* Entry `(15.425, 20.122, -1.275)` lies on `M01aa_01a` face 55 with `dz = 0.0000`; direction is unit
  +Y. This repeats the coordinate-contract fixture.
* WOK winding: +1,365 / −0 / 0; minimum normal dot 0.0280.
* All 15 doors map to hooks (max 3D offset 51.004 mm); all 15 hooks map to MDL door nodes
  (max 0.731 mm).
* PTH has one component and no non-reciprocal edges.

## Discrepancies (recorded, not corrected)

* `end_door06` (room `M01aa_09`, node `Door_07`) is **51.004 mm** from its hook; every other door is
  within 6 µm. GIT coordinates remain authoritative for the instance.
* `wp_room8_02` is 0.642 m above the floor; `plc_pwrcond` and `end_explosion` are 0.120 m above.
  These are waypoint/placeable origin offsets, not floor errors.
* The MDL XY bounding box misses the WOK in six rooms (max 842.3 mm); model-header bboxes are not a
  geometry oracle (consistent with `docs/recon/04`).
* `M01aa_07b` has no WOK floor and is not drawn; `M01aa_08d` has no MDL trimesh.

## Reproduce

```bash
# KOTOR_AREA_DIR points at a flat directory extracted from the user's install
KOTOR_AREA_DIR=<area-dir> KOTOR_PREVIEW_OUT=<out-dir> python tools/geometry/preview.py
```

The preview closes the Sprint 0 geometry gate for m01aa. Engine-side rendering, map compilation,
and the full object/model pipeline remain P1/P4 work.
