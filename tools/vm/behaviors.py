#!/usr/bin/env python3
"""State-derived behavior for the 264 corpus-unused action handlers.

`ActionRuntime` has specific handlers for 42 unused actions.  This module gives
the remaining generic handlers observable, state-derived behavior wherever the
action is a query: object fields, world configuration, party/faction queries,
locations, traps/locks, journal/encounter state, and last-event records.  It
returns ``None`` for actions that are engine effects; those remain mediated and
are recorded by the runtime's event log.

The mapping is deliberately data-driven and uses the action name, so the
behavioral suite can assert each result against a prepared world.
"""
from __future__ import annotations

from collections import Counter

from vm.ncs_vm import OBJECT_INVALID, Value

# action name -> (object attribute, value type, default)
OBJECT_FIELDS = {
    "GetAC": ("ac", "int", 10),
    "GetAlignmentGoodEvil": ("good_evil", "int", 50),
    "GetFortitudeSavingThrow": ("fortitude", "int", 0),
    "GetWillSavingThrow": ("will", "int", 0),
    "GetReflexSavingThrow": ("reflex", "int", 0),
    "GetChallengeRating": ("challenge_rating", "int", 1),
    "GetMovementRate": ("movement_rate", "int", 1),
    "GetWeaponRanged": ("weapon_ranged", "int", 0),
    "GetIdentified": ("identified", "int", 1),
    "GetGoldPieceValue": ("item_value", "int", 0),
    "GetItemACValue": ("item_ac", "int", 0),
    "GetItemHasItemProperty": ("item_props", "int", 0),
    "GetIsTrapped": ("trapped", "int", 0),
    "GetTrapDisarmable": ("trap_disarmable", "int", 0),
    "GetTrapDetectable": ("trap_detectable", "int", 0),
    "GetTrapFlagged": ("trap_flagged", "int", 0),
    "GetTrapOneShot": ("trap_one_shot", "int", 0),
    "GetTrapDisarmDC": ("trap_disarm_dc", "int", 0),
    "GetTrapDetectDC": ("trap_detect_dc", "int", 0),
    "GetTrapKeyTag": ("trap_key_tag", "string", ""),
    "GetLockKeyRequired": ("lock_key_required", "int", 0),
    "GetLockKeyTag": ("lock_key_tag", "string", ""),
    "GetLockLockable": ("lockable", "int", 0),
    "GetLockUnlockDC": ("unlock_dc", "int", 0),
    "GetLockLockDC": ("lock_dc", "int", 0),
    "GetPlaceableIllumination": ("illumination", "int", 0),
    "GetIsPlaceableObjectActionPossible": ("placeable_action_possible", "int", 1),
    "GetHasFeatEffect": ("feat_effect", "int", 0),
    "GetIsListening": ("listening", "int", 0),
    "GetLastAttackType": ("last_attack_type", "int", 0),
    "GetLastAttackMode": ("last_attack_mode", "int", 0),
    "GetLastAssociateCommand": ("last_associate_command", "int", 0),
    "GetLastWeaponUsed": ("last_weapon", "int", 0),
    "GetLastDisarmed": ("last_disarmed", "int", 0),
    "GetLastDisturbed": ("last_disturbed", "int", 0),
    "GetLastLocked": ("last_locked", "int", 0),
    "GetLastUnlocked": ("last_unlocked", "int", 0),
    "GetLastPerceptionHeard": ("last_perception_heard", "int", 0),
    "GetLastPerceptionInaudible": ("last_perception_inaudible", "int", 0),
    "GetLastClosedBy": ("last_closed_by", "int", 0),
    "GetLastTrapDetected": ("last_trap_detected", "int", 0),
    "GetAreaOfEffectCreator": ("aoe_creator", "int", 0),
    "GetLastItemEquipped": ("last_item_equipped", "int", 0),
    "GetSpellCastItem": ("spell_cast_item", "int", 0),
    "GetItemActivatedTarget": ("item_activated_target", "int", 0),
    "GetItemActivatedTargetLocation": ("item_activated_location", "int", 0),
    "GetStartingLocation": ("starting_location", "int", 0),
    "GetLastPlayerDying": ("last_player_dying", "int", 0),
    "GetLastRespawnButtonPresser": ("last_respawn_button_presser", "int", 0),
    "GetClickingObject": ("clicking_object", "int", 0),
    "GetLastConversation": ("last_conversation", "string", ""),
    "GetModuleName": ("module", "string", "end_m01aa"),
    "GetGameDifficulty": ("difficulty", "int", 1),
    "GetDifficultyModifier": ("difficulty_modifier", "int", 0),
    "GetEncounterSpawnsMax": ("encounter_spawns_max", "int", 10),
    "GetEncounterSpawnsCurrent": ("encounter_spawns_current", "int", 0),
    "GetEncounterDifficulty": ("encounter_difficulty", "int", 1),
    "GetMaxStealthXP": ("max_stealth_xp", "int", 100),
    "GetStealthXPEnabled": ("stealth_enabled", "int", 1),
    "GetStealthXPDecrement": ("stealth_decrement", "int", 5),
    "IsMoviePlaying": ("movie_playing", "int", 0),
    "GetMatchedSubstring": ("matched_substring", "string", ""),
    "GetMatchedSubstringsCount": ("matched_count", "int", 0),
    "GetReputation": ("reputation", "int", 50),
    "GetButtonMashCheck": ("button_mash", "int", 0),
    "GetIsPlayableRacialType": ("playable_racial_type", "int", 1),
    "GetPCLevellingUp": ("pc_levelling_up", "int", 0),
    "GetStrRefSoundDuration": ("strref_sound_duration", "float", 0.0),
    "GetTransitionTarget": ("transition_target", "int", 0),
    "GetObjectHeard": ("last_speaker", "int", 0),
    "GetGoingToBeAttackedBy": ("last_attacker", "int", 0),
    "GetJournalQuestExperience": ("journal_xp", "int", 0),
}


def _obj_arg(rt, args, aid):
    spec = rt.specs.get(aid)
    if spec:
        for k, p in enumerate(spec["params"]):
            if p["type"] == "object":
                return rt._o(args, k)
    return OBJECT_INVALID


def _world(rt):
    return rt.world


def _party(rt):
    return [o for o in (_world(rt).get(i) for i in _world(rt).party)
            if o is not None and o.valid]


def _value(kind, value):
    return [Value(kind, value)]


WORLD_ONLY = {
    "GetLastItemEquipped", "GetLastPerceptionHeard",
    "GetLastPerceptionInaudible", "GetLastClosedBy", "GetClickingObject",
    "GetLastDisarmed", "GetLastDisturbed", "GetLastLocked",
    "GetLastUnlocked", "GetLastPlayerDying", "GetStartingLocation",
    "GetLastRespawnButtonPresser", "GetSpellCastItem",
    "GetItemActivatedTargetLocation", "GetItemActivatedTarget",
    "GetLastConversation",
}


def _field(rt, name, args, aid):
    attr, typ, default = OBJECT_FIELDS[name]
    if name == "GetJournalQuestExperience":
        plot = rt._s(args, 0)
        return _value("int", _world(rt).attrs.get("journal_xp", {}).get(plot, default))
    if name in WORLD_ONLY:
        val = _world(rt).attrs.get(attr, default)
        if name in ("GetStartingLocation", "GetItemActivatedTargetLocation"):
            return _value("engine", 0)
        if typ == "string":
            return _value("string", str(val))
        if typ == "float":
            return _value("float", float(val))
        return _value("int", int(val))
    if name == "GetModuleName":
        return _value("string", _world(rt).attrs.get("module", default))
    if name in ("GetGameDifficulty", "GetDifficultyModifier",
                "GetEncounterSpawnsMax", "GetEncounterSpawnsCurrent",
                "GetEncounterDifficulty", "GetMaxStealthXP",
                "GetStealthXPEnabled", "GetStealthXPDecrement",
                "IsMoviePlaying", "GetMatchedSubstring",
                "GetMatchedSubstringsCount", "GetReputation",
                "GetButtonMashCheck", "GetIsPlayableRacialType",
                "GetPCLevellingUp", "GetStrRefSoundDuration"):
        val = _world(rt).attrs.get(attr, default)
        if typ == "float":
            return _value("float", float(val))
        if typ == "string":
            return _value("string", str(val))
        return _value("int", int(val))
    obj = _world(rt).get(_obj_arg(rt, args, aid))
    if obj is None:
        return _value("string", default) if typ == "string" else (
            _value("float", default) if typ == "float" else _value("int", default))
    val = obj.attrs.get(attr, default)
    if typ == "string":
        return _value("string", str(val))
    if typ == "float":
        return _value("float", float(val))
    return _value("int", int(val))


def _faction(rt, name, args):
    party = _party(rt)
    if not party:
        if name in ("GetFactionLeader", "GetFirstFactionMember"):
            return _value("object", OBJECT_INVALID)
        return _value("int", 0)
    if name == "GetFactionWeakestMember":
        return _value("object", min(party, key=lambda o: o.hp).id)
    if name == "GetFactionStrongestMember":
        return _value("object", max(party, key=lambda o: o.hp).id)
    if name == "GetFactionMostDamagedMember":
        return _value("object", min(party, key=lambda o: o.hp).id)
    if name == "GetFactionLeastDamagedMember":
        return _value("object", max(party, key=lambda o: o.hp).id)
    if name == "GetFactionGold":
        return _value("int", sum(int(o.attrs.get("gold", 0)) for o in party))
    if name == "GetFactionAverageGoodEvilAlignment":
        return _value("int", sum(int(o.attrs.get("good_evil", 50)) for o in party) // len(party))
    if name == "GetFactionAverageLevel":
        return _value("int", sum(int(o.attrs.get("level", 1)) for o in party) // len(party))
    if name == "GetFactionAverageXP":
        return _value("int", sum(int(o.attrs.get("xp", 0)) for o in party) // len(party))
    if name == "GetFactionMostFrequentClass":
        counts = Counter(int(o.attrs.get("class_id", 0)) for o in party)
        return _value("int", counts.most_common(1)[0][0])
    if name == "GetFactionWorstAC":
        return _value("int", min(int(o.attrs.get("ac", 10)) for o in party))
    if name == "GetFactionBestAC":
        return _value("int", max(int(o.attrs.get("ac", 10)) for o in party))
    if name == "GetFactionLeader":
        return _value("object", party[0].id)
    if name == "GetFirstFactionMember":
        _world(rt).set_cursor(("faction",), 0)
        return _value("object", party[0].id)
    if name == "GetNextFactionMember":
        pos = _world(rt).cursor(("faction",))
        _world(rt).set_cursor(("faction",), pos + 1)
        return _value("object", party[pos].id if pos < len(party) else OBJECT_INVALID)
    return None


def _distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def _location(rt, args, idx):
    return [float(x.as_float()) for x in rt._vec(args, idx)]


def _special(rt, name, args, aid):
    world = _world(rt)
    if name == "GetStringUpperCase":
        return _value("string", rt._s(args, 0).upper())
    if name == "GetStringLowerCase":
        return _value("string", rt._s(args, 0).lower())
    if name in ("GetIsDay", "GetIsNight", "GetIsDawn", "GetIsDusk"):
        hour = world.time % 24.0
        if name == "GetIsDay":
            return _value("int", 1 if 6 <= hour < 18 else 0)
        if name == "GetIsNight":
            return _value("int", 1 if hour < 6 or hour >= 18 else 0)
        if name == "GetIsDawn":
            return _value("int", 1 if 5 <= hour < 7 else 0)
        return _value("int", 1 if 17 <= hour < 19 else 0)
    if name in ("GetPlanetSelectable", "GetPlanetAvailable"):
        planet = rt._i(args, 0)
        return _value("int", 1 if world.attrs.get("planet_flags", {}).get(planet) else 0)
    if name in ("GetDistanceBetweenLocations", "GetDistanceBetweenLocations2D"):
        a = _location(rt, args, 0)
        b = _location(rt, args, 1)
        if name.endswith("2D"):
            a[2] = b[2] = 0.0
        return _value("float", _distance(a, b))
    if name == "GetFacingFromLocation":
        return _value("float", 0.0)
    if name in ("GetNearestCreatureToLocation", "GetNearestObjectToLocation"):
        target = _location(rt, args, 2 if name.startswith("GetNearestCreature") else 1)
        candidates = [o for o in world.objects.values() if o.valid]
        if not candidates:
            return _value("object", OBJECT_INVALID)
        best = min(candidates, key=lambda o: _distance(o.position, target))
        return _value("object", best.id)
    if name == "GetNearestTrapToObject":
        candidates = [o for o in world.objects.values()
                      if o.valid and o.attrs.get("trapped")]
        if not candidates:
            return _value("object", OBJECT_INVALID)
        return _value("object", candidates[0].id)
    if name == "GetEffectDurationType" or name == "GetEffectSubType":
        return _value("int", 0)
    if name == "GetEffectCreator":
        return _value("object", OBJECT_INVALID)
    if name == "GetGoldPieceValue":
        return _field(rt, name, args, aid)
    if name in ("GetFirstAttacker", "GetNextAttacker"):
        attackers = [o.id for o in world.objects.values()
                     if o.attrs.get("attacker")]
        if name == "GetFirstAttacker":
            world.set_cursor(("attackers",), 0)
            return _value("object", attackers[0] if attackers else OBJECT_INVALID)
        pos = world.cursor(("attackers",))
        world.set_cursor(("attackers",), pos + 1)
        return _value("object", attackers[pos] if pos < len(attackers) else OBJECT_INVALID)
    return _faction(rt, name, args)


def resolve(rt, name: str, args, aid: int):
    """Return state-derived cells, or None for a mediated engine effect."""
    if name in OBJECT_FIELDS:
        return _field(rt, name, args, aid)
    return _special(rt, name, args, aid)
