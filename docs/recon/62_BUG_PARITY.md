# Recon 62 - bug-parity models

Status: Sprint 14 implementation note. Deterministic classic-vs-fixed models for
the seven documented KotOR 1 glitches. Classic mode reproduces the retail
behavior; fixed mode opts out.

## Models

| Glitch | Classic behavior | Fixed behavior | Fixture |
|---|---|---|---|
| Anywhere Menu Glitch | Character panel opens during dialogue | Panel is refused | `classic_anywhere_menu` / `fixed_anywhere_menu` |
| Displaced Load Zone | Door transition works from any distance | Requires 2 m proximity | `classic_displaced_load` / `fixed_displaced_load` |
| Spawn Warps | Party keeps pre-transition coordinates | Party spawns at destination entry | `classic_spawn_warp` / `fixed_spawn_warp` |
| Hotshot | Destination snapshot restores MC level/XP; inventory and quests persist | Warp refused | `classic_hotshot*` / `fixed_hotshot*` |
| Fake Level Up | Level advances, XP unchanged | Level-up refused | `classic_fake_level*` / `fixed_fake_level*` |
| Item duping | Equipped items copied into inventory on the airlock cycle | No copy | `classic_dupe*` / `fixed_dupe*` |
| The Routine | Chain of menu glitch, hotshot, spawn warp, fake level and duping completes | Chain refuses | `classic_routine` / `fixed_routine*` |

The menu and load-zone models are integrated in `GameSession`
(`toggleUiPanel`, `testTransition`). The remaining state transitions live in
`engine/src/game/BugParity.h` so the fixture can assert both the mutation and
the unchanged fixed-mode state.

## Sources

- KotOR 1 No Major Glitches route, speedrun.com:
  `https://www.speedrun.com/kotor1/guide/d8i6l` (archived:
  `https://web.archive.org/web/20210729145021/https:/www.speedrun.com/kotor1/guide/d8i6l`).
- KotOR 2 Hotshot guide, speedrun.com: `https://www.speedrun.com/kotor2/guides/y4rma`.
- SWKOTOR Wiki glitches page: `https://swkotorwiki.fandom.com/wiki/KOTOR:Glitches`.
- GameFAQs Fake Level Up thread:
  `https://gamefaqs.gamespot.com/boards/516675-star-wars-knights-of-the-old-republic/54586547`.
- GameFAQs level-up glitch thread:
  `https://gamefaqs.gamespot.com/boards/332266-star-wars-knights-of-the-old-republic/79772616`.
- CheatCC KotOR glitches page (Manaan airlock duplication):
  `https://www.cheatcc.com/articles/star-wars-knights-of-the-old-republic-cheats-secrets-for-ios-pc-nintendo-switch-and-xbox/`.

## Boundaries

- These are observable state-transition models, not full multi-hour quest
  replays. The classic mutation and fixed opt-out are deterministic and gated.
- The airlock model duplicates the equipped set; stack counts and per-item
  metadata follow the existing inventory item model.
- "The Routine" is modeled as the documented chain, not as every community
  variation of the route.
