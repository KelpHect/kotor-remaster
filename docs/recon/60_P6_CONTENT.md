# Recon 60 - P6 all-area content checklist

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(`swkotor.exe` SHA-256 in the baseline).

## What the gate does

`engine/scripts/check_p6_content.py` runs
`kotor_engine --content-stats` for every KotOR 1 module, parses one JSON line
per module, and compares the result with the committed baseline
`engine/tests/fixtures/p6_content_baseline.json`.

Per module it records:

- area, area tag;
- rooms, vertices, indices, batches, textures;
- collision vertices/facets;
- entities split into doors, placeables, creatures, waypoints, stores,
  encounter entities, sound entities;
- entity meshes, trigger volumes, encounter volumes, cameras, sound emitters;
- script hooks, scripted entities, scene scripts, script runs, warnings.

The gate asserts:

1. exactly **117** modules, each with a non-empty area;
2. every module has at least one room, vertex, texture and entity;
3. every module has **0** loader warnings;
4. every per-module count matches the baseline exactly;
5. the aggregate totals match the baseline exactly;
6. every coverage category is non-zero;
7. the install executable hash matches the pinned KotOR 1 build.

`--write-baseline` regenerates the committed baseline after an intentional
engine/data change. Validation mode never writes the baseline.

## Shipped coverage

| category | total |
|---|---:|
| modules | 117 |
| unique areas | 106 |
| rooms | 1,472 |
| vertices / indices | 13,963,434 |
| texture batches | 3,509 |
| textures | 5,679 |
| collision vertices / facets | 116,231 / 190,865 |
| entities | 11,887 |
| doors | 724 |
| placeables | 3,214 |
| creatures | 1,699 |
| waypoints | 4,064 |
| stores | 36 |
| encounter entities / volumes | 51 / 51 |
| sound entities / emitters | 1,103 / 1,103 |
| entity meshes | 7,677 |
| triggers | 991 |
| cameras | 1,137 |
| script hooks / scripted entities | 20,497 / 3,487 |
| scene scripts | 355 |
| script runs during load | 1,749 |
| warnings | 0 |

Unique areas are 106 rather than 117 because several modules share an area
(e.g. `stunt_endbridge` covers four modules, `m12aa` four, `m40ad` three).
Three modules (`ebo_m40aa`, `tar_m03mg`, `tat_m17mg`) have no entity meshes;
their area geometry, entities and scripts are still covered.

## Why this is the P6 checklist core

The MASTER_PLAN P6 also lists differential playthroughs, performance budgets,
controller/accessibility and legal review. Those remain release-verification
tasks. This gate closes the *all-area content coverage* portion: every shipped
module is loaded, every content category is counted, and any missing room,
texture, entity, script or area fails deterministically.

## Evidence

- `docs/evidence/s13/p6-content.json` - full per-module report.
- `engine/tests/fixtures/p6_content_baseline.json` - committed baseline.
- CTest `kotor_p6_content` (about 21 s with six workers).
