# 36 — Source-level struct recovery for the 201 group-slice files

Status: **struct recovery complete** - 201/201 struct globals recovered; **189/201 are raw byte-identical** (2 pure struct recovery + 187 duplicate trailing-return fixes); 12 use the full struct ABI because their cleanup shape differs.

| check | result |
|---|---|
| Files processed | **201/201** |
| Structs recovered | **201/201** |
| Source-level byte-identical | **189/201** |
| Full struct ABI fallback | **12** |

`tools/scripts/return_cleanup.py` restores the compiler's duplicate trailing `return vN;` for the 187 files (`docs/recon/37_RETURN_CLEANUP.md`). The 12 remaining files have a different five-instruction cleanup shape and are completed by the full struct-group path in `docs/recon/31_COMPILER_CODEGEN.md`.
