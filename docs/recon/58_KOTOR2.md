# Recon 58 - KotOR 2 profile-gated world loading

Status: Sprint 13 implementation note. Target install:
`E:\SteamLibrary\steamapps\common\Knights of the Old Republic II`,
`swkotor2.exe` SHA-256
`6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef`.

## Profile

`GameProfile::detect` already recognised `swkotor2.exe`; `supportsWorldLoad`
now returns true only for a recognised KotOR 1 or KotOR 2 build.
`supportsOriginalSaves` remains KotOR 1 only (K2 saves are not round-tripped).

## MDL/MDX layout difference

KotOR 2 keeps the K1 MDL container but changes the geometry token and the
trimesh header:

| Field | K1 | K2 |
|---|---|---|
| geometry id at file `0x0C` | `0x413670` | `0x416310` |
| trimesh header size (after the 80-byte node header) | 332 | 340 |
| MDX vertex-data offset | trimesh `+324` | trimesh `+332` |
| skin/dangly/saber sub-header base | trimesh `+332` | trimesh `+340` |

Everything else stays on the K1 offsets: root node at `0x34`, animation
table at `0x64/0x68`, name-offset table at `0xC4`, name count at `0xC8/0xCC`,
node header 80 bytes, stride at trimesh `+252`, vertex count at `+304`.
`Mdl::parse` accepts both geometry ids, records `k2_`, and selects the
trimesh base/offset. An unknown token is a hard format error.

The K2 `001EBO2` room proves the layout: header stride **32**, **74**
vertices, MDX **86,896** bytes and vertex offset **84,496**; `84,496 + 74*32 =
86,864` fits the MDX. The old K1-offset read produced the bogus
`1116097253` offset and failed at `range ... exceeds 86896`.

## Resource layout

`Install::bindModule` now also mounts `modules/<stem>_dlg.erf` in pass 4.
K2 splits dialogue out of the area RIM: `001EBO_dlg.erf` carries **47** DLG
resources (e.g. `3cfd`, 97,918 bytes), while `001EBO.rim` carries only
ARE/GIT/IFO and `001EBO_s.rim` carries scripts, templates, sounds and a PTH.
The LYT comes from `Layouts.bif`; room models/textures come from the BIF set,
exactly like K1.

## Verified areas

`--k2-test` loads three shipped areas through the public path:

| Module | Area | Rooms | Scene vertices |
|---|---|---:|---:|
| `001EBO` | `001ebo` | 18 | 240,252 |
| `007EBO` | `003ebo` | 16 | 196,581 |
| `002EBO` | `002ebo` | 2 | 42,081 |

`001EBO` through `GameSession` has **175 entities** (11 doors, 43 placeables,
2 creatures, 24 waypoints), **70 entity meshes**, **17 triggers**, **55
texture batches**, 284 script hooks and **0 warnings**. `001EBO2` parses to
921 triangles with finite, room-scale bounds (`maxAbs=7.38929`).

## Gate

`--k2-test` asserts profile kind/hash/`supportsWorldLoad`, K1-only saves, the
K2 trimesh layout, room geometry finiteness/bounds, the `_dlg.erf` layer, and
the `001EBO` scene counts plus the two extra areas. CTest target
`kotor_world_k2` runs when `KOTOR2_TEST_INSTALL` (or the default path) has
`swkotor2.exe`. Evidence: `docs/evidence/s13/k2.log`.

## Limits

The K1 117-module sweep remains the K1 gate; K2 is covered by the three-area
gate plus the profile test. K2-exclusive systems (influence, crafting,
prestige classes) are content/rules work outside world loading.
