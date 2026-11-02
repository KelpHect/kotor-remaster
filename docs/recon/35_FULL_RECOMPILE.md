# 35 — Full-corpus source recompile (8,666 scripts)

Status: **closed** — 8666/8666 clean-room NSS files recompiled with per-file historical headers for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Results

| class | files | meaning |
|---|---:|---|
| raw_exact | 8646 | recompiled bytes equal the shipped NCS |
| compiler_limited | 15 | the 12 different-cleanup struct + 3 body-order cases |
| header_limited | 0 | the 5 anomalous-action prototype cases |
| reachable_only | 5 | the 5 unreachable-code cases |
| other | 0 | unexpected (gate failure) |

## Method

For every script the shipped NCS is scanned for ACTION calls whose arity is smaller than the current prototype. A per-file `nwscript.nss` is generated with the historical arity, the unchanged clean-room NSS is compiled with `nwnnsscomp_ktool`, and the result is compared byte-for-byte. Files without such calls use the full header.

The raw_exact count includes the 24 default-argument cases (`docs/recon/33_HISTORICAL_HEADERS.md`), the 5 header-limited overrides, the 2 pure struct recoveries (`docs/recon/36_STRUCT_RECOVERY.md`), and the 187 duplicate-return fixes (`docs/recon/37_RETURN_CLEANUP.md`). The 12 different-cleanup struct cases and 3 body-order cases remain the ABI group in `docs/recon/31_COMPILER_CODEGEN.md`.
