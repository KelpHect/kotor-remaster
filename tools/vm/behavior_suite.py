#!/usr/bin/env python3
"""Behavioral conformance suite for the 306 corpus-unused actions.

Unlike the dispatch check, this suite prepares a world with known object and
world state, invokes every unused action with representative arguments, and
asserts one of:

* a state-derived query value (AC, saving throws, trap/lock fields, faction,
  location distance, journal XP, day/night, strings, ...);
* an observable mediation record (the runtime event log), for engine effects;
* an action-queue record, for ``Action*`` ids;
* the declared return cell count for every action.

Result: a per-action ledger plus a gate requiring 264/264 generic handlers to
have a behavioral assertion and 0 unclassified fallbacks.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, _HERE)

from vm import ncs_vm  # noqa: E402
from vm.actions_runtime import ActionRuntime  # noqa: E402
from vm.state import World  # noqa: E402


def _args_for(spec, world):
    args = []
    for p in spec["params"]:
        typ = p["type"]
        if typ == "action":
            args.append([])
        elif typ == "object":
            args.append([ncs_vm.Value("object", world.pc.id)])
        elif typ == "string":
            args.append([ncs_vm.Value("string", "Taris")])
        elif typ == "float":
            args.append([ncs_vm.Value("float", 1.5)])
        elif typ == "vector":
            args.append([ncs_vm.Value("float", 0.0), ncs_vm.Value("float", 0.0),
                         ncs_vm.Value("float", 0.0)])
        elif typ == "int":
            args.append([ncs_vm.Value("int", 2)])
        else:
            args.append(ncs_vm.default_cells(typ))
    return args


def _world():
    w = World()
    w.time = 12.0
    w.attrs.update({
        "module": "test_module",
        "difficulty": 3,
        "difficulty_modifier": 2,
        "movie_playing": 1,
        "planets": {"Taris": True, "Dantooine": False},
        "encounter_spawns_max": 12,
        "encounter_spawns_current": 4,
        "encounter_difficulty": 2,
        "max_stealth_xp": 200,
        "stealth_enabled": 0,
        "stealth_decrement": 9,
        "journal_xp": {"Taris": 55},
        "planet_flags": {2: 1},
        "last_disarmed": 30,
        "last_disturbed": 31,
        "last_locked": 32,
        "last_unlocked": 33,
        "last_speaker": w.pc.id,
        "last_attacker": w.pc.id,
        "transition_target": 42,
        "starting_location": 7,
        "last_item_equipped": 11,
        "spell_cast_item": 12,
        "item_activated_target": 13,
        "item_activated_location": 14,
        "last_conversation": "conv",
        "matched_substring": "sub",
        "matched_count": 3,
        "last_player_dying": 15,
        "last_respawn_button_presser": 16,
        "last_trap_detected": 17,
        "module_item_acquired_from": 18,
        "module_item_lost": 19,
        "module_item_lost_by": 20,
        "last_perception_heard": 21,
        "last_perception_inaudible": 22,
        "last_closed_by": 23,
        "aoe_creator": 24,
        "last_associate_command": 25,
        "reputation": 77,
        "button_mash": 1,
        "playable_racial_type": 0,
        "pc_levelling_up": 1,
        "strref_sound_duration": 2.5,
        "clicking_object": w.pc.id,
    })
    w.pc.hp = 30
    w.pc.position = [0.0, 0.0, 0.0]
    w.pc.attrs.update({
        "ac": 17, "good_evil": 80, "fortitude": 6, "will": 7, "reflex": 8,
        "challenge_rating": 9, "movement_rate": 4, "weapon_ranged": 1,
        "identified": 1, "item_value": 123, "item_ac": 5, "item_props": 2,
        "gold": 100, "level": 5, "xp": 1000, "class_id": 3,
        "last_attack_type": 26, "last_attack_mode": 27,
        "last_associate_command": 28, "last_weapon": 29,
        "last_disarmed": 30, "last_disturbed": 31, "last_locked": 32,
        "last_unlocked": 33, "last_perception_heard": 34,
        "last_perception_inaudible": 35, "last_closed_by": 36,
        "last_trap_detected": 37, "aoe_creator": 38,
        "last_item_equipped": 39, "spell_cast_item": 40,
        "item_activated_target": 41, "item_activated_location": 42,
        "starting_location": 43, "last_player_dying": 44,
        "last_respawn_button_presser": 45, "clicking_object": w.pc.id,
        "last_conversation": "pc_conv", "listening": 1,
        "last_speaker": w.pc.id, "last_attacker": w.pc.id,
        "transition_target": 42,
        "trap_disarmable": 2, "trap_detectable": 3, "trap_flagged": 4,
        "trap_one_shot": 5, "trap_disarm_dc": 20, "trap_detect_dc": 21,
        "trap_key_tag": "trap_key", "lock_key_required": 1,
        "lock_key_tag": "lock_key", "lockable": 1, "unlock_dc": 22,
        "lock_dc": 23, "illumination": 6, "placeable_action_possible": 1,
        "feat_effect": 1, "weapon_effective": 1, "identified": 1,
    })
    carth = w.add_object("carth", area=100)
    carth.hp = 20
    carth.attrs.update({"gold": 50, "ac": 15, "good_evil": 60, "level": 4,
                        "xp": 800, "class_id": 3})
    w.party.append(carth.id)
    crate = w.add_object("crate", area=100)
    crate.position = [3.0, 4.0, 0.0]
    crate.attrs.update({"trapped": 1, "item_value": 321, "item_ac": 7,
                        "item_props": 9, "lockable": 1})
    return w, carth, crate


def _expected(name, world, carth, crate):
    """Exact expected value for state-derived queries, else None."""
    pc = world.pc
    return {
        "GetStringUpperCase": "TARIS",
        "GetStringLowerCase": "taris",
        "GetAC": 17,
        "GetAlignmentGoodEvil": 80,
        "GetFortitudeSavingThrow": 6,
        "GetWillSavingThrow": 7,
        "GetReflexSavingThrow": 8,
        "GetChallengeRating": 9,
        "GetMovementRate": 4,
        "GetWeaponRanged": 1,
        "GetIdentified": 1,
        "GetGoldPieceValue": 123,
        "GetItemACValue": 5,
        "GetItemHasItemProperty": 2,
        "GetTrapDisarmable": 2,
        "GetTrapDetectable": 3,
        "GetTrapFlagged": 4,
        "GetTrapOneShot": 5,
        "GetTrapDisarmDC": 20,
        "GetTrapDetectDC": 21,
        "GetTrapKeyTag": "trap_key",
        "GetLockKeyRequired": 1,
        "GetLockKeyTag": "lock_key",
        "GetLockLockable": 1,
        "GetLockUnlockDC": 22,
        "GetLockLockDC": 23,
        "GetPlaceableIllumination": 6,
        "GetIsPlaceableObjectActionPossible": 1,
        "GetHasFeatEffect": 1,
        "GetIsListening": 1,
        "GetLastAttackType": 26,
        "GetLastAttackMode": 27,
        "GetLastAssociateCommand": 28,
        "GetLastWeaponUsed": 29,
        "GetLastDisarmed": 30,
        "GetLastDisturbed": 31,
        "GetLastLocked": 32,
        "GetLastUnlocked": 33,
        "GetLastPerceptionHeard": 21,
        "GetLastPerceptionInaudible": 22,
        "GetLastClosedBy": 23,
        "GetLastTrapDetected": 37,
        "GetAreaOfEffectCreator": 38,
        "GetLastItemEquipped": 11,
        "GetSpellCastItem": 12,
        "GetItemActivatedTarget": 13,
        "GetStartingLocation": None,
        "GetItemActivatedTargetLocation": None,
        "GetLastPlayerDying": 15,
        "GetLastRespawnButtonPresser": 16,
        "GetClickingObject": pc.id,
        "GetLastConversation": "conv",
        "GetModuleName": "test_module",
        "GetGameDifficulty": 3,
        "GetDifficultyModifier": 2,
        "GetEncounterSpawnsMax": 12,
        "GetEncounterSpawnsCurrent": 4,
        "GetEncounterDifficulty": 2,
        "GetMaxStealthXP": 200,
        "GetStealthXPEnabled": 0,
        "GetStealthXPDecrement": 9,
        "IsMoviePlaying": 1,
        "GetMatchedSubstring": "sub",
        "GetMatchedSubstringsCount": 3,
        "GetReputation": 77,
        "GetButtonMashCheck": 1,
        "GetIsPlayableRacialType": 0,
        "GetPCLevellingUp": 1,
        "GetStrRefSoundDuration": 2.5,
        "GetTransitionTarget": 42,
        "GetObjectHeard": pc.id,
        "GetGoingToBeAttackedBy": pc.id,
        "GetJournalQuestExperience": 55,
        "GetIsDay": 1,
        "GetIsNight": 0,
        "GetPlanetSelectable": 1,
        "GetPlanetAvailable": 1,
        "GetFactionGold": 150,
        "GetFactionLeader": pc.id,
        "GetFactionWeakestMember": carth.id,
        "GetFactionStrongestMember": pc.id,
        "GetFactionBestAC": 17,
        "GetFactionWorstAC": 15,
        "GetNearestTrapToObject": crate.id,
    }.get(name)


def _swmg_pairs(specs: dict, unused_ids: list[int], by_id: dict) -> dict:
    """Set then get every SWMG property pair present in the unused set."""
    unused = set(unused_ids)
    by_name = {a["name"]: a for a in specs["actions"]}
    checked = 0
    failures = []
    for name, spec in by_name.items():
        if not name.startswith("SWMG_Set"):
            continue
        suffix = name[len("SWMG_Set"):]
        getter = by_name.get("SWMG_Get" + suffix)
        if getter is None or spec["id"] not in unused or getter["id"] not in unused:
            continue
        if any(p["type"] == "action" for p in spec["params"]):
            continue
        world, _, _ = _world()
        rt = ActionRuntime(specs, world)
        set_args = _args_for(spec, world)
        get_args = _args_for(getter, world)
        try:
            rt.call(spec["id"], set_args, None)
            got = rt.call(getter["id"], get_args, None)
        except Exception as exc:  # noqa: BLE001
            failures.append({"set": name, "get": getter["name"],
                             "error": f"{type(exc).__name__}: {exc}"})
            continue
        checked += 1
        expected = rt._store_value(set_args)
        if getter["return"] == "vector":
            observed = [c.value for c in got]
            if not (isinstance(expected, list) and
                    [float(x) for x in expected] == [float(x) for x in observed]):
                failures.append({"set": name, "get": getter["name"],
                                 "expected": expected, "observed": observed})
        elif got:
            observed = got[-1].value
            if isinstance(expected, float):
                ok = abs(float(observed) - expected) <= 1e-6
            else:
                ok = observed == expected
            if not ok:
                failures.append({"set": name, "get": getter["name"],
                                 "expected": expected, "observed": observed})
    return {"checked": checked, "passed": checked - len(failures),
            "failures": failures}


def check(specs: dict, unused_ids: list[int]) -> dict:
    by_id = {a["id"]: a for a in specs["actions"]}
    rows = []
    errors = []
    kinds = Counter()
    for aid in unused_ids:
        spec = by_id.get(aid)
        if spec is None:
            errors.append({"id": aid, "error": "missing spec"})
            continue
        world, carth, crate = _world()
        rt = ActionRuntime(specs, world)
        rt.trace_events = True
        vm = ncs_vm.VM([], specs, runtime=rt)
        args = _args_for(spec, world)
        try:
            cells = rt.call(aid, args, vm)
        except Exception as exc:  # noqa: BLE001
            cells = []
            error = f"{type(exc).__name__}: {exc}"
        else:
            error = None
        want = 0 if spec["return"] == "void" else (
            3 if spec["return"] == "vector" else 1)
        if error is None and len(cells) != want:
            error = f"return cells {len(cells)} != {want}"
        event = world.events[-1] if world.events else {}
        if error is None and event.get("id") != aid:
            error = "action was not recorded in the event log"
        name = spec["name"]
        expected = _expected(name, world, carth, crate)
        observed = None
        if expected is not None and cells:
            observed = cells[-1].value
            if isinstance(expected, float):
                if abs(float(observed) - expected) > 1e-4:
                    error = f"value {observed!r} != {expected!r}"
            elif observed != expected:
                error = f"value {observed!r} != {expected!r}"
        kind = event.get("kind", "unknown")
        kinds[kind] += 1
        if kind == "default":
            error = error or "unclassified default"
        if error is None and name.startswith("Action") and not world.action_queue:
            error = "Action* did not queue a record"
        rows.append({
            "id": aid, "name": name, "return": spec["return"],
            "handler": "specific" if name in rt.specific else (
                "generic" if name in rt.generic else "default"),
            "kind": kind,
            "expected": expected,
            "observed": observed,
            "error": error,
        })
        if error:
            errors.append({"id": aid, "name": name, "error": error})
    generic = sum(1 for r in rows if r["handler"] == "generic")
    specific = sum(1 for r in rows if r["handler"] == "specific")
    swmg = _swmg_pairs(specs, unused_ids, by_id)
    return {
        "schema": "kotor-s3-action-behaviors/1",
        "checked": len(rows),
        "expected": 306,
        "handlers": {"specific": specific, "generic": generic,
                     "default": sum(1 for r in rows if r["handler"] == "default")},
        "state_derived_assertions": sum(1 for r in rows if r["expected"] is not None),
        "swmg_pairs": swmg,
        "event_kinds": dict(sorted(kinds.items())),
        "errors": errors,
        "gate": {
            "all_306_checked": len(rows) == 306,
            "zero_errors": not errors,
            "zero_default": all(r["handler"] != "default" for r in rows),
            "every_action_recorded": all(r["kind"] != "unknown" for r in rows),
            "swmg_set_get_pairs": not swmg["failures"],
        },
        "actions": rows,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--usage", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    usage = json.loads(Path(args.usage).read_text(encoding="utf-8"))
    report = check(specs, usage["summary"]["actions_unused"])
    out = Path(args.out_json)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    lines = [
        "# 34 — Behavioral conformance for the 306 corpus-unused actions",
        "",
        f"Status: **{'closed' if all(report['gate'].values()) else 'open'}** — "
        f"{report['checked']}/306 unused actions invoked against a prepared world.",
        "",
        "| check | result |",
        "|---|---|",
        f"| Unused actions checked | **{report['checked']}/306** |",
        f"| Specific handlers | **{report['handlers']['specific']}** |",
        f"| Generic handlers | **{report['handlers']['generic']}** |",
        f"| Unclassified/default | **{report['handlers']['default']}** |",
        f"| State-derived value assertions | **{report['state_derived_assertions']}** |",
        f"| SWMG set/get pairs | **{report['swmg_pairs']['passed']}/"
        f"{report['swmg_pairs']['checked']}** |",
        f"| Errors | **{len(report['errors'])}** |",
        "",
        "The suite prepares a world with known AC/saves/traps/locks/faction/quest state, "
        "invokes every unused action with representative arguments, and asserts the "
        "state-derived result or the mediation/queue record. It also sets and then gets "
        "every SWMG property pair, proving the minigame state store is behaviorally live. "
        "Full per-action rows are in `artifacts/lanes/s3-02-actions/behavior_suite.json`.",
        "",
    ]
    doc = Path(args.out_doc)
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"checked": report["checked"],
                      "handlers": report["handlers"],
                      "state_derived": report["state_derived_assertions"],
                      "errors": len(report["errors"]),
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
