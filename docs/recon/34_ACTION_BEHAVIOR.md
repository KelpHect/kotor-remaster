# 34 — Behavioral conformance for the 306 corpus-unused actions

Status: **closed** — 306/306 unused actions invoked against a prepared world.

| check | result |
|---|---|
| Unused actions checked | **306/306** |
| Specific handlers | **42** |
| Generic handlers | **264** |
| Unclassified/default | **0** |
| State-derived value assertions | **82** |
| SWMG set/get pairs | **20/20** |
| Errors | **0** |

The suite prepares a world with known AC/saves/traps/locks/faction/quest state, invokes every unused action with representative arguments, and asserts the state-derived result or the mediation/queue record. It also sets and then gets every SWMG property pair, proving the minigame state store is behaviorally live. Full per-action rows are in `artifacts/lanes/s3-02-actions/behavior_suite.json`.
