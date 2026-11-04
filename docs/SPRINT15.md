# Sprint 15 - Release candidate checks

Status: **planned**. Sprint 14 closes P6 correctness and the residual gaps.
Sprint 15 is the release-candidate gate: performance, input/accessibility,
legal review, installer/launcher and packaging. It adds no gameplay; it
certifies the feature-complete KotOR 1 build from Sprint 14. KotOR 2 gameplay
parity is deferred to the final sprint (Sprint 18).

## Why this is a separate sprint

These checks need a feature-complete build. They are verification and
packaging work, not gameplay changes.

## Gate S15

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
6. **Regression** - full CTest, 117-module K1 sweep, VM parity, GL/Vulkan
   parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s15-01-performance | budgets and profiling captures | `docs/evidence/s15/`, docs |
| s15-02-input | controller/accessibility matrix | `engine/src/app/`, docs |
| s15-03-legal | asset/licence/release review | `docs/LEGAL_REVIEW.md` |
| s15-04-installer | launcher, install validation, external cache | `tools/`, `docs/` |
| s15-05-package | release candidate build and packaging | build scripts, docs |
| s15-06-evidence | gate, report, logs | `docs/` |

## Evidence carried into Sprint 15

- `docs/SPRINT13_REPORT.md` - feature-complete runtime and gate baseline.
- `docs/SPRINT14.md` - P6 correctness and residual gaps.
- `docs/evidence/s13/p6-content.json` - content coverage.
- `docs/recon/02_TECHNICAL_NOTES.md` - ownership split and legal boundaries.
