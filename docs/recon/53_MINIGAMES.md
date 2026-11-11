# Recon 53 - minigame rules and state machines

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Shipped data

- `pazaakdecks.2da` has 4 rows and `card0..card9` plus `deckname`.
  - row 0 `PlayerDefault_NOTUSED`: `+1,+1,+2,+2,+3,+3,+4,+4,+5,+5`;
  - row 1 `TestAverage_ERASEME`, row 2 `TestNightmare_ERASEME`,
    row 3 `SuperDeck_ERASEME`, the last two containing `*N` double cards.
  There is no shipped opponent AI deck beyond these rows; the engine uses row
  0 for the player side deck and the requested row for the opponent's side
  deck, skipping `*` double cards until their last-card semantics are
  verified.
- `PlayPazaak` (action 364) is documented in `nwscript.nss` as
  `(nOpponentPazaakDeck, sEndScript, nMaxWager, bShowTutorial, oOpponent)`;
  `GetLastPazaakResult` (365) returns 1 for a player win and 0 for a loss.

## Pazaak

`engine/src/game/Minigames.*` implements:

- a 40-card main deck (four copies of 1-10) shuffled by xorshift32; exhaustion
  reshuffles into a fresh deck without resetting set state;
- two side decks (player and opponent), default `{-1,-2,3,4}` for direct
  construction and `pazaakdecks.2da` rows when started through `GameSession`;
- `playerDraw`, `playerPlaySide`, `playerStand`, `opponentTurn`;
- bust above 20, opponent draws to 17 then spends side cards that move closer
  to 20 without busting, ties are draws, first to two sets wins;
- `reset(seed, playerSide, opponentSide)` makes the full match reproducible.

`GameSession::startPazaak` loads `pazaakdecks.2da`, seeds the RNG from the deck
id and opponent object, stores the end script/wager/tutorial flag, and
`resolvePazaakIfOver` settles the wager (clamped at 0 gold) and runs the end
script. Actions 364/365 are wired to these methods.

## Swoop and turret: shipped modules

The Sprint 13 `SwoopRace` and `TurretGame` (an invented 3 x 3000 m track and five
invented targets) are gone. Four module RIMs carry an ARE `MiniGame` struct, found by
scanning every module in the install:

| Module | ARE | Type | Enemies | Obstacles | Return |
| --- | --- | --- | --- | --- | --- |
| `tar_m03mg` | `m03mg` | 1 swoop | 29 boost pads | 22 | `tar_m03af` at `tar03_wpmechanic` |
| `tat_m17mg` | `m17mg` | 1 swoop | 30 boost pads | 22 | `tat_m17ae` at `tat17ae_swoop_return` |
| `manm26mg` | `m26mg` | 1 swoop | 30 boost pads | 22 | `manm26ab` at `from26mg` |
| `M12ab` | `m12ab` | 2 turret | 6 Sith fighters | 1 | scripted by story state |

Entry: `tar03_announcer` runs `k_ptar_swooprun`; `tat17_04celis_01` and `tat17_04race_01`
run `k_ptat_swoop04`; `man26_swpreg` runs `k_pman_swoop42` and `man26_zoffice_01` runs
`k_pman_swoop44`; the turret loads from `k_ren_taris03` (STUNT_07 `scene_start`),
`k_ren_unkturret` (STUNT_34 `m12aa_c03_carth`), `k_ren_turretload`, `k_ren_turretld02`,
`k_ren_levescape`, `k_sup_galaxymap` and `k_act_hk47simul`.

Code: `kotor/MiniGameData.*` parses the struct and the LYT `trackcount` and
`obstaclecount` lists. `game/MiniGame3D.*` is the object model, with the retail routine
named beside each rule. `game/ScriptMiniGame.cpp` holds the SWMG actions and the gate.

Rules recovered from retail, all in `MiniGame3D.cpp`:

- Order per frame: input (006227e0), then the manager (006735d0): heartbeats, tracks,
  models, guns, bullets, collisions. A follower's OnCreate runs in its load (0066c370).
- Player motion: the track clip `track` plays at `speed * 0.01` (0066c870); models hang
  off its `modelhook` node. Lateral input goes through an RK4 of
  `dv = a*u - (a/m)*v` (00670fb0, 00670a90) into an offset clamped to the tunnel, or
  wrapped as a heading on a turret's infinite axis, by 360 outside 0..359 (0066cb40).
- Speed moves toward `Maximum_Speed` at the acceleration each frame (0066d640).
- A gun fires when its cooldown is 0 (006daec0, 006db3d0). The clip's start notification
  `startfire` runs OnFire (006738d0); its `fire<N>` event spawns the bullet at
  `bullethook<N>` (00673a40). Enemy shots aim at the target plus Target_Offset, scattered by
  Inaccuracy * distance. Bullets fly along local Y (006db090).
- Bullets test the moved segment against each target model's `hitbullet` mesh (006730a0);
  obstacles use `hitbump` against the player's path from the model's previous position
  (+0x94, vt+0x6c 004499d0) to its position (+0x78, vt+0x64 00449980) (0066e6b0). Both go
  through 0043e930 and the AABB part (vtable 00741048, 004428f0 -> 00441570) to the face
  test 004ab750, which is one-sided: a hit needs the path to cross a face from the front of
  its plane to the back. Every shipped hit mesh (68 nodes) is a closed box with outward
  faces, so a path through an obstacle hits once, on entry; the exit face does not count.
  Trigger enemies use a moving sphere of the summed radii (0066eda0, 004abdd0).
- Callback scripts replace the default; the SWMG_On* actions run the default from inside
  them (0066c190, 0066e5c0, 0066e2a0, 0066e7a0).
- The view hangs from `camerahook`. In the shipped `m12ab_camera` and `m17mg_camera` bind
  poses that node has local Y up and looks along local -Z, unlike bullets.

The race has no failure state of its own: `heartbeat` ends it when the player passes
y 3800 and records the time in globals for shipped dialogue to judge. Turret loss is
`k_pebo_hawkhit` (hit points under 2000 of 3000) and turret victory is the last
`k_pebo_sthdeath*`. The engine decides neither. That script picks its destination from
story globals; the Taris departure needs `K_STAR_MAP` 0 with `K_KOTOR_MASTER` 10, read from
the bytecode's stack offsets (offsets 911 and 941) because one decompiled view shifts the
local names by one.

Known simplifications, each noted at its code:

- The UseInertia branch of 0066d640 smooths the model toward queued track rotation keys;
  models here follow `modelhook` directly.
- Turret offset as Euler degrees: 0066d640 copies the offset into every rotating model
  part of type 0x3eb and into the camera model (+0x220); 0049f5f0 builds the rotation with
  004acac0, which takes degrees (007415f8) and multiplies axis rotations about Z (offset z),
  X (offset x) and Y (offset y) through 004a9ac0, then multiplies the result with the part's
  hook orientation (vt+0x68). The camera model and the cockpit models get the same rotation,
  so this alone does not explain retail's side-switching cockpit (see below).
- The enemy gun's horizontal angle axis is assumed (`004aa0f0` was not read to its end).
- `DoBumping` is 0 in all four ARE files, so the push-out branch of 0066eda0 cannot run
  and is not implemented.
- Emitters on bullets and models are not drawn, speed blur is kept as state only, the
  engine loop sounds and sound frequency fields do not reach the mixer, and the ARE's
  `Music` plays once as an effect.
- Mouse sensitivity in the window is the window's choice; retail's device scale was not
  recovered. Whether a held key repeats retail's fire event was not established, so a
  swoop shifts once per press and a turret fires while held.
- Node alpha controllers on HUD overlay models are not applied, so parts of the swoop HUD
  draw as opaque dark bands. Scene textures use the renderers' clamped sampler in every
  module, which streaks tiled track surfaces. Enemy fire pressure against retail is
  unmeasured: a fighter's shots almost always hit the stationary Hawk here.

Clip weights (00485bd0 adds, 00486670 updates, applied by 00483d80 -> 00483330):

- A new clip starts at weight 0 with the animation's transition time; a replayed clip keeps
  its time and weight and clears its done mark. An overlay goes to the front of the list,
  which is applied last.
- Each frame a clip that is not done gains `dt / transition` (or 1.0 when the transition is
  0, 0073d764) up to 1. A one-shot clip at its end loses `2 dt / transition` (or 2.0,
  00741be0); below 0 it is marked done (weight 1 again when the transition is 0) and the
  next update sends `done` and frees it.
- 00483330 blends each controller as `node = lerp(node, key(time), weight)`: the previous
  time is passed as `-100 - prevTime` (0074186c), which is always below -95 (00743254).
  Values stay in the nodes after a clip is freed.

Open differences, with the evidence gathered so far:

- Swoop gear meter: settled. The heartbeat (stack offsets traced from the bytecode, not the
  decompiled names) computes the meter from `MIN_RACE_GEAR` and `SWMG_GetPlayerSpeed`
  (005ccc60 reads follower +0x98, written by 0066c870 every frame from the 0066d640 ramp):
  gear 1 gives `speed / 60 * 8`, gears 2-4 `(speed - low) / (high - low) * 8` over
  60/100/150/210, gear 5 `(speed - 210) / 210 * 16`. At the finish it plays only `meter0`.
  Retail's speed log (`swoop_speed_log.tsv`) shows `TAR_SWOOP_ACCEL` 0, the per-gear
  minimum/maximum/acceleration 35/70/20, 60/120/8, 100/190/6, 150/225/3.3 and 210/420/2
  (raised by each boost pad), and 0/0/71.97 after the finish; the meter shows 3 to 9
  chevrons during that race and one around the finish. The earlier one-and-six reading was
  that run's input. Ours gives the same values gear by gear.
- Turret cockpit. The offset is in degrees, yaw wraps (no limit) and pitch is held to
  2..45 (+0x1e4), as here. Retail's log shows every rotating part and the camera model
  holding the same quaternion (pitch about X, then yaw about Z) and the view orbiting the
  pivot 0.64 away at height 0.444, as here. Yet the frames show the cockpit and its one
  visible gun behaving as if fixed to the ship: the gun is on the left facing forward (yaw
  about 300..60) and on the right, with the cockpit seen mirrored, facing backward (about
  160..260), while the HUD stays put. Here `mgf_turret` (RotatingModel 1, holding both
  gunbanks) turns with the camera, so the cockpit never changes. The only model attached
  without the rotation is `mgf_ebonhawk` (RotatingModel 0: 0066d2f0 attaches it to the
  track's `modelhook` with kind 0, the disabled 0x3e9 part); its hull behind the cockpit
  changes with heading in retail's frames as it does here. In retail's side sheet the
  cockpit keeps its screen position and its two dials stay in place, but the struts and the
  gun swap sides, which a rotation cannot produce. What draws it that way was not found. Callers of 0049fb10 (the plain
  dependent, vtable 00743894) are the camera hook (00671748, 00671797), bullets (00674a9d)
  and 00690f21.

Diagnostics (`game/ScriptMiniGame.cpp`): setting the environment variable
`KOTOR_DEBUG_MG_ANIM` logs every `SWMG_PlayAnimation` call with its target and retail flags;
`KOTOR_DEBUG_MG_SPEED` logs the player's speed, minimum, maximum and acceleration each time
a script reads `SWMG_GetPlayerSpeed` (the swoop heartbeat does so every frame), in the
columns of retail's speed log.

## Gate

`--minigame-test` (with `--minigame-script`) asserts the seeded pazaak transcript,
deck/side/turn rules, the `PlayPazaak` script bridge, session match completion, result
reporting and wager settlement, and that an ordinary module has no minigame.

`kotor_family_minigame3d` (`--family-test minigame3d`) loads all four modules and plays
each from the shipped entry script to the shipped return through player-level input only:
`MiniGameInput` and `MiniGameView`, both public on `GameSession`. The
`kotor_family_minigame3d_{swoop,turret}_{gl,vulkan}` tests capture a frame and compare it
with the same frozen view without the minigame's meshes (`--minigame-render-check`).

Evidence: `docs/evidence/s13/minigame.log` predates the shipped modules; fixture
`engine/tests/fixtures/minigame_script_test.nss/.ncs`.
