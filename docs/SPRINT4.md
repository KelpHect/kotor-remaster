# Sprint 4 - semantic-trace completion, validation and the KotOR game profile

Status: **complete - gate S4 met (2026-11-03)** - the Hydra 12.3-hydra.6 semantic-trace stack is
green, KotOR 1.0.3.0 is its first real game profile, two independent live runs are gap-free, and
the engine's aligned diff proves the deterministic 351-action prefix identical. See
[`SPRINT4_REPORT.md`](SPRINT4_REPORT.md) for the exit report and evidence.

Goal: finish and validate the semantic trace introduced by Hydra 12.3-hydra.6 / Hydralysis 0.8.0,
make KotOR its first real game profile, run a real 32-bit live trace through the shipped harness,
and diff two independent runs with the engine. Fix every fault found in our own tools while doing
it. Sprint 4 is the trace-stack sprint; it is separate from the P4 vertical slice in
[`MASTER_PLAN.md`](MASTER_PLAN.md).

Baseline: Hydra `12.3-hydra.6`, Hydralysis `0.8.0` (21 tools, catalog epoch 5), the Sprint 0-3
closure (`bb2b92e`): 228/228 source-level recompiles, 0 ABI, 8,666/8,666 raw exact, and the P0 v4
inventory at 10,849 functions / 565 handlers.

## Exit criteria (gate S4)

1. **Trace tool stack green** - the offline trace suite, the live recorder class and the live M1
   end-to-end class pass (13/13 in one interpreter). No regressions in the Sprint 0-3 lanes.
2. **Build-bound KotOR profile** - a TOML profile binds the four verified hooks to both the stock
   build and the lane's windowed copy, arms only on exact bytes, and can coexist with another
   instrumenter that has already detoured a site.
3. **Real 32-bit live trace** - a KotOR process records a gap-free session with the four probes
   armed, no refused required hook, and the semantic kinds (`kotor.exec`, `kotor.action`,
   `kotor.command`, `kotor.frame`) present.
4. **Real diff** - two independent live runs are compared by the engine's aligned diff on a
   bounded, gap-free action prefix; the deterministic prefix must classify `same`, not
   `not_comparable`.
5. **Evidence and safety** - tests, command transcripts, session ids and artifacts are recorded;
   the install stays read-only and no save file changes.

## Lanes

| Lane | Scope | Output root |
|---|---|---|
| s4-01-tools | Hydralysis/Hydra trace faults: x86 recorder, live prologue alternatives, kind validation, live pump, owner tokens, stop robustness | `tools/hydralysis/` (separate repo); `artifacts/tests/` |
| s4-02-profile | KotOR trace profile, build binding, hook prologues, detour alternatives | `tools/traces/kotor/swkotor-1.0.3.toml` |
| s4-03-live | Harness-driven live trace: elevated Mesa run, early stop while alive, extract and diff | `tools/harness/s4_*.py`; `artifacts/lanes/s4-trace/` |
| s4-04-report | Gate report, recon note, evidence index | `docs/SPRINT4_REPORT.md`; `docs/recon/41_SEMANTIC_TRACE.md` |

## Non-goals

- Replaying the captured KotOR programs through the Sprint 3 VM (the live differential already
  does that for its own corpus; Sprint 4 is about the trace stack and the game profile).
- Adding new engine features outside the trace protocol.
- Changing game assets, the install, or the real save slots.
