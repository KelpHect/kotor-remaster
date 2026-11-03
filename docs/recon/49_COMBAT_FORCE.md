# Recon 49 - combat, Force powers and alignment core

Status: Sprint 11 evidence note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## 1. Data catalogs

`Rules` already loads `classes`, `exptable`, `skills`, `feat`, `baseitems`,
`featgain` and `spells`. Sprint 11 adds:

- `ForcePowerInfo` for every `spells.2da` row with `usertype=1`
  (**44/44**): label, force-point cost, `goodevil` (`G`/`E`/`-`), range
  (`W`/`T` = 2 m, `M` = 15 m, `S` = 25 m, personal = 0).
- `Rules::featCount()` exposes the **125** `feat.2da` rows.
- Jedi classes (3 Guardian, 4 Consular, 5 Sentinel) derive
  `maxForcePoints = level * (10 + max(0,WIS mod) + max(0,CHA mod))`.
  This is an explicit engine rule because the shipped tables do not carry a
  force-point progression.
- `CharacterSheet::alignment` is 0 dark .. 50 neutral .. 100 light.

## 2. Force powers

`GameSession::useForcePower` validates:

1. the power is in `CharacterSheet::powers`;
2. the force-point cost is available;
3. a hostile target exists and is inside the power's range.

It spends FP, records `lastForcePower_`, applies the effect and shifts alignment
by `+2` (`G`) or `-2` (`E`), clamped 0..100.

Save DC is `10 + level/2 + WIS modifier`; the target's `FortSaveThrow`,
`WillSaveThrow` or `RefSaveThrow` from UTC is added to a d20. Damage powers use
`max(1, level/2)` d6 and halve on a successful save.

| Power family | Effect |
|---|---|
| Heal, Cure | `8 + 2*level + max(0,WIS mod)` HP |
| Lightning, Shock, Storm, Death Field, Drain Life, Wound, Kill, Choke, Plague, Affliction, Saber Throw, Push, Wave, Whirlwind, Droid Disable/Destroy | d6 damage per two levels; reflex/fortitude/will save for half |
| Stun, Stasis, Mass Stasis, Slow, Droid Stun, Affect Mind, Dominate | 6 s stun on failed will save |
| Fear, Horror, Insanity | 6 s fear on failed will save |
| Armor, Aura, Shield, Resist Energy | `ActiveEffect` +4..+9 AC for 30 s; Shield also absorbs `10 + 2*level` damage |
| Valor, Knight Valor, Master Valor | `ActiveEffect` +2..+6 attack/damage for 30 s |
| Speed, Speed Burst, Knight Speed, Speed Mastery | `ActiveEffect` 1.5x movement for 20 s |
| Force Immunity | `ActiveEffect`; blocks Force powers until removed/expired |
| Force Breach | removes every dispellable `ActiveEffect`, including Immunity |
| Suppress Force | removes dispellable effects and blocks Force powers for 15 s |

## 3. Feats

`feat.2da` categories are loaded for all 125 rows. Combat-relevant effects:

- **Power Attack / Improved / Master** and **Power Blast / Improved / Master**:
  attack -3, damage +5/+8/+10.
- **Flurry / Improved / Master**: one extra attack, attack -4/-2/0.
- **Critical Strike / Improved / Master**: attack -2, critical threat 19/18/17.
- **Whirlwind Attack**: attack -4, hits every hostile within 2.4 m.
- **Toughness / Improved / Master**: +2/+4/+6 HP per level when learned.
- **Weapon Focus**: +1 attack (max one focus bonus).
- **Weapon Specialization**: +2 damage (max one specialization bonus).
- **Dueling / Advanced / Master**: +1/+2/+3 attack.
- **Two-Weapon Fighting / Advanced / Mastery**: -6/-4/-2/0 attack while two
  weapons are equipped.
- **Caution / Improved / Master**: +1/+2/+3 Demolitions and Stealth.
- **Empathy / Improved / Master**: +1/+2/+3 Persuade, Awareness and Treat Injury.
- **Gear Head / Improved / Master**: +1/+2/+3 Repair, Security and Computer Use.
- **Conditioning / Improved / Master**: +1/+2/+3 to all saving throws.
- **Force Focus / Improved / Master**: +1/+2/+4 to Force power save DC.
- **Scoundrel's Luck / Improved / Master**: +2/+4/+6 defense.
- **Jedi Sense / Knight / Master**: +2/+4/+6 defense.
- **Sneak Attack I-X**: +1d6..+10d6 against stunned/fear-immobilized targets.
- **Improved/Master Toughness**: +1 HP/level and 2-point damage reduction
  (Master adds a second +1 HP/level).
- **Wookiee Toughness**: +2 HP/level and 2-point damage reduction.
- **Armor/Weapon Proficiency**: base item `reqfeat0..4` is enforced by
  `Rules::canEquip`; classes receive their starting proficiencies.
- Battle Meditation, Jedi Defense projectile deflection, Force Camouflage and
  Force Jump remain content-specific no-ops until their presentation/projectile/
  stealth/navigation subsystems exist.

Active modes last 30 s and are stored as `CombatMode`; the character panel
shows the current mode.

## 4. Combat integration

- Attack rolls use the feat-adjusted `Rules::attackBonus`.
- `playerAttack` applies the active mode's attack/damage/threat values and the
  target's Force shield.
- `npcAttack` checks stun/fear, adds Force armor to AC and absorbs shield
  damage before applying HP.
- `updateCombat` decrements mode/buff/stun/fear timers, skips stunned or feared
  hostiles, and executes Flurry/Whirlwind attacks.
- Force Speed multiplies movement speed while active.

## 5. Script bridge

`engine/tests/fixtures/force_script_test.nss` is compiled with the historical
`nwnnsscomp -v129`. It exercises:

```text
SetGoodEvilValue(OBJECT_SELF, 77)
GetGoodEvilValue, GetHasFeat, GetHasSpell
GetCurrentForcePoints, GetMaxForcePoints
GetTypeFromTalent(TalentSpell(35)), GetIdFromTalent(...)
ActionUseFeat(28, OBJECT_SELF)
ActionCastSpellAtObject(17, OBJECT_SELF)
ActionUseTalentOnObject(TalentSpell(17), OBJECT_SELF)
```

`GameSession::action` handles action ids 48, 55, 56, 125, 127, 201, 285, 287,
304, 309, 310, 377, 543, 723, 726 and 750. Talent values encode type in bits
29-31 and id in bits 0-28; the corpus keeps the original generic talent
mediation.

## 6. Persistence

- Engine text save: `alignment`, `force_points`, `max_force_points`, plus one
  `effect=` line per live `ActiveEffect` and `force_suppressed`.
- Original `SAVEGAME.sav`: PC `GoodEvil`, `CurrentForce`, `MaxForcePoints` are
  read and written through the same `Mod_PlayerList` schema; the live effect
  stack uses `EffectList` entries with spell id, label, duration, bonuses,
  shield, force and dispellable flags, plus `ForceSuppressed`. A generated slot
  round-trips alignment 25, FP and an armor effect.
- Feats round-trip through `feat=` lines and the original `FeatList`. Powers
  round-trip through `power=` lines and the original per-level
  `LvlStatList[].KnownList0[].Spell` list (the schema used by the real Jedi
  autosave).

## 7. Evidence

```text
combat force test: jedi_force_points=1
combat force test: force_catalog_44=1
combat force test: feat_catalog_125=1
combat force test: lightning_used=1 / lightning_cost=1 / lightning_damage=1
combat force test: heal_used=1 / armor_used=1 / stun_used=1
combat force test: power_attack_used=1 / power_blast_used=1
combat force test: flurry_used=1 / toughness_learned=1
combat force test: two_weapon_penalty=1 / two_weapon_fighting=1
combat force test: immunity_blocks_power=1 / breach_removes_immunity=1
combat force test: shield_effect=1 / breach_removes_buffs=1
combat force test: suppress_blocks_power=1 / suppress_expires=1
combat force test: script_action_feat=1 / script_action_spell=1
combat force test: alignment_engine_roundtrip=1
combat force test: alignment_original_roundtrip=1
combat force test: PASS
```

Regression: 35/35 CTest, 117/117 modules, 8,666/8,666 VM, 0.0300 parity,
143 Python tests / 7 skips, install 15,380 files unchanged. The generated
`000012 - ENGINE` original slot independently validates with
`kotor_save.py` at 0 errors / 0 warnings.

## 8. Residual

- Exact original formulas/animations for non-combat feats (crafting, skill
  focus, gear feats) and content-specific scripted exceptions.
- Persisting active `ActiveEffect` entries across save/load; the stack is live
  state.
- Cutscenes, animation/skinning, audio/EAX, Bink/lip sync, full `.gui`,
  minigames, localization, mods/Override, KotOR 2 world loading and P6 content.
