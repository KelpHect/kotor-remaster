# 26 — P2 engine map and action specification (gate G2)

Status: **closed** — built from the v4 inventory for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## G2 checklist

| item | result | evidence |
|---|---|---|
| Engine-relevant functions classified | **10,302** | v4 inventory (`functions-v4.jsonl`) |
| Unknown / medium / low | **0** | v4 inventory + recon/14 |
| Unexplained `.text` bytes | **0** | `docs/recon/14_P0_FUNCTION_CLASSIFICATION.md` |
| Action table slots | **772/772** | `artifacts/action_table.json` |
| Action specs / typed stubs | **772/772** | `action_specs.json` |
| Unique handler targets | **565/565** | v4 inventory + action table |
| Handler functions present | **772/772** | v4 inventory |
| Dispatcher | **0x0052c0d0**, bound 772 | vtable `0x7450e4` +8 |
| No unexplained entry points | **yes** | v4 inventory |

## Function inventory

| category | count |
|---|---:|
| compiler_helper | 30 |
| crt_runtime | 441 |
| engine | 10,302 |
| thunk | 76 |
| **total** | **10,849** |

Role counts: `action_dispatcher` 1, `action_handler` 565, `action_table_initializer` 1, `import_trampoline` 14, `jump_thunk` 62, `unassigned` 8,463, `vtable_method` 1,743.

## Action table and dispatcher

- Initializer `0x0054c960` (8295 bytes) writes the 772-slot table; the tail `0x005cd2c0` supplies the remaining slots (774 writes, ids 156/709 duplicated).
- Dispatcher `0x0052c0d0` is vtable `0x7450e4` slot `+8`; it rejects null handlers with `-2002` and calls `handler(id, arg)`.
- Return-type mix: `effect` 87, `event` 4, `float` 48, `int` 236, `location` 6, `object` 95, `string` 26, `talent` 5, `vector` 11, `void` 254.

## Subsystem map

| subsystem | owner | representative functions | evidence |
|---|---|---|---|
| script-vm | script service | `0x0054c960` action-table initializer (677 slots, tail-jumps to 0x005cd2c0); `0x005cd2c0` action-table tail (97 slots; 774 writes, ids 156/709 duplicated); `0x0052c0d0` action dispatcher (vtable 0x7450e4 +8, bound 772, null -> -2002) | docs/recon/02 §3; docs/recon/14 |
| resource-vfs | resource manager / module loader | `0x004c44d0` five-pass module/area mounter; `0x004babb0` game-loop module load/transition path; `0x004c5720` area module mount | docs/recon/23 #23-#25; docs/recon/21 |
| save-gff | save system | `0x00411730` GFF field-index append allocator; `0x00411020` GFF fidx growth/append; `0x004113d0` GFF six-section writer | docs/recon/21; docs/recon/23 #23-#25 |
| renderer-gl | renderer | `0x0046f900` mesh/indexed draw path; `0x00425ab0` glDrawElements wrapper; `0x00420f20` save/screenshot TGA path | docs/recon/22; docs/recon/23 §D |
| input-camera | input / camera | `0x006227e0` per-frame input update (ECX = input object); `0x00621210` game action dispatcher (Quick Save = 0xDA); `0x006bb6f8` camera +0x38 write; `0x0062fed0` dialogue camera controller ctor | docs/recon/12; docs/recon/22; docs/recon/25 |
| audio-stream | audio | `0x005dca30` voice-stream request builder (HD0:STREAMWAVES); `0x005dbd30` AIL_open_stream wrapper | docs/recon/23 §D |
| chargen-ui | chargen / UI | `0x006eb420` chargen manager + sub-screen ctor; `0x006eb320` real PLAY/start-module handler | docs/recon/22 |
| minimap | GUI / area map | `0x00508c50` ARE Map loader; reads MapResX/NorthAxis/MapZoom/MapPt/WorldPt; `0x00578c60` minimap affine scale/offset setup | docs/recon/19 §6 |

## Threads and state ownership

- Main game path: single-threaded in every P1/P2 hook (input, render, chargen, module load, scripts).
- Audio: Miles Mss32 owns internal audio threads; no engine-owned worker thread observed.
- Module/VFS owns mounted archives; the save writer owns GFF/fidx; the script service owns
  the 772-slot action table; the camera controller owns `+0xc4`/`+0x38`; the chargen manager
  owns the `END_M01AA` handoff; input/UI owns the action queue.
- Evidence: `docs/recon/22_DYNAMIC_P1.md`, `docs/recon/25_SYNTHETIC_RUNTIME.md`.

## Action specifications

`artifacts/lanes/s2-01-engine-map/action_specs.json` contains all 772 records: id, name,
return type, parameter list, required/total arity, handler address, handler presence,
default value, and stub text. The generator is `tools/analysis/action_specs.py`; it reads
the read-only `action_table.json`, `nwscript.nss`, and v4 inventory, and exits non-zero if
any action lacks a return type, handler, or typed stub.

## Reproduce

```bash
python tools/analysis/action_specs.py \
  --actions artifacts/action_table.json \
  --nwscript artifacts/game-extract/nwscript.nss \
  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
  --out artifacts/lanes/s2-01-engine-map/action_specs.json
python tools/analysis/engine_map.py \
  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
  --actions artifacts/action_table.json \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out-json artifacts/lanes/s2-01-engine-map/engine_map.json \
  --out-doc docs/recon/26_ENGINE_MAP.md
```

