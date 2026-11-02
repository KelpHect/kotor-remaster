# 38 - Historical function-address assignment (3 body-order scripts)

Status: **closed** - 3/3 scripts recompile byte-identically with the rebuild that discovers call arguments right-to-left, as the shipped compiler did.

| script | result |
|---|---|
| `k_pend_chest02__8dc902eaa3ab1a42` | byte-identical |
| `k_ren_visionland__fa424c25c40c6a90` | byte-identical |
| `k_sup_galaxmap__e7c4bc307b1672b8` | byte-identical |

The clean-room NSS is unchanged. `tools/compiler/README.md` records the compiler rebuild (`GatherUsedArguments`); `-v129` selects the pre-1.30 KotOR rules that do not emit the extra `NOP`.
