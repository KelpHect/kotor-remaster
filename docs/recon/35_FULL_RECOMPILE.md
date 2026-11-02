# 35 — Full-corpus source recompile (8,666 scripts)

Status: **closed** — 8666/8666 clean-room NSS files recompiled with per-file historical headers for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Results

| class | files | meaning |
|---|---:|---|
| raw_exact | 8665 | recompiled bytes equal the shipped NCS |
| compiler_limited | 0 | still needing a compiler change |
| header_limited | 0 | the 5 anomalous-action prototype cases |
| reachable_only | 1 | unreachable code that cannot be expressed in source |
| other | 0 | unexpected (gate failure) |

## Method

For every script the shipped NCS is scanned for ACTION calls whose arity is smaller than the current prototype. A per-file `nwscript.nss` is generated with the historical arity, the unchanged clean-room NSS is compiled with `nwnnsscomp_ktool`, and the result is compared byte-for-byte. Struct-recovered source uses the same per-file header rule. The three function-layout scripts are compiled with the historical nwnnsscomp rebuild (`tools/compiler`) at `-v129`, which discovers call arguments right-to-left as the shipped compiler did. Files without such calls use the full header.

The raw_exact count includes the 24 default-argument cases (`docs/recon/33_HISTORICAL_HEADERS.md`), the 5 header-limited overrides, all 201 struct recoveries (`docs/recon/36_STRUCT_RECOVERY.md`), the duplicate-return fixes (`docs/recon/37_RETURN_CLEANUP.md`), and the 3 body-order scripts (`docs/recon/38_BODY_ORDER.md`). No compiler-limited or ABI-completed file remains; only the 5 unreachable-code scripts are outside source-level recompilation.
