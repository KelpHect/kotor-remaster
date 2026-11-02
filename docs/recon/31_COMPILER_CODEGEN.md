# 31 — Compiler-codegen reconciliation for the 228 residuals

Status: **closed** — 228/228 compiler-limited recompiles verified byte-identical for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Gate

| check | result |
|---|---|
| Compiler-limited files present | **228/228** |
| Reconciled byte-identical | **228/228** |
| Failures | **0** |

## Transforms

| transform | files | meaning |
|---|---:|---|
| struct-group | 201 | restore the historical `CPTOPBP size=40; DESTRUCT` group slice and the five-instruction cleanup block |
| default-args | 24 | strip ktool-materialized trailing defaults and restore the original `argc`/frame operands |
| body-order | 3 | reorder function bodies to the historical emitted-JSR address order |

## Method

- The raw ktool output is recorded and hashed per file; the reconciler never replaces it silently.
- The clean-room decompiler's source recovery is unchanged. The lane's 8,666-row `status.jsonl` remains the source-recovery ledger (8,428 exact, 0 decompiler-side open).
- The reconciler applies only the three named compiler-ABI transforms above and verifies the final NCS byte-for-byte against the shipped file.
- Every row in `codegen_reconcile.json` carries the original, raw-recompiled and reconciled SHA-256 values and the transform used.

## Reproduce

```bash
python -m tools.scripts.codegen_reconcile \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --recompiled artifacts/lanes/ncs-decompiler/cont2/out/ncs \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --out-json artifacts/lanes/s3-live/codegen_reconcile.json \
  --out-doc docs/recon/31_COMPILER_CODEGEN.md
```
