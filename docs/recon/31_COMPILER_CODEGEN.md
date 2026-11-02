# 31 — Compiler-codegen reconciliation for the 228 residuals

Status: **closed** - 228/228 compiler-limited recompiles verified byte-identical for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (213 source-level, 15 ABI-reconciled).

## Gate

| check | result |
|---|---|
| Source-level byte-identical (headers + struct recovery) | **213** |
| ABI-reconciled byte-identical | **15/15** |
| Total byte-identical | **228/228** |
| Failures | **0** |

## Transforms

| transform | files | meaning |
|---|---:|---|
| source-level | 213 | historical prototype arity (24), `tLastRound` struct recovery (2), duplicate trailing-return cleanup (187) |
| struct-group | 12 | restore the historical `CPTOPBP size=40; DESTRUCT` group slice and the five-instruction cleanup block |
| body-order | 3 | reorder function bodies to the historical emitted-JSR address order |

## Method

- The 24 default-argument cases are compiled from the unchanged source with per-file historical prototype headers (`docs/recon/33_HISTORICAL_HEADERS.md`); the 201 struct cases are recovered at source level by `tools/scripts/struct_source.py` (`docs/recon/36_STRUCT_RECOVERY.md`).
- Of the 201 struct cases, 189 are source-level exact (2 pure struct recovery + 187 duplicate trailing-return cleanup fixes); 12 use the full struct-group ABI path because their control-flow cleanup shape differs.
- The raw ktool output is recorded and hashed per file; the reconciler never replaces it silently.
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
