# 39 - Unreachable statements after `return`

Status: **4/4 restored** - the four `k_pkas_news_*` scripts are byte-identical after restoring the statement the decompiler dropped. `k_plev_space_hb` remains the single documented residual.

| script | result |
|---|---|
| `k_pkas_news_01__f69d97f6c0170f98` | source_exact |
| `k_pkas_news_02__45dee6a6adbdc972` | source_exact |
| `k_pkas_news_03__4b34b22c12799199` | source_exact |
| `k_pkas_news_04__9e93320253a30ef8` | source_exact |
| `k_plev_space_hb__a2b5e977f40bbbce` | documented_residual |

The four news scripts ended with `return ...;` followed by an unreachable `SetGlobalNumber("tat_LinesChecked", 0);`. The shipped compiler still emitted that statement; the decompiler removed it. Restoring it compiles to the shipped bytes with no other change.

`k_plev_space_hb` is different: its discarded block reuses one stack slot as an `int` in the loop condition and as an `object` in the loop body, a shape NWScript's type checker rejects. It is reported as `reachable_only` in the full-corpus ledger.
