# Recon 54 - procedural facial rig from LIP visemes

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Shipped data

`visemes.2da` has 16 rows. The shipped table is slightly non-standard: its
header is `viseme`, `animname`, `visemeid`, the row labels are `1`..`16`, and
the parsed cells arrive as `["Silence", "", "0"]` - `animname` is shifted into
the first data column. `FacialRig::load` handles this by falling back to
column 0 for the name and the row label - 1 for the id if `visemeid` is absent.

Rows: `Silence`, `Eat`, `If`, `Ox`, `Oat`, `Wet`, `Size`, `Church`, `Fave`,
`Though`, `Told`, `Bump`, `New`, `Roar`, `Cage`, `Earth`.

Heads are GFF-free MDL models with facial bones: `f_jaw_g`, `f_lmc_g`,
`f_rmc_g`, `f_um_g`, `f_lbrw_g`, `f_rbrw_g`, `tongue`, `Head`, `head_g`.
Across all **107** `heads.2da` rows (**106** parsed models) there are **0**
viseme-named animation clips and no morph-target arrays. The original facial
deformation is therefore not shipped as per-viseme clips; Sprint 13 applies a
procedural channel rig instead of inventing data.

## Channels and bones

| Viseme | jawOpen | mouthWide | lipRound | browRaise |
|---|---:|---:|---:|---:|
| Silence | 0.00 | 0.00 | 0.00 | 0.00 |
| Eat | 0.35 | 0.15 | 0.25 | 0.00 |
| If | 0.18 | 0.65 | 0.10 | 0.10 |
| Ox | 0.55 | -0.35 | 0.80 | 0.00 |
| Oat | 0.62 | -0.20 | 0.70 | 0.00 |
| Wet | 0.22 | 0.45 | 0.20 | 0.05 |
| Size | 0.12 | 0.75 | 0.10 | 0.15 |
| Church | 0.30 | 0.15 | 0.55 | 0.00 |
| Fave | 0.20 | 0.50 | 0.15 | 0.00 |
| Though | 0.32 | 0.25 | 0.30 | 0.00 |
| Told | 0.40 | -0.10 | 0.50 | 0.00 |
| Bump | 0.00 | 0.00 | 0.40 | 0.00 |
| New | 0.30 | -0.20 | 0.35 | 0.00 |
| Roar | 0.85 | 0.35 | 0.15 | 0.20 |
| Cage | 0.50 | 0.40 | 0.20 | 0.00 |
| Earth | 0.35 | 0.25 | 0.20 | 0.00 |

`FacialRig::rotations(id)` returns local rotations:

- `f_jaw_g`, `tongue`: X rotation from `jawOpen`;
- `f_lmc_g` / `f_rmc_g`: opposite Z rotations from `mouthWide`;
- `f_um_g`: X rotation from `lipRound`;
- `f_lbrw_g` / `f_rbrw_g`: X rotation from `browRaise`.

`Mdl::poseWorld` accepts extra local rotations and
`Mdl::posedTrianglesRigged` applies them before skinning. On `p_CarthH`, the
Roar viseme moves head vertices by **0.470906** model units; Silence produces
an exact bind-pose match (0.0).

## Runtime

`GameSession` owns a `FacialRig`, loads `visemes.2da` at startup, and each
update sets the viseme from `LipSync::visemeAt(lipTime)` while a dialogue LIP
is active (otherwise Silence). `--facial-demo <0-15>` injects a two-second
synthetic LIP and shows the rig panel; `docs/evidence/s13/facial-roar.png`
shows viseme 13 with a -26 degree jaw.

## Limits

The scene does not yet attach separate `heads.2da` models to creature bodies;
the rig is proven against the shipped head geometry and in the debug rig view.
Head attachment and per-vertex scene rendering are a separate renderer step.
