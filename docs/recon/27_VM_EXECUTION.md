# 27 — Independent NCS VM: full-corpus execution

Status: **closed** — 8,666/8,666 unique shipped NCS for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Gate S2 checks

| check | result |
|---|---|
| Unique NCS decoded from entry | **8,666/8,666** |
| Decode errors | **0** |
| VM faults | **0** |
| Unknown action ids | **0** |
| Invalid jump/STORESTATE targets | **0** |
| Action specs available | **772/772** |
| Gate | **closed** |

## Execution results

| status | files | meaning |
|---|---:|---|
| returned | 8,184 | reached RETN with an empty return stack |
| blocked | 472 | stopped at `ActionWait` (event-driven script) |
| budget | 10 | hit the 200,000-step ceiling (loop without a wait) |
| error | 0 | VM fault |
| decode_error | 0 | malformed NCS |

Decoded instructions: **4,703,036**; executed: **3,895,590**; ACTION calls: **306,116**; unique action ids used: **345**; maximum stack: **217** cells.

## Top actions by calls

| id | action | calls |
|---:|---|---:|
| 168 | GetTag | 108,285 |
| 200 | GetObjectByTag | 41,315 |
| 340 | GetNextItemInInventory | 38,215 |
| 7 | DelayCommand | 20,418 |
| 24 | GetArea | 16,224 |
| 94 | GetNextObjectInArea | 15,984 |
| 397 | GetBaseItemType | 15,227 |
| 155 | GetItemInSlot | 7,710 |
| 42 | GetIsObjectValid | 4,086 |
| 580 | GetGlobalNumber | 2,335 |
| 679 | GetLocalBoolean | 2,162 |
| 6 | AssignCommand | 1,908 |
| 548 | GetFirstPC | 1,765 |
| 176 | SetListenPattern | 1,504 |
| 578 | GetGlobalBoolean | 1,328 |
| 197 | GetWaypointByTag | 1,327 |
| 581 | SetGlobalNumber | 1,185 |
| 577 | GetPartyMemberByIndex | 1,126 |
| 681 | GetLocalNumber | 1,013 |
| 205 | ActionPauseConversation | 895 |

## Blocked / budget samples

`ActionWait` (id 202) stopped 472 event-loop scripts. The 10 budget stops are loops with no wait action; they are expected for scripts that poll engine state.

Budget stops:

- `k_man_com43__2fe941d24eb8414e.ncs` — 200,000 steps
- `k_pdan_fragvalid__d2b80032b5269dbe.ncs` — 200,000 steps
- `k_pdan_fragvalix__aa269d4db3968fbc.ncs` — 200,000 steps
- `k_pdan_mineset1__1fc6fb55b4a6ced4.ncs` — 200,000 steps
- `k_pman_minecheck__f8d2fdcff1b82d22.ncs` — 200,000 steps
- `k_psta_bastend__c8129360c18d5292.ncs` — 200,000 steps
- `k_psta_saber__4d2ed58838fec230.ncs` — 200,000 steps
- `k_ptat_minetake__ca011cb405e0a369.ncs` — 200,000 steps
- `k_sta_bastatt__4a0faa11ae492db5.ncs` — 200,000 steps
- `k_tat_compart__75a3b57002500a46.ncs` — 200,000 steps

## Method

- `tools/vm/ncs_vm.py` decodes the shipped NCS V1.0 stream with the independent `tools/scripts/ncs.py` codec and executes the stack machine from offset 0.
- Control-flow parity: every `JMP`/`JSR`/`JZ`/`JNZ` and `STORESTATE` target is checked against the decoded instruction offsets; the gate requires zero invalid targets.
- Stack offsets use `len(stack) + off/4`; every shipped `MOVSP` is negative and pops cells (699,919 occurrences checked). `JSR`/`RETN` use a separate return stack with the documented recursion limit 8. `SAVEBP`/`RESTOREBP` and `CPTOPBP`/`CPDOWNBP` implement globals; `DESTRUCT` uses the `(size, offset, keep)` cell counts.
- ACTION mediation uses the 772 typed specs from `action_specs.json`: arguments are popped by declared width (vector = 3 cells, functor = 0 cells, otherwise 1), and every non-void action returns a typed default. No engine code is called.
- A 200,000-instruction per-script budget bounds event loops; `ActionWait` is classified as `blocked` instead of spinning.

## Reproduce

```bash
python tools/analysis/action_specs.py \
  --actions artifacts/action_table.json \
  --nwscript artifacts/game-extract/nwscript.nss \
  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
  --out artifacts/lanes/s2-01-engine-map/action_specs.json
python tools/vm/corpus_run.py \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out-json artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \
  --out-doc docs/recon/27_VM_EXECUTION.md
```

