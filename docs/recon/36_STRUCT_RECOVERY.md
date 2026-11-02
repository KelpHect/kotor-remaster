# 36 — Source-level struct recovery for the 201 group-slice files

Status: **closed** - 201/201 struct globals recovered and **201/201 raw byte-identical** from unchanged clean-room source.

| check | result |
|---|---|
| Files processed | **201/201** |
| Structs recovered | **201/201** |
| Source-level byte-identical | **201/201** |
| Full struct ABI fallback | **0** |

`tools/scripts/return_cleanup.py` restores the compiler's duplicate trailing `return vN;` and uses a per-file historical prototype for the 12 files that call `ActionStartConversation` with the KotOR-era 11 arguments (`docs/recon/37_RETURN_CLEANUP.md`). No bytecode transform remains.
