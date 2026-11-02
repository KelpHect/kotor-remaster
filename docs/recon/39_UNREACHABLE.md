# 39 - Unreachable statements after `return`

Status: **closed** - 5/5 scripts are byte-identical after restoring the unreachable statements the decompiler dropped.

| script | result |
|---|---|
| `k_pkas_news_01__f69d97f6c0170f98` | source_exact |
| `k_pkas_news_02__45dee6a6adbdc972` | source_exact |
| `k_pkas_news_03__4b34b22c12799199` | source_exact |
| `k_pkas_news_04__9e93320253a30ef8` | source_exact |
| `k_plev_space_hb__a2b5e977f40bbbce` | source_exact |

The four news scripts ended with `return ...;` followed by an unreachable `SetGlobalNumber("tat_LinesChecked", 0);`. The shipped compiler still emitted that statement; the decompiler removed it. Restoring it compiles to the shipped bytes with no other change.

`k_plev_space_hb` discards a complete party-damage loop after `return;`. Restoring `v1` (the spacewalk counter), `v2` (the party index), and `v3` (the party member) reproduces the shipped block exactly; the source is type-correct and needs no compiler extension.
