# Sprint 18 - Remaster stages 2-4, modding and platforms

Status: **planned**. Sprint 17 completed the first remaster stage. Sprint 18
finishes the optional remaster path and the public modding/platform surface.

## Gate S18

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
6. **Regression** - Classic/remaster toggles, CTest, K1/K2 sweeps, VM parity,
   GL/Vulkan parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s18-01-shadows | baked/dynamic shadow path | render/ |
| s18-02-rt | optional RT with raster fallback | render/ |
| s18-03-upscale | FSR-class upscaling | render/ |
| s18-04-modding | modding API and docs | docs/, public headers |
| s18-05-platforms | additional platform builds | build/ |
| s18-06-evidence | captures, matrices, release notes | `docs/evidence/s18/` |

## Evidence carried into Sprint 18

- `docs/SPRINT17.md` - remaster stage 1.
- `docs/SPRINT16.md` - release candidate and legal review.
- `docs/adr/0001-runtime-and-renderer.md`.
