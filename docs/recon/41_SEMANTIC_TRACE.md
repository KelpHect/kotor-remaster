# 41 - Semantic trace: KotOR as the first real game profile

Date: 2026-11-03. Status: **complete**. Hydra Engine `12.3-hydra.6` and Hydralysis `0.8.0`
record a real 32-bit KotOR process, the engine ingests the capture live, and two independent runs
diff cleanly on their deterministic script-action prefix. This note records the hook map, the
live path, and every tool fault found and fixed while proving it.

## Profile

`tools/traces/kotor/swkotor-1.0.3.toml`, id `kotor/1`, SHA-256
`8e887fb069dda5a24aa92a9b117cfa956eb0e005b86958d180cd101d457eaa6f`.

| Hook | RVA / VA | Prologue (image form) | Event | Fields |
|---|---|---|---|---|
| `execute-code` | `0x1d2bd0` / `0x5d2bd0` | `6A FF 64 A1 00 00 00 00 68 81 3E 72 00 50 64 89` | `kotor.exec` | `program:va@1`, `length:u32@2` |
| `script-action` | `0x12c0d0` / `0x52c0d0` | `8B 54 24 04 81 FA 04 03 00 00 7D 17 8B 41 0C 8B` | `kotor.action` | `action:u32@0` |
| `run-command` | `0x1d45d0` / `0x5d45d0` | `53 55 56 57 8B 7C 24 14 33 ED 3B FD 8B F1 0F 84` | `kotor.command` | `command:va@0` |
| `frame` | `0x0ae860` / `0x4ae860` | `8B 49 04 E9 48 C3 00 00` | `kotor.frame` | - |

Two builds are bound: the stock `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
and the lane-06 windowed 1280x720 copy
`280ba024d7410933ef1dad5ed829e9bef428c07e7ae47d8a6ad7a3a9420d49e3`. Both verify 4/4 hooks.

The hook prologues are the image bytes. The profile also lists Frida-detour alternatives with the
`rel32` wildcarded; the live check accepts any `jmp rel32` detour whose declared tail (at least six
bytes) is intact at the same offset. A detoured probe is armed through Interceptor, never by
patching over the other instrumenter's jump, and the refusal/limitation is recorded as
`core.capture.source` with `limitation = "detour:prologue"`.

## Live path

1. `tools/harness/sprint1_dynamic_run.py` launches the lane-06 profile under Mesa llvmpipe with
   `live_hooks.js`, focuses the window and clicks New Game / class / Quick Character with real
   in-process mouse events (`hwclick2`).
2. `tools/harness/s4_trace_run.py` attaches the trace to the harness-owned PID with the KotOR
   profile (build hash checked by the runtime), opens the engine session and arms the four probes.
3. The runtime `TraceSource` writes a chained segment under the runtime session. `trace start`
   starts a `trace_pump.Pump` on a second engine client; the pump tails the segment and appends
   batches to the session, so `status`/`query` see live events.
4. Once the first `kotor.exec` appears, the runner keeps recording for a grace period and stops
   the trace **while the game is still alive**: capture stop finalizes the segment, the pump
   drains it, the session closes, and only then does the harness quit. This avoids the
   producer-lost gap that a game exit creates.
5. `tools/harness/s4_trace_extract.py` pages the semantic kinds to JSONL; the engine's aligned
   diff over a bounded prefix proves the match.

## Live evidence

| Run | Session | Records | Semantic events | Gaps | Probes |
|---|---|---:|---:|---:|---|
| live-04 | `tr_d4036f0998a9` | 27,796 | 27,714 (1,671 exec / 15,346 action / 2,915 command / 7,782 frame) | 0 | 4/4 |
| live-05 | `tr_82bfe0d0e41c` | 5,277 | 5,195 (481 / 3,008 / 609 / 1,097) | 0 | 4/4 |

Both runs report `producer.dropped = 0`, `producer.final = true`, `detach.quiescent = true`, and
`refused_required = 0`. Live-04's detach waited 125 ms with 0 busy/0 entered/0 left; live-05's
122 ms.

The earlier long runs `tr_f1785eb86c79` (36,083 events) and `tr_f5414c3d49f5` (36,324 events)
prove volume but have one producer-lost gap at game exit; the diff uses the gap-free pair.

## Real diff

`tools/harness/s4_trace_bounded_diff.py` takes the longest identical `kotor.action` prefix of two
extracted runs, imports exactly that prefix into two fresh sessions (sequential source sequences,
so no gap is possible), and asks the engine's aligned diff (`keys = kind, payload:action`).

```text
prefix        351 actions
groups         35
classes        same: 35
scanned        351 / 351
unmatched      0 / 0
gaps           0 / 0
```

Direct sequence checks agree: the first 351 action ids are identical between live-04 and live-05;
the first 100 `kotor.exec` lengths are 92/100 identical (the first 66 exactly). Divergence after
the prefix is expected: the two runs stop after different amounts of gameplay and the AI/script
scheduler interleaves differently.

The engine's whole-session diff (`diff-live02-vs-live03.json`) is correctly `not_comparable`
because both those sessions have a producer-lost gap; the bounded diff is the evidence that the
deterministic path matches.

## Tool faults found and fixed

| Fault | Fix | Files |
|---|---|---|
| x86 recorder compiled x64-only unwind/fast/TLS code and used cdecl for Win32 imports | x64 blocks guarded by `__x86_64__`, `HYDRA_STDCALL` for ia32, `bp_ticks` as `uint32`, one-frame ia32 return-address fallback | `runtime_agents.py` |
| Live prologue check compared only the image bytes, so a Frida detour refused the hook | `expects` alternatives with `??` wildcards; detour tail detection; force Interceptor on a detoured site; `detour:prologue` coverage event | `profiles/probes.py`, `runtime_agents.py`, `runtime_trace.py` |
| Profile loader allowed `game.script.action` (three parts); the engine accepts only `<profile>.<name>` | loader regex tightened to exactly two parts; KotOR kinds renamed `kotor.exec/action/command/frame` | `profiles/loader.py`, `tools/traces/kotor/swkotor-1.0.3.toml` |
| `trace start` with a live launch opened an engine session but never ingested the segment | `trace_pump.Pump` wired into start/stop; `trace_start` returns `segment_id`; pump drains before close | `trace.py`, `runtime_trace.py` |
| Capture stop compared the trace writer cap against the runtime owner token | `_trace_option(owner=...)` uses the runtime token; `RuntimeRunner` forwards it to the worker for capture stop/scope | `trace.py`, `runtime.py` |
| A game that exits before `trace stop` left the trace session open (detach raised) | stop captures each failure, always drains the pump and always closes the session | `trace.py` |
| Late lifetime query at stop counted `script has been destroyed` as a probe fault | recorded as a coverage refusal (`lifetimes-unavailable`) | `runtime_trace.py` |
| A site whose stride reset to 1 emitted its next kept record as `exact`, so no kept event carried the `sampled` label | `Site.thinned` sticky flag drives `F_SAMPLED` in both record paths; per-test target cleanup and a 1,000/s budget make the adaptive test deterministic | `runtime_agents.py`, `test_trace_e2e.py` |
| The M1 class retained each test's in-process agent/`TraceSource`; accumulated captures exhausted memory at the 100k-frame test, whose fixture could also finish or hit its own watchdog before the stop | `Live.close` drops the agent trace/script references and `tearDown` collects; `frameloop --watchdog=` is configurable and the failure-mode test uses a slow 60k×10 ms producer | `tests/trace_live.py`, `tests/test_trace_e2e.py`, `tests/fixtures/frameloop.c` |
| Live tests attached to the venv launcher instead of the real interpreter | spawn `sys._base_executable` in the watchdog and recorder tests | `test_trace_extras.py`, `test_trace_capture.py` |

## Reproduction

```powershell
# offline suite
python tools/hydralysis/tools/run_tests.py -k trace -j 8 --strict

# live recorder class (Frida on PYTHONPATH)
$env:PYTHONPATH="<repo>\tools\hydra-root\runtime-backend\frida-17.24.3;<repo>\tools\hydralysis\src;<repo>\tools\hydralysis\tests;<repo>\tools\hydralysis\tests\fixtures"
python -m unittest tests.test_trace_capture.Recorder -v

# one elevated KotOR live trace (UAC is disabled on this host)
Start-Process -Verb RunAs -Wait -FilePath tools\.venv\Scripts\python.exe `
  -ArgumentList 'tools\harness\s4_trace_run.py','--label','s4-kotor-live-05' -WorkingDirectory <repo>

# extract and diff
python tools\harness\s4_trace_extract.py --session <session> --out <run>.jsonl
python tools\harness\s4_trace_bounded_diff.py --a <a>.jsonl --b <b>.jsonl --out diff.json
```

Artifacts: `artifacts/lanes/s4-trace/` (runs, timelines, frames, summaries, extracted events,
diff JSON), `artifacts/tests/hydralysis_trace_suite6.log`, `artifacts/tests/trace_capture_live3.log`,
`artifacts/tests/trace_e2e_m1_final5.log`.
