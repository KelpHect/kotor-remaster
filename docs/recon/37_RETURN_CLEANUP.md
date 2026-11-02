# 37 — Source-level duplicate trailing-return cleanup

Status: **closed** - 201/201 struct files are raw byte-identical from unchanged clean-room source.

| check | result |
|---|---|
| Struct files processed | **201/201** |
| Source-level exact | **201/201** |
| Historical prototype files | **12** |
| Fallback | **0** |

Two source-level fixes are applied. First, a per-file header trims a prototype to the call-site arity when the shipped script omitted trailing default arguments (12 files call `ActionStartConversation` with 11 arguments while the KotOR header declares 12). Second, the compiler's duplicate trailing `return vN;` is restored: the shipped compiler emits two identical cleanup blocks and the decompiler kept one. No bytecode is edited.
