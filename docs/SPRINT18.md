# Sprint 18 - KotOR 2 gameplay parity (final)

Status: **planned, final sprint**. Sprint 13 closed KotOR 2 world loading
(profile detection, K2 MDL/MDX geometry, `_dlg.erf`, three shipped areas).
KotOR 2 gameplay is deliberately scheduled last, after P6 correctness, the
release candidate and the remaster path. Sprint 18 adds the K2 gameplay
layers that world loading does not cover.

## Why this is the final sprint

K2 uses the same data layer with different rules and content: prestige classes,
influence, crafting/upgrades and K2-only script actions. Those are rules and
content work, not resource loading, and they are scoped after everything else
is done.

## Gate S18

1. **K2 rules** - classes, feats, powers, skills and 2DA differences load from
   the K2 install; character creation and level-up produce the K2 tables.
2. **Influence** - companion influence values, dialogue gating, influence
   rewards and the influence cap behave deterministically.
3. **Crafting and upgrades** - workbench flows, upgrade item slots, component
   costs and crafted-item stats use the K2 2DA data.
4. **K2 scripts/actions** - the K2 action set used by the shipped module
   scripts executes through the VM; unimplemented K2-only actions are
   enumerated and gated.
5. **K2 module sweep** - every shipped K2 module pair loads with zero scene
   warnings; the sweep records areas, entities, meshes and script runs.
6. **Regression** - all K1 gates remain green; K2 world tests, CTest,
   GL/Vulkan parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s18-01-rules | K2 classes/feats/powers/skills | `engine/src/game/Rules.*` |
| s18-02-influence | influence state, dialogue gates, rewards | `engine/src/game/GameSession.*`, script bridge |
| s18-03-crafting | workbench/upgrade slots and costs | `engine/src/game/`, `engine/src/kotor/` |
| s18-04-actions | K2-only action coverage | `engine/src/script/`, `ActionTable` |
| s18-05-sweep | all-module K2 sweep + gate | `engine/scripts/`, `engine/CMakeLists.txt` |
| s18-06-evidence | gate, report, recon notes, logs | `docs/` |

## Evidence carried into Sprint 18

- `docs/evidence/s13/k2.log`, `docs/recon/58_KOTOR2.md` - K2 profile and
  geometry layout.
- `docs/recon/59_FACTIONS.md` - faction model reused by K2.
- `docs/SPRINT13_REPORT.md` - K1 regression baseline.
- `docs/SPRINT17.md` - completed remaster/platform surface.
