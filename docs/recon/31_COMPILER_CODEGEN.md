# 31 — Compiler-codegen reconciliation for the 228 residuals

Status: **closed** - 228/228 compiler-limited recompiles verified byte-identical for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (228 source-level, 0 ABI-reconciled).

## Gate

| check | result |
|---|---|
| Source-level byte-identical (verified bytes) | **228/228** |
| ABI-reconciled byte-identical | **0** (retired) |
| Total byte-identical | **228/228** |
| Failures | **0** |

## Transforms

| transform | files | meaning |
|---|---:|---|
| source-level | 228 | all 228 compile from unchanged clean-room source; no bytecode transform is needed |
| struct-group | 0 | retired (was 12) |
| body-order | 0 | retired (was 3) |

## Method

- The 24 default-argument cases are compiled from the unchanged source with per-file historical prototype headers (`docs/recon/33_HISTORICAL_HEADERS.md`); the 201 struct cases are recovered at source level by `tools/scripts/struct_source.py` (`docs/recon/36_STRUCT_RECOVERY.md`).
- All 201 struct cases are now source-level exact: 2 pure struct recovery, 187 duplicate trailing-return fixes, and 12 historical `ActionStartConversation` prototypes (`docs/recon/37_RETURN_CLEANUP.md`).
- The 3 body-order cases compile with the historical nwnnsscomp rebuild (`tools/compiler`), which discovers call arguments right-to-left (`docs/recon/38_BODY_ORDER.md`).
- The raw compiler output is recorded and hashed per file; the reconciler never replaces it silently.
- The clean-room decompiler's source recovery is unchanged. The lane's 8,666-row `status.jsonl` remains the source-recovery ledger (8,428 exact, 0 decompiler-side open).
- The reconciler applies only the named compiler-ABI transforms above and verifies the final NCS byte-for-byte against the shipped file.
- Every row in `codegen_reconcile.json` carries the original, raw-recompiled and reconciled SHA-256 values and the transform used.

## Reproduce

```bash
python -m tools.scripts.codegen_reconcile \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --recompiled artifacts/lanes/ncs-decompiler/cont2/out/ncs \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --source-exact artifacts/lanes/s3-live/historical_headers.json \
  --out-json artifacts/lanes/s3-live/codegen_reconcile.json \
  --out-doc docs/recon/31_COMPILER_CODEGEN.md
```
