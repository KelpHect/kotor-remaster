# 37 — Source-level duplicate trailing-return cleanup

Status: **closed for 189/201** — 189 struct files are raw byte-identical after restoring the compiler's duplicate trailing return; 12 use the full struct-group ABI path because their cleanup shape differs.

| check | result |
|---|---|
| Struct files processed | **201/201** |
| Source-level exact | **189/201** |
| Full struct ABI fallback | **12** |

The fix duplicates the last local `return vN;` in the function whose cleanup-block count is one short. The shipped compiler emits two identical cleanup blocks; the decompiler kept one. No bytecode is edited.
