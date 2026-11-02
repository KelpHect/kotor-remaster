# 32 — Live ExecuteCode / action-dispatch differential (gate S3, P3/G3)

Status: **closed** — 28/28 unique live script/command programs replay exactly in the
independent VM for build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Engine anchors

| item | address | evidence |
|---|---|---|
| `ExecuteCode` | `0x005d2bd0` | thiscall `(vm, int* ip, byte* program, int length)`; VM fields `+0x1ac` SP, `+0x1b0` BP, `+0x1b8` type array, `+0x1bc` value array |
| action dispatcher | `0x0052c0d0` | action-table vtable `0x007450e4` slot `+8`; calls `handler(id, arg)` or returns `-2002` |
| VM vtable | `0x0074c4fc` | first live `ExecuteCode` object; one virtual slot `0x005d4c10` |
| action-table initializer | `0x0054c960` | allocates `0xC10` = 772 × 4 and writes the table |

The engine program pointer excludes the 13-byte NCS header; the replay wraps the captured
bytes before decoding. Action IPs are body-relative (header-excluded), matching the VM.

## Harness

`tools/harness/live_hooks.js` now captures, per `ExecuteCode` call:

- program pointer, length, and the full instruction bytes for every unique program;
- entry IP, SP, BP, and the full entry stack (types + values) up to 1,024 cells;
- before every ACTION dispatch: action id, IP, SP, BP, and the top 12 stack cells;
- after the dispatch: `sp_after`, the top 6 cells, the raw return value, and decoded strings;
- string cells through the live `{char* data; int length}` layout, with the NUL terminator
  stripped (`"carth\0"` → `"carth"`).

`tools/vm/live_diff.py` seeds the VM with the engine's entry IP/SP/BP and entry stack, injects
the engine's return cells at each ACTION, and compares, action by action:

1. action id;
2. action IP;
3. SP before dispatch;
4. BP before dispatch;
5. SP after dispatch;
6. post-action stack depth after injecting the engine's return cells (vectors 3 cells,
   void 0, everything else 1).

A trace matches only when the complete action sequence and every compared cell agree.

## Canonical run

Run `mesa-vmdiff7-20261102` (Mesa llvmpipe, `Disable Movies=1`, Endar Spire start):

| metric | value |
|---|---:|
| `ExecuteCode` calls captured | 400 (hook limit) |
| ACTION dispatches captured | 12,000 (hook limit) |
| unique programs / command bodies | **28** |
| matched exactly | **28 / 28** |
| mismatched | **0** |
| fresh script bodies (entry IP 0, SP 0) | 21 |
| resumed / command bodies (nonzero entry IP or stack) | 7 |
| distinct ACTION events across the unique programs | 363 |

Results: `artifacts/lanes/s3-live/live_diff7.json`; raw events:
`artifacts/lanes/s1-06-dynamic/runs/mesa-vmdiff7-20261102/events.jsonl`.

The seven resumed/command traces include small 55-, 78-, 129-, 213- and 327-byte programs with
nonzero entry IPs or a pre-existing stack/BP; they replay exactly with the captured entry stack.
The remaining 21 are fresh 55–11,643-byte script bodies (including the 6,174-, 8,213-, 8,293-,
11,603-, 11,623- and 11,643-byte scripts).

## RunCommand (queued execution)

`FUN_005d45d0` (`0x005d45d0`) is the engine's queued-command runner: it pushes a script frame,
stores the command arguments, and calls `FUN_005d4270` (`0x005d4270`), which calls
`ExecuteCode` (`0x005d2bd0`).  A dedicated hook on `FUN_005d45d0` recorded:

| metric | value |
|---|---:|
| `run_command` calls captured | 4,000 (hook limit) |
| `run_command` exits | 4,408 |
| paired `ExecuteCode` programs | 400 |
| unique programs replayed | 30 |
| matched exactly | **30 / 30** |

The run-command capture (`mesa-runcommand-20261102`) replays the same 30 unique programs,
including the small command bodies, with exact action id/IP/SP/BP/`sp_after` agreement:
`artifacts/lanes/s3-live/live_diff_runcommand.json`.

The `run_command` hook records the command/situation pointers and the two argument slots; the
paired `vm_exec` events carry the program bytes and entry state.  The dispatcher hook remains the
shared action entry point, so both the direct script bodies and the queued-command bodies are
covered by the same differential.

## Two VM fixes proved by the differential

1. **`SAVEBP`**: the engine sets `BP = len(stack) - 1` (the pushed saved-BP cell), not
   `len(stack)`. First live action: engine `SP=191, BP=190`; the old VM produced `BP=191`.
2. **String comparison**: KotOR tag comparisons are case-insensitive (`"carth"` equals
   `"Carth"`). The VM now uses `casefold()` for string `EQ`/`NEQUAL` and ordering; the
   `end_repairspawn` branch chain then follows the engine exactly.

## Reproduce

```bash
python tools/vm/live_diff.py \
  --events artifacts/lanes/s1-06-dynamic/runs/mesa-vmdiff7-20261102/events.jsonl \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out artifacts/lanes/s3-live/live_diff7.json
```

Expected output:

```json
{"matched": 28, "mismatched": 0, "replayed": 28, "traces": 400, "unique_programs": 28}
```
