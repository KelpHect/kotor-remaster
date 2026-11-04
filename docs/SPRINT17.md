# Sprint 17 - Remaster stages 2-4, modding and platforms

Status: **planned**. Sprint 16 completed the first remaster stage. Sprint 17
finishes the optional remaster path and the public modding/platform surface.
KotOR 2 gameplay parity remains the final sprint (Sprint 18).

## Gate S17

1. **Shadows/AO** - re-baked static shadows/AO and dynamic shadow maps, with
   raster fallback and Classic parity preserved.
2. **Optional RT** - opt-in RT shadows/reflections on capable hardware with a
   full raster fallback.
3. **Upscaling** - FSR-class upscaling using depth/motion vectors, with
   deterministic quality presets.
4. **Modding API** - documented data/mod hooks that do not expose copied
   engine code or game assets.
5. **Platforms** - documented Windows baseline plus the agreed additional
   target(s), with install validation and input parity.
6. **Regression** - Classic/remaster toggles, CTest, K1 sweeps, VM parity,
   GL/Vulkan parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s17-01-shadows | baked/dynamic shadow path | render/ |
| s17-02-rt | optional RT with raster fallback | render/ |
| s17-03-upscale | FSR-class upscaling | render/ |
| s17-04-modding | modding API and docs | docs/, public headers |
| s17-05-platforms | additional platform builds | build/ |
| s17-06-evidence | captures, matrices, release notes | `docs/evidence/s17/` |

## Evidence carried into Sprint 17

- `docs/SPRINT16.md` - remaster stage 1.
- `docs/SPRINT15.md` - release candidate and legal review.
- `docs/adr/0001-runtime-and-renderer.md`.
