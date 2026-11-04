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
- `swoop` and turret rules are not exposed in a 2DA; the shipped scripts drive
  them through `SWMG_*` actions and their content lives in models/areas.
  Sprint 13 therefore models the deterministic rules and timing, not track
  geometry.
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

## Swoop

`SwoopRace` models speed toward
`35 + throttle*70 + boost*35 - brake*25`, clamped to 140 m/s; 3 laps of a
3,000 m track. `update(dt, throttle, boost, brake)` is the only entry point, so
the 150-second gate run is deterministic (9,001.53 m in 66.20 s).

## Turret

`TurretGame` spawns five targets at -40..40 degrees and 100..68 m; `update`
approaches live targets at 12 m/s and removes 20 hit points when one reaches
the player; `fireAt` records shots/hits, awards 100 points per kill and rejects
dead targets.

## Gate

`--minigame-test` (with `--minigame-script`) asserts the seeded pazaak
transcript, deck/side/turn rules, swoop laps/brake/determinism, turret
shots/score/arrival damage, the `PlayPazaak` script bridge, session match
completion, result reporting and wager settlement.

Evidence: `docs/evidence/s13/minigame.log`; fixture
`engine/tests/fixtures/minigame_script_test.nss/.ncs`.
