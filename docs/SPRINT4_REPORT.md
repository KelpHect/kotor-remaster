# Sprint 4 exit report - semantic trace and the KotOR game profile

Status: **complete - gate S4 met (2026-11-03).** Hydra Engine `12.3-hydra.6` and Hydralysis
`0.8.0` record a real 32-bit KotOR process end to end: four build-bound hooks, a live pump into
the engine, zero capture gaps, and an engine aligned diff that proves the deterministic
351-action prefix of two independent runs identical. Every tool fault found on the way is fixed
in Hydralysis `sprint4/live-trace` (`d150b9b` + `0f4e102` + `b502214` + `cda7baf`, pushed to origin).

Baseline: Hydra `12.3-hydra.6`, Hydralysis `0.8.0` (21 tools, catalog epoch 5), Sprint 0-3
closure `bb2b92e`: 228/228 source-level recompiles, 0 ABI, 8,666/8,666 raw exact, 10,849
functions / 565 handlers reconciled.

## Exit criteria (gate S4)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | Trace tool stack green | **met** - offline **339/339**; live recorder **12/12**; live M1 **13/13 in one class run, 71.0 s** | `artifacts/tests/hydralysis_trace_suite5.log`; `artifacts/tests/trace_capture_live3.log`; `artifacts/tests/trace_e2e_m1_final5.log` |
| 2 | Build-bound KotOR profile | **met** - `kotor/1`, SHA `8e887fb0...`, both `0290889e...` and `280ba024...` verify **4/4** hooks; detour alternatives accepted | `tools/traces/kotor/swkotor-1.0.3.toml`; profile binaries |
| 3 | Real 32-bit live trace | **met** - live-04 `tr_d4036f0998a9`: **27,796 records / 27,714 semantic events**, 0 gaps, 0 dropped, 4/4 probes; live-05 `tr_82bfe0d0e41c`: 5,277 / 5,195, 0 gaps | `artifacts/lanes/s4-trace/s4-kotor-live-04.log`, `...-05.log`; `artifacts/lanes/s4-trace/live-0{4,5}-semantic.jsonl` |
| 4 | Real diff | **met** - common prefix **351 actions**, **35 groups, all `same`**, scanned 351/351, 0 unmatched, 0 gaps | `artifacts/lanes/s4-trace/diff-bounded-live04-05.json`; `tools/harness/s4_trace_bounded_diff.py` |
| 5 | Evidence and safety | **met** - install sentinels unchanged, no save changes, commands/results below | `artifacts/lanes/s4-trace/runs/s4-kotor-live-04/summary.json`, `...-05/summary.json` |

## Test evidence

```text
offline trace suite     339 tests, 70 tasks, 8 workers; 0 failed alone, 0 failed only under load; OK
                         artifacts/tests/hydralysis_trace_suite5.log
live recorder class      12 tests, 42.1 s; OK
                         artifacts/tests/trace_capture_live3.log
live M1 class            13 tests, 71.0 s; OK
                         artifacts/tests/trace_e2e_m1_final5.log
profile + prologue       35 tests; OK (tests.test_trace_profiles, tests.test_trace_capture.PrologueAlternatives)
```

The 339-test suite includes the new `PrologueAlternatives` tests (wildcard parsing, the
detour-tail rule, and the two-part kind refusal) and the corrected start/stop delegation test
(runtime owner token plus resume). The M1 class now releases each test's in-process agent and
`TraceSource` before the next test, and `frameloop` has a configurable watchdog for the
failure-mode test; the complete class passes in one interpreter in 71 s.

## Live KotOR trace

The harness (`tools/harness/sprint1_dynamic_run.py` + `live_hooks.js`) starts the lane-06
windowed profile under Mesa llvmpipe, focuses the window and drives New Game / class / Quick
Character with real in-process mouse events (`hwclick2`). `tools/harness/s4_trace_run.py`
attaches the KotOR profile to that PID, arms the four hooks, tails the segment through
`trace_pump`, and stops the trace while the game is still alive (capture stop -> pump drain ->
session close) so the exit cannot lose producer records.

| Run | Session | Records | Event kinds | Gaps | Drops | Detach |
|---|---|---:|---|---:|---:|---|
| live-04 | `tr_d4036f0998a9` | 27,796 | 1,671 exec; 15,346 action; 2,915 command; 7,782 frame | 0 | 0 | quiescent, 125 ms |
| live-05 | `tr_82bfe0d0e41c` | 5,277 | 481 exec; 3,008 action; 609 command; 1,097 frame | 0 | 0 | quiescent, 122 ms |

Both report `counts.verified = 4`, `refused_required = 0`, `refused_optional = 0`,
`producer.dropped = 0`, `producer.final = true`, and `fault_free`. The `script-action` site was
already detoured by `live_hooks.js`; the trace accepted the detour tail, recorded
`core.capture.source` with `limitation = "detour:prologue"`, and armed through Interceptor
without touching the other instrumenter's jump.

The earlier long runs `tr_f1785eb86c79` (36,083 events) and `tr_f5414c3d49f5` (36,324 events)
show the volume, but each has one producer-lost gap at game exit. The whole-session diff between
them is correctly `not_comparable` for that reason (`artifacts/lanes/s4-trace/diff-live02-vs-live03.json`);
the gap-free pair is the diff evidence.

## Real diff

`tools/harness/s4_trace_extract.py` pages the semantic kinds to JSONL.
`tools/harness/s4_trace_bounded_diff.py` takes the longest identical `kotor.action` prefix,
imports exactly that many actions into two fresh trace sessions with sequential source
sequences, and asks the engine's aligned diff (`keys = kind, payload:action`).

```text
prefix        351 actions
groups         35
classes        same: 35
scanned        351 / 351
unmatched      0 / 0
gaps           0 / 0
```

Direct checks agree: the first 351 action ids are identical; the first 100 `kotor.exec` lengths
are 92/100 identical (the first 66 exactly). Divergence after the prefix is expected: the runs
stop after different amounts of gameplay and the AI/script scheduler interleaves differently.
A self-diff (`tr_f5414c3d49f5` against itself) reports every group `same`, which validates the
diff path independently of the two-run match (`artifacts/lanes/s4-trace/diff-self.json`).

## Tool faults fixed

All fixes are in the Hydralysis repository, branch `sprint4/live-trace`, commits `d150b9b`, `0f4e102`, `b502214` and `cda7baf`
(pushed `origin/sprint4/live-trace`), plus the KotOR profile and harness scripts in this repo.

| Fault | Fix |
|---|---|
| The recorder compiled x64-only unwind/fast/TLS code on ia32 and used cdecl for Win32 imports | x64 blocks guarded by `__x86_64__`; `HYDRA_STDCALL` for ia32 imports and JS wrappers; `bp_ticks` is `uint32`; ia32 has a one-frame return-address fallback |
| Live prologue check compared only image bytes, so a Frida detour refused the hook | profile `expects` alternatives with `??`; detour-tail detection; force Interceptor on a detoured site; `detour:prologue` coverage record |
| Profile loader allowed three-part kinds (`game.script.action`); the engine accepts only `<profile>.<name>` and rejected every such event at ingest | loader regex tightened; KotOR kinds renamed `kotor.exec/action/command/frame`; a synthetic append of the renamed event is accepted |
| `trace start` with a live launch never ingested the runtime segment | `trace_pump.Pump` wired into start/stop; `trace_start` returns `segment_id`; the pump drains before close |
| Capture stop compared the trace writer cap with the runtime owner token | `_trace_option(owner=...)` uses the runtime token; `RuntimeRunner` forwards it to the worker for capture stop/scope |
| A game that exited before `trace stop` left the session open because detach raised | `_stop` records each failure, always drains the pump, always closes the session |
| Late lifetime query at stop counted `script has been destroyed` as a probe fault | recorded as a coverage refusal (`lifetimes-unavailable`); `fault_free` stays about probes |
| A site whose stride reset to 1 could emit its next kept record as `exact`, so no kept event carried the `sampled` label | `Site.thinned` is sticky and drives `F_SAMPLED` in both record paths; the M1 class closes each test's targets and uses a 1,000/s budget for the adaptive test and checks every record from a thinned site |
| The M1 class kept each test's in-process agent/`TraceSource` alive, so accumulated stress captures exhausted memory at the 100k-frame test; the failure-mode fixture could finish or hit its own watchdog before the stop | `Live.close` drops the agent's trace/script references and `tearDown` collects; `frameloop --watchdog=` is configurable and the failure-mode test uses a slow 60k×10 ms producer |
| Live tests attached to the venv launcher instead of the real interpreter | watchdog and recorder tests spawn `sys._base_executable` |

## Safety and scope

The install sentinels are unchanged before and after every run (`swkotor.exe`, `patch.erf`,
`swkotor.ini`, `chitin.key`), no save files are created or changed, and the game process is the
harness-owned copy under `artifacts/lanes/s1-06-dynamic/profile`. The stock and windowed builds
are both bound by SHA-256; a mistyped path cannot select another program. The trace store lives
under `tools/hydra-root/`, which is gitignored and local.

Known limitation: a one-shot CLI `hydralysis call trace` exits after `start`, so the in-process
pump dies with it. The supported live path is a long-lived client (the harness) or the pump API;
the CLI `start` reply carries the segment id, and the capture can always be imported offline.
This is a CLI lifetime limitation, not a capture or engine gap.

## Reproduce

```powershell
# offline trace suite
python tools/hydralysis/tools/run_tests.py -k trace -j 8 --strict

# live recorder (Frida on PYTHONPATH)
$env:PYTHONPATH="<repo>\tools\hydra-root\runtime-backend\frida-17.24.3;<repo>\tools\hydralysis\src;<repo>\tools\hydralysis\tests;<repo>\tools\hydralysis\tests\fixtures"
python -m unittest tests.test_trace_capture.Recorder -v

# one elevated KotOR run (UAC disabled on this host)
Start-Process -Verb RunAs -Wait -FilePath tools\.venv\Scripts\python.exe `
  -ArgumentList 'tools\harness\s4_trace_run.py','--label','s4-kotor-live-05' -WorkingDirectory <repo>

# extract and diff
python tools\harness\s4_trace_extract.py --session tr_d4036f0998a9 --out artifacts\lanes\s4-trace\live-04-semantic.jsonl
python tools\harness\s4_trace_bounded_diff.py --a artifacts\lanes\s4-trace\live-04-semantic.jsonl --b artifacts\lanes\s4-trace\live-05-semantic.jsonl --out artifacts\lanes\s4-trace\diff-bounded-live04-05.json
```

Full technical notes: [`docs/recon/41_SEMANTIC_TRACE.md`](recon/41_SEMANTIC_TRACE.md).
