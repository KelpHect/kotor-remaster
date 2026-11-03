# Sprint 11 - combat, Force and alignment core

Status: **complete - gate S11 met (2026-11-03)**. Sprint 10 closed delayed
script closures. Sprint 11 closes the combat/Force/alignment gap left after the
earlier roadmap: characters now have Force points and alignment, Force powers
spend FP and apply save-based damage/heal/stun/fear/buff effects, combat feats
have active modes and passive bonuses, and both engine and original saves
round-trip the new state.

Baseline: Sprint 10 `34/34` CTest, 117/117 modules, VM 8,666/8,666, scene parity
0.0300, Python 143 tests / 7 skips, install unchanged. Sprint 11 result:
`35/35` CTest, 77.84 s.

## Gate S11

1. **Force catalog** - all **44** `spells.2da` rows with `usertype=1` load with
   label, force-point cost, `goodevil` alignment and range code. Force points
   follow a documented engine rule for Jedi classes.
2. **Force effects** - `useForcePower` applies:
   - heal/cure,
   - lightning/shock/storm/death-field/drain/wound/kill/choke/plague/affliction/
     saber-throw/push/wave/whirlwind damage with reflex/fortitude/will saves,
   - stun/stasis/slow/droid-stun and fear/horror/insanity timers,
   - Force armor/shield/resist and valor/speed buffs through a shared
     `ActiveEffect` stack,
   - Force Immunity (blocks Force powers), Force Breach (removes dispellable
     effects, including Immunity), and Suppress Force (removes effects and
     blocks further Force powers for 15 s).
   FP is spent, dark/light alignment shifts by the `goodevil` field, and the
   power is logged.
3. **Feats** - all **125** `feat.2da` rows load. Combat-relevant feats have
   effects: Power Attack/Improved/Master, Power Blast/Improved/Master,
   Flurry/Improved/Master, Critical Strike/Improved/Master, Whirlwind,
   Toughness/Improved/Master, Weapon Focus/Specialization, Dueling and
   Two-Weapon Fighting/Advanced/Mastery. Modes modify attack/damage/critical
   threat; Flurry adds an attack; Whirlwind hits adjacent hostiles; two-weapon
   penalties are -6/-4/-2/0 by tier. Skill feats (Caution/Empathy/Gear Head),
   Conditioning saves, Force Focus DC, Scoundrel's Luck/Jedi Sense defense,
   Sneak Attack, Toughness/Wookiee damage reduction and armor/weapon
   proficiency follow their TLK-described formulas.
4. **Alignment** - `CharacterSheet::alignment` is 0 dark .. 50 neutral ..
   100 light. Script get/set/adjust (`GetGoodEvilValue`,
   `GetAlignmentGoodEvil`, `SetGoodEvilValue`, `AdjustAlignment`) and party
   `good_evil` attributes read it. The character panel displays it.
5. **Script bridge** - `ActionUseFeat`, `ActionCastSpellAtObject`,
   `TalentSpell`/`TalentFeat`/`TalentSkill`, `ActionUseTalentOnObject`,
   `GetHasFeat`, `GetHasSpell`, `GetCurrentForcePoints`,
   `GetMaxForcePoints` and the talent type/id queries are exercised from a
   compiled NCS fixture.
6. **Persistence** - engine saves and original `SAVEGAME.sav` slots round-trip
   alignment, current/max Force points, feats, powers, the live `ActiveEffect`
   stack and suppression timer.
7. **Regression** - **35/35** CTest, 117/117 modules with 0 failures /
   0 warnings, VM corpus 8,666/8,666 with 0 mismatches, scene parity 0.0300,
   Python 143 tests / 7 skips, install sentinel 0 session changes.
8. **Evidence and docs** - report, technical note, fixtures and logs.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s11-01-force-data | ForcePowerInfo, FP/alignment fields, Jedi FP rule | `engine/src/game/Rules.*` |
| s11-02-combat | feat modes/passives, round integration, stun/fear/buffs | `engine/src/game/GameSession.*` |
| s11-03-force | power effects, saves, damage/heal, alignment | `engine/src/game/GameSession.*` |
| s11-04-script | action/talent bridge and script fixture | `engine/src/script/ActionRuntime.*`, `engine/tests/fixtures/` |
| s11-05-save | engine/original save alignment + FP | `engine/src/game/GameSession.*`, `engine/src/kotor/SaveGame.*` |
| s11-06-evidence | gate, report, recon note, regression logs | `engine/CMakeLists.txt`, `docs/` |

## Explicitly not this sprint

- Battle Meditation is re-scoped out by shipped data: its TLK description says
   it is large-scale only and "not applicable to small-scale combat".
- Force Camouflage exposes `allowsStealth`; full stealth AI is later work.
- Jedi Defense deflection is implemented for ranged NPC attacks.
- `ActionWait`/closure persistence across save, cutscenes, animation, audio/EAX,
   Bink/lip sync, full `.gui`, minigames, localization, mods/Override, KotOR 2
   world loading and P6 content.
- Retail-engine visual/audio acceptance of Force powers; this gate is
   deterministic simulation + script/action + save evidence in this engine.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa `
  --combat-force-test --combat-force-script "tests\fixtures\force_script_test.ncs" `
  --null --headless
```
