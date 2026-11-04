# Sprint 17 - Remaster stage 1

Status: **planned**. Sprint 16 produces the release candidate. Sprint 17 is
the first optional remaster stage; Classic remains the 1:1 oracle and stays
selectable at runtime.

## Gate S17

1. **High-res textures** - replace shipped textures at runtime from an external
   cache, preserving the material/TXI contract and the read-only install.
2. **HFR and widescreen** - presentation clock and aspect handling that do not
   change gameplay timing or cutscene camera composition.
3. **MSAA and anisotropy** - configurable on GL and Vulkan with deterministic
   fallback.
4. **SSAO and bloom** - screen-space effects that can be disabled independently
   and do not affect Classic captures.
5. **Parity** - Classic vs remaster captures documented; Classic GL/Vulkan
   parity remains the gate; no install writes.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s17-01-textures | external high-res texture cache | render/asset pipeline |
| s17-02-display | HFR/widescreen presentation | `engine/src/app/`, `render/` |
| s17-03-aa | MSAA/anisotropy settings | GL/Vulkan backends |
| s17-04-post | SSAO/bloom | render/ |
| s17-05-evidence | captures, budget, opt-out matrix | `docs/evidence/s17/` |

## Evidence carried into Sprint 17

- `docs/SPRINT13_REPORT.md` - Classic parity and install sentinel.
- `docs/SPRINT16.md` - release candidate build.
- `docs/adr/0001-runtime-and-renderer.md`, `docs/adr/0003-distribution-and-install-model.md`.
