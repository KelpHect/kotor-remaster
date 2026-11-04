# Sprint 16 - Release candidate checks

Status: **planned**. Sprint 14 closes P6 correctness; Sprint 15 closes K2
gameplay parity. Sprint 16 is the release-candidate gate: performance,
input/accessibility, legal review, installer/launcher and packaging.

## Why this is a separate sprint

These checks need a feature-complete build. They are verification and
packaging work, not gameplay changes.

## Gate S16

1. **Performance budgets** - frame time, memory, streaming and load times on
   the reference machine and the documented minimum tier; capture methodology
   and evidence format follow the project performance rules (calm baseline,
   cited captures, no inferred numbers).
2. **Controller and accessibility** - gamepad mapping, rebinding, subtitles,
   UI scaling and keyboard-only navigation.
3. **Legal review** - `docs/LEGAL_REVIEW.md` closes asset ownership,
   no-redistribution policy, FFmpeg runtime packaging/licensing and the
   release checklist.
4. **Installer/launcher** - points at a user-owned install, validates the data
   set, builds the cache outside the install and never modifies or
   redistributes game files (ADR 0003).
5. **Release candidate** - version pinning, reproducible build, packaged
   runtime DLLs/shaders, docs and a clean install sentinel.
6. **Regression** - full CTest, 117-module K1 sweep, K2 sweep, VM parity,
   GL/Vulkan parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s16-01-performance | budgets and profiling captures | `docs/evidence/s16/`, docs |
| s16-02-input | controller/accessibility matrix | `engine/src/app/`, docs |
| s16-03-legal | asset/licence/release review | `docs/LEGAL_REVIEW.md` |
| s16-04-installer | launcher, install validation, external cache | `tools/`, `docs/` |
| s16-05-package | release candidate build and packaging | build scripts, docs |
| s16-06-evidence | gate, report, logs | `docs/` |

## Evidence carried into Sprint 16

- `docs/SPRINT13_REPORT.md` - feature-complete runtime and gate baseline.
- `docs/SPRINT14.md`, `docs/SPRINT15.md` - P6 correctness and K2 gameplay.
- `docs/evidence/s13/p6-content.json` - content coverage.
- `docs/recon/02_TECHNICAL_NOTES.md` - ownership split and legal boundaries.
