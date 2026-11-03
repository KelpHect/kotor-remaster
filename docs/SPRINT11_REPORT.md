# Sprint 11 report - combat, Force and alignment core

Status: **complete - gate S11 met (2026-11-03)**. Sprint 11 closes the combat,
Force-power and alignment gap left after Sprint 10: the 44 shipped Force powers
load with costs/alignment/range, Force points and saves drive damage/heal/
stun/fear/buff effects, combat feats have active modes and passive bonuses, and
alignment/FP/feats/powers persist in both engine and original saves.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 35
Total Test time (real) =  77.84 sec
```

The new target is `kotor_combat_force`. All Sprint 10 save/profile/delayed
targets still pass. Python tools remain **143 tests, 0 failures, 7 skips**.

## 2. Force catalog and resources

- **44/44** `spells.2da` rows with `usertype=1` load as Force powers with
  label, force-point cost, `goodevil` alignment and range.
- **125/125** `feat.2da` rows load.
- Jedi classes get deterministic force points:
  `level * (10 + positive WIS modifier + positive CHA modifier)`.
- `CharacterSheet::alignment` is 0..100 (dark..light).

## 3. Force effects

`GameSession::useForcePower` spends FP, checks range, rolls the target's
Fortitude/Will/Reflex save against `10 + level/2 + WIS`, and applies:

- heal/cure,
- lightning/shock/storm/death field/drain/wound/kill/choke/plague/affliction/
  saber throw/push/wave/whirlwind damage,
- stun/stasis/slow/droid stun/affect mind/dominate,
- fear/horror/insanity,
- armor/shield/resist, valor and speed through a shared `ActiveEffect` stack,
- Force Immunity, Force Breach and Suppress Force:
  Immunity blocks Force powers, Breach removes all dispellable effects
  (including Immunity), and Suppress Force removes effects and blocks further
  Force powers for 15 s,
- dark/light alignment shifts (`G` +2, `E` -2).

The gate checks lightning cost/damage/dark-side shift, heal, armor, stun and
alignment.

## 4. Feats and combat

Combat feats now have effects:

- Power Attack/Improved/Master and Power Blast/Improved/Master: attack -3,
  damage +5/+8/+10.
- Flurry/Improved/Master: extra attack, attack -4/-2/0.
- Critical Strike/Improved/Master: attack -2, threat 19/18/17.
- Whirlwind: attack -4, adjacent AoE.
- Toughness: +1 HP/level; Improved adds 2-point damage reduction; Master adds
  a second +1 HP/level while retaining the reduction.
- Weapon Focus: one +1 attack bonus; Weapon Specialization: one +2 damage bonus.
- Dueling/Advanced/Master: +1/+2/+3 attack.
- Two-Weapon Fighting/Advanced/Mastery: -6/-4/-2/0 attack with two weapons.
- Caution/Empathy/Gear Head: +1/+2/+3 to their skill groups.
- Conditioning/Improved/Master: +1/+2/+3 all saving throws.
- Force Focus/Improved/Master: +1/+2/+4 Force save DC.
- Scoundrel's Luck and Jedi Sense tiers: +2/+4/+6 defense.
- Sneak Attack I-X: +1d6..+10d6 against stunned/fear-immobilized targets.
- Improved/Master Toughness: +1 HP/level and 2-point damage reduction.
- Wookiee Toughness: +2 HP/level and 2-point damage reduction.
- Armor/weapon proficiency is enforced from `baseitems.reqfeat0..4`; classes
  receive their starting proficiencies.
- Force Jump / Advanced / Mastery closes a lightsaber attack from beyond 10 m
  to 2 m and adds +0/+2/+4 to the first strike.
- Jedi Defense / Advanced / Master: lightsaber deflection rolls with +0/+3/+6
  and returned bolts on a 10-point margin.
- Force Camouflage: `allowsStealth` data ability; full stealth AI is later.

Flurry adds an attack in the round, Whirlwind hits adjacent hostiles, and
stunned/feared creatures skip movement and attacks. Force armor adds AC, Force
shield absorbs damage, Force speed multiplies movement. Armor and weapon
proficiencies are enforced from `baseitems.reqfeat0..4`; classes receive their
starting proficiencies.

Deterministic assertions include `caution_1/2/3`, `empathy_1`, `gear_head_1`,
`conditioning_1/2/3`, `force_focus_1/2/4`, `scoundrel_luck_2/4/6`,
`jedi_sense_2`, `knight_sense_4`, `master_sense_6`, `sneak_1/2`,
`wookiee_endurance_dr/hp`, proficiency checks, `ranged_entities_detected`,
`jedi_defense_6`, `jedi_deflect_yes/no`, `jedi_deflect_integration`,
`force_camouflage_stealth`, `effect_engine_roundtrip` and
`effect_original_roundtrip`.

## 5. Alignment and script bridge

The compiled fixture `engine/tests/fixtures/force_script_test.ncs` verifies:

```text
SetGoodEvilValue / GetGoodEvilValue
GetHasFeat / GetHasSpell
GetCurrentForcePoints / GetMaxForcePoints
GetTypeFromTalent / GetIdFromTalent
ActionUseFeat
ActionCastSpellAtObject
ActionUseTalentOnObject
```

Talent values encode type in the high bits and id in the low 29 bits. The
pinned corpus keeps its original generic talent mediation, so the 8,666-script
parity is unchanged.

## 6. Persistence

- Engine text save: `alignment`, `force_points`, `max_force_points`, and every
  live `ActiveEffect` plus `force_suppressed`.
- Original save: PC `GoodEvil`, `CurrentForce`, `MaxForcePoints`, and the
  `EffectList`/`ForceSuppressed` fields.
- Feats use `feat=` and the original `FeatList`; powers use `power=` and the
  original `LvlStatList[].KnownList0[].Spell` list.
- The gate applies Force Armor, saves/reloads both formats, and checks the
  effect label and computed armor bonus survive.

The gate round-trips alignment 25 and the current FP through both paths and
reloads the original slot.

## 7. Regression

- **CTest: 35/35 passed** (77.84 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0300**, PASS.
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 8. Not in Sprint 11

- Battle Meditation is **re-scoped out by shipped data**: its TLK description
  says it is for large fleets/armies and "not applicable to small-scale
  combat", so there is no small-scale rule to implement.
- Force Camouflage exposes `allowsStealth`; full stealth perception/presentation
  remains later work.
- Cutscenes, animation/skinning, audio/EAX, Bink/lip sync, full `.gui`,
  minigames, localization, mods/Override, KotOR 2 world loading and P6 content.

## Evidence index

- `docs/evidence/s11/ctest.log`
- `docs/evidence/s11/combat-force.log`
- `docs/evidence/s11/000012 - ENGINE/` (generated original slot)
- `docs/evidence/s11/validate-combat-save.log`, `combat-save-parse.json`
- `docs/evidence/s11/all-modules.log`, `all-modules.json`
- `docs/evidence/s11/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s11/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s11/tools-tests.log`
- `docs/evidence/s11/install-sentinel.log`
- Fixtures: `engine/tests/fixtures/force_script_test.nss/.ncs`
- Technical note: [docs/recon/49_COMBAT_FORCE.md](recon/49_COMBAT_FORCE.md)
