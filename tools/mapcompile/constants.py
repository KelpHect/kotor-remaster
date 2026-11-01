"""Resource type ids, rule codes, and frozen validation tolerances.

Type ids verified by payload magic against the user's install (build
SHA-256 0290889e...74437e6): ARE=2012, IFO=2014, GIT=2023, WOK/BWM=2016,
2DA=2017, MDL=2002, LYT=3000 (#MAXLAYOUT text), VIS=3001 (room graph text),
PTH=3003, MDX=3008.  Sources: module RIMs hold ARE/GIT/IFO and PTH
(`<mod>.rim`, `<mod>_s.rim`); LYT lives in data/layouts.bif; VIS and
lightmaps in data/lightmaps.bif; WOK/MDL/MDX in data/models.bif (verified
via artifacts/analysis/coordinates2/extract/manifest.json).
"""

from __future__ import annotations

# --- resource type ids (verified by magic, see module docstring) ----------
T_ARE = 2012
T_IFO = 2014
T_BWM_WOK = 2016
T_2DA = 2017
T_GIT = 2023
T_MDL = 2002
T_LYT = 3000
T_VIS = 3001
T_PTH = 3003
T_MDX = 3008

IFO_RESREF = "module"

# --- GIT list labels (verified against shipped GIT roots) ----------------
GIT_LISTS = (
    "Creature List",
    "Door List",
    "Encounter List",
    "SoundList",
    "StoreList",
    "TriggerList",
    "WaypointList",
    "Placeable List",
    "CameraList",
)
# rule 6 anchor lists
ANCHOR_LISTS = (
    "Door List",
    "WaypointList",
    "TriggerList",
    "Encounter List",
    "CameraList",
)

# --- validator rules / error codes ----------------------------------------
R_WINDING = "R1_WINDING"          # WOK face winding vs RH Z-up convention
R_BOUNDS = "R2_BOUNDS"            # geometry vs ARE/IFO room/entry bounds
R_WALKMESH = "R3_WALKMESH"        # walkmesh connectivity (shared-edge BFS)
R_PTH = "R4_PTH"                  # path graph endpoints + connectivity
R_LIGHTMAP_UV = "R5_LIGHTMAP_UV"  # MDL mesh lightmap (uv1) in [0,1]
R_ANCHOR = "R6_ANCHOR"            # GIT anchors inside bounds / on walkmesh
R_LOAD = "R0_LOAD"                # resource load/parse failure

HARD_CODES = (R_WINDING, R_BOUNDS, R_WALKMESH, R_PTH, R_LIGHTMAP_UV, R_ANCHOR, R_LOAD)

# triage codes (reported, do not fail the build; known shipped outliers)
T_DOOR_HOOK_OFFSET = "T_DOOR_HOOK_OFFSET"   # GIT door vs LYT hook > 10 mm (known 51 mm outlier, recon/11)
T_ANCHOR_OFF_WALKMESH = "T_ANCHOR_OFF_WALKMESH"  # anchor inside bounds but not on walkmesh
T_ROOM_NO_WOK = "T_ROOM_NO_WOK"             # LYT room without a WOK (e.g. m01aa_07b, recon/11/12)
T_UV_SENTINEL = "T_UV_SENTINEL"             # 0xFEFFEEEE lightmap-UV sentinel (recon/15)
T_PTH_MULTI_COMPONENT = "T_PTH_MULTI_COMPONENT"  # >1 PTH component (triaged if shipped)
T_WINDING_DEGENERATE = "T_WINDING_DEGENERATE"   # zero-area WOK faces (shipped: 123 corpus-wide)
T_PTH_OFF_WALKMESH = "T_PTH_OFF_WALKMESH"       # PTH node >0.5 m off the walkmesh

UV_SENTINEL_BITS = 0xFEFFEEEE

WINDING_INVERT_EPS = 1e-9      # dot below -eps = inverted face (hard); |dot|~0 = degenerate (triage)

# --- frozen tolerances (coordinate contract; see docs/recon/19) -----------
# Source space is RH Z-up, 1 unit = 1 m (recon/04 T08/T10, ADR 0002 §4.2).
WINDING_DOT_MIN = 1e-12        # cross(v1-v0,v2-v0).normal must exceed this
DEGENERATE_AREA = 1e-12        # face area below this is degenerate
BOUNDS_TOL = 0.5               # m, slack when testing "inside area bounds"
ENTRY_FLOOR_DZ = 1.0           # m, IFO entry z vs walkmesh floor (m12ab ships dz=0.70)
ON_WALKMESH_TOL = 0.05         # m, XY distance from anchor to a walkable face
HOOK_TOL_TRIAGE = 0.010        # m, door-hook offset above this is triaged
HOOK_TOL_HARD = 1.0            # m, no LYT hook this close = misplaced door
PTH_POINT_WALK_TOL = 0.5       # m, PTH node XY distance to walkmesh
UV_EPS = 1e-6                  # lightmap uv slack around [0,1]
